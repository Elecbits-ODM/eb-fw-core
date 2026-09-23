#include "stpm34.h"
#include "STPM3X_define.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stpm34";

static inline void stpm34_set_auto_latch(stpm34_t *dev, bool en) { dev->auto_latch = en; }
static inline void stpm34_set_crc_enabled(stpm34_t *dev, bool en) { dev->crc_enabled = en; }

/**
 * @brief Drive the EN/RST line to the requested level, via whichever
 *        mechanism is configured for this device: a caller-supplied
 *        callback (see stpm34_set_reset_callback()) if one is registered,
 *        otherwise a direct GPIO on dev->pin_enrst. If neither is set
 *        (pin_enrst == GPIO_NUM_NC and no callback), this is a no-op -
 *        some boards don't expose a software-controllable reset line.
 */
static inline void stpm34_drive_enrst(stpm34_t *dev, bool level)
{
    if (dev->reset_fn != NULL)
    {
        dev->reset_fn(level, dev->reset_ctx);
    }
    else if (dev->pin_enrst != GPIO_NUM_NC)
    {
        gpio_set_level(dev->pin_enrst, level ? 1 : 0);
    }
}

static bool stpm34_buf_looks_valid(const uint8_t *b, size_t n)
{
    bool all_ff = true, all_00 = true;
    for (size_t i = 0; i < n; i++)
    {
        if (b[i] != 0xFF)
            all_ff = false;
        if (b[i] != 0x00)
            all_00 = false;
    }
    return !(all_ff || all_00);
}

/* Global register bitfield unions  */
DSP_CR100bits_t DSP_CR100bits;
DSP_CR101bits_t DSP_CR101bits;
DSP_CR200bits_t DSP_CR200bits;
DSP_CR201bits_t DSP_CR201bits;
DSP_CR400bits_t DSP_CR400bits;
US1_REG100bits_t US1_REG100bits;

DFE_CR101bits_t DFE_CR101bits;
DFE_CR201bits_t DFE_CR201bits;
DSP_CR301bits_t DSP_CR301bits;
DSP_CR500bits_t DSP_CR500bits;

DSP_SR100bits_t DSP_SR100bits;
DSP_SR101bits_t DSP_SR101bits;

static inline void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void delay_us(uint32_t us) { esp_rom_delay_us(us); }

static inline void cs_low(const stpm34_t *dev) { gpio_set_level(dev->pin_cs, 0); }
static inline void cs_high(const stpm34_t *dev) { gpio_set_level(dev->pin_cs, 1); }

static esp_err_t spi_xfer(stpm34_t *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t ret = spi_device_polling_transmit(dev->spi, &t);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_xfer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* CRC (ported from Arduino) */
static void stpm34_crc8_calc(stpm34_t *dev, uint8_t data)
{
    uint8_t idx = 0;
    while (idx < 8)
    {
        uint8_t tmp = data ^ dev->crc_checksum;
        dev->crc_checksum <<= 1;
        if (tmp & 0x80)
        {
            dev->crc_checksum ^= CRC_8;
        }
        data <<= 1;
        idx++;
    }
}

static uint8_t stpm34_calc_crc8(stpm34_t *dev, uint8_t *buf)
{
    dev->crc_checksum = 0x00;
    for (uint8_t i = 0; i < STPM3x_FRAME_LEN - 1; i++)
    {
        stpm34_crc8_calc(dev, buf[i]);
    }
    return dev->crc_checksum;
}

static void stpm34_send_frame(stpm34_t *dev, uint8_t readAdd, uint8_t writeAdd, uint8_t dataLSB, uint8_t dataMSB)
{
    uint8_t tx[4] = {readAdd, writeAdd, dataLSB, dataMSB};
    cs_low(dev);
    (void)spi_xfer(dev, tx, NULL, sizeof(tx));
    cs_high(dev);
}

static void stpm34_send_frame_crc(stpm34_t *dev, uint8_t readAdd, uint8_t writeAdd, uint8_t dataLSB, uint8_t dataMSB)
{
    uint8_t tx[STPM3x_FRAME_LEN] = {0};
    tx[0] = readAdd;
    tx[1] = writeAdd;
    tx[2] = dataLSB;
    tx[3] = dataMSB;
    tx[4] = stpm34_calc_crc8(dev, tx);

    cs_low(dev);
    (void)spi_xfer(dev, tx, NULL, sizeof(tx));
    cs_high(dev);
}

/* Pipelined read with required 5µs inter‑transaction delay */
static void stpm34_read_frame(stpm34_t *dev, uint8_t address, uint8_t *buffer4)
{
    uint8_t tx1[4] = {address, 0xFF, 0xFF, 0xFF};
    uint8_t rx1[4] = {0};

    uint8_t tx2[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t rx2[4] = {0};

    // 1) Set pointer
    cs_low(dev);
    (void)spi_xfer(dev, tx1, rx1, sizeof(tx1));
    cs_high(dev);

    delay_us(5); // mandatory inter-transaction gap (datasheet)

    // 2) Read data
    cs_low(dev);
    (void)spi_xfer(dev, tx2, rx2, sizeof(tx2));
    cs_high(dev);

    buffer4[0] = rx2[0];
    buffer4[1] = rx2[1];
    buffer4[2] = rx2[2];
    buffer4[3] = rx2[3];

    if (dev->debug)
    {
        ESP_LOGI(TAG, "read_frame(0x%02X): %02X %02X %02X %02X",
                 address, buffer4[0], buffer4[1], buffer4[2], buffer4[3]);
    }
}

/* Math helpers */
static inline uint16_t buffer0to14(uint8_t *buffer) { return ((buffer[1] & 0x7f) << 8) | buffer[0]; }
static inline int32_t buffer0to32(uint8_t *buffer) { return (int32_t)((((uint32_t)buffer[3] << 24) | ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[1] << 8) | buffer[0])); }

/* Signed 29‑bit to 32‑bit conversion for power registers (bits 28:0, bit28 = sign) */
static inline int32_t buffer0to28_signed(uint8_t *buffer)
{
    int32_t val = (int32_t)(((uint32_t)buffer[3] << 24) | ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[1] << 8) | buffer[0]);
    if (val & 0x10000000)
    {                      // bit28 is sign
        val |= 0xE0000000; // sign‑extend bits 31‑29
    }
    else
    {
        val &= 0x0FFFFFFF;
    }
    return val;
}

static inline uint32_t buffer15to32(uint8_t *buffer) { return ((((uint32_t)buffer[3] << 16) | ((uint32_t)buffer[2] << 8) | buffer[1]) >> 7); }

/* ---------- CONVERSION FACTORS ---------- */
// RMS voltage (15-bit) → volts
static inline float calcRmsVolt(uint16_t value)
{
    return (float)value * 0.035484044f;
}

// RMS current (17-bit) → amperes
static inline float calcRmsCurrent(uint32_t value)
{
    return (float)value * 0.0002143f;
}

// Power (29-bit signed) → watts (or var, VA)
static inline float calcPower(int32_t value)
{
    return (float)value * 0.0001217f;
}
/* -------------------------------------------------- */

/* Software latch using SPI –  delay after write */
static void stpm34_latch(stpm34_t *dev)
{
    // Read current register
    stpm34_read_frame(dev, 0x05, dev->read_buffer);

    DSP_CR301bits.LSB = dev->read_buffer[0];
    DSP_CR301bits.MSB = dev->read_buffer[1];

    // STEP 1: clear latch bits
    DSP_CR301bits.SW_Latch1 = 0;
    DSP_CR301bits.SW_Latch2 = 0;

    stpm34_send_frame(dev, 0xFF, 0x05,
                      DSP_CR301bits.LSB,
                      DSP_CR301bits.MSB);

    delay_us(5);

    // STEP 2: set latch bits (rising edge)
    DSP_CR301bits.SW_Latch1 = 1;
    DSP_CR301bits.SW_Latch2 = 1;

    stpm34_send_frame(dev, 0xFF, 0x05,
                      DSP_CR301bits.LSB,
                      DSP_CR301bits.MSB);

    delay_us(10); // give DSP time
}

/* --- Helpers ported 1:1 -- */

static void stpm34_crc_control(stpm34_t *dev, bool enabled)
{
    if (dev->crc_enabled == enabled)
        return;

    stpm34_read_frame(dev, 0x24, dev->read_buffer);
    US1_REG100bits.LSB = dev->read_buffer[0];
    US1_REG100bits.MSB = dev->read_buffer[1];
    US1_REG100bits.CRC_EN = enabled ? 1 : 0;

    stpm34_send_frame_crc(dev, 0x24, 0x24, US1_REG100bits.LSB, US1_REG100bits.MSB);
    dev->crc_enabled = enabled;
}

static void stpm34_auto_latch_control(stpm34_t *dev, bool enabled)
{
    if (dev->auto_latch == enabled)
        return;

    stpm34_read_frame(dev, 0x05, dev->read_buffer);
    DSP_CR301bits.LSB = dev->read_buffer[0];
    DSP_CR301bits.MSB = dev->read_buffer[1];

    if (enabled)
    {
        DSP_CR301bits.SWAuto_Latch = 1;
        DSP_CR301bits.SW_Latch1 = 0;
        DSP_CR301bits.SW_Latch2 = 0;
    }
    else
    {
        DSP_CR301bits.SWAuto_Latch = 0;
        DSP_CR301bits.SW_Latch1 = 1;
        DSP_CR301bits.SW_Latch2 = 1;
    }

    if (dev->crc_enabled)
    {
        stpm34_send_frame_crc(dev, 0x05, 0x05, DSP_CR301bits.LSB, DSP_CR301bits.MSB);
    }
    else
    {
        stpm34_send_frame(dev, 0x05, 0x05, DSP_CR301bits.LSB, DSP_CR301bits.MSB);
    }

    dev->auto_latch = enabled;
}

static void stpm34_set_current_gain(stpm34_t *dev, uint8_t channel, stpm_gain_t gain)
{
    uint8_t readAdd = 0x00;
    uint8_t writeAdd = 0x00;

    if (channel == 1)
    {
        stpm34_read_frame(dev, 0x18, dev->read_buffer);
        DFE_CR101bits.LSB = dev->read_buffer[2];
        DFE_CR101bits.MSB = dev->read_buffer[3];
        DFE_CR101bits.GAIN1 = (uint8_t)gain;
        writeAdd = 0x19;
        if (dev->crc_enabled)
            stpm34_send_frame_crc(dev, readAdd, writeAdd, DFE_CR101bits.LSB, DFE_CR101bits.MSB);
        else
            stpm34_send_frame(dev, readAdd, writeAdd, DFE_CR101bits.LSB, DFE_CR101bits.MSB);
        dev->gain1 = gain;
    }
    else if (channel == 2)
    {
        stpm34_read_frame(dev, 0x1A, dev->read_buffer);
        DFE_CR201bits.LSB = dev->read_buffer[0];
        DFE_CR201bits.MSB = dev->read_buffer[1];
        DFE_CR201bits.GAIN1 = (uint8_t)gain;
        writeAdd = 0x1B;
        if (dev->crc_enabled)
            stpm34_send_frame_crc(dev, readAdd, writeAdd, DFE_CR201bits.LSB, DFE_CR201bits.MSB);
        else
            stpm34_send_frame(dev, readAdd, writeAdd, DFE_CR201bits.LSB, DFE_CR201bits.MSB);
        dev->gain2 = gain;
    }
}

static bool stpm34_check_gain(stpm34_t *dev, uint8_t channel, const uint8_t *buf)
{
    if (channel == 1)
    {
        return (DFE_CR101bits.LSB == buf[2]) &&
               (DFE_CR101bits.MSB == buf[3]) &&
               (DFE_CR101bits.GAIN1 == (uint8_t)dev->gain1);
    }
    else if (channel == 2)
    {
        return (DFE_CR201bits.LSB == buf[2]) &&
               (DFE_CR201bits.MSB == buf[3]) &&
               (DFE_CR201bits.GAIN1 == (uint8_t)dev->gain2);
    }
    return false;
}

/* Register init – with longer software reset and delays */
static bool stpm34_init_regs(stpm34_t *dev)
{
    uint8_t readAdd, writeAdd, dataLSB, dataMSB;

    // Software reset (ensures DSP is properly initialized)
    stpm34_read_frame(dev, 0x05, dev->read_buffer);
    DSP_CR301bits.LSB = dev->read_buffer[0];
    DSP_CR301bits.MSB = dev->read_buffer[1];
    DSP_CR301bits.SW_Reset = 1;
    stpm34_send_frame_crc(dev, 0xFF, 0x05, DSP_CR301bits.LSB, DSP_CR301bits.MSB);
    delay_ms(20); // increased from 10 ms to be safe

    // set Voltage Reference (CH1)
    DSP_CR100bits.ENVREF1 = 1;
    DSP_CR100bits.TC1 = 0x02;
    readAdd = 0x00;
    writeAdd = 0x00;
    dataLSB = DSP_CR100bits.LSB;
    dataMSB = DSP_CR100bits.MSB;
    stpm34_send_frame_crc(dev, readAdd, writeAdd, dataLSB, dataMSB);

    // DSP_CR101 (CH1 filters)
    DSP_CR101bits.BHPFV1 = 0;
    DSP_CR101bits.BHPFC1 = 0;
    DSP_CR101bits.BLPFV1 = 1;
    DSP_CR101bits.BLPFC1 = 1;
    DSP_CR101bits.LPW1 = 0x04;
    readAdd = 0x00;
    writeAdd = 0x01;
    dataLSB = DSP_CR101bits.LSB;
    dataMSB = DSP_CR101bits.MSB;
    stpm34_send_frame_crc(dev, readAdd, writeAdd, dataLSB, dataMSB);

    // set Voltage Reference (CH2)
    DSP_CR200bits.ENVREF2 = 1;
    DSP_CR200bits.TC2 = 0x02;
    readAdd = 0x01;
    writeAdd = 0x02;
    dataLSB = DSP_CR200bits.LSB;
    dataMSB = DSP_CR200bits.MSB;
    stpm34_send_frame_crc(dev, readAdd, writeAdd, dataLSB, dataMSB);

    // DSP_CR201 (CH2 filters)
    DSP_CR201bits.BHPFV2 = 0;
    DSP_CR201bits.BHPFC2 = 0;
    DSP_CR201bits.BLPFV2 = 1;
    DSP_CR201bits.BLPFC2 = 1;
    DSP_CR201bits.LPW2 = 0x04;
    readAdd = 0x02;
    writeAdd = 0x03;
    dataLSB = DSP_CR201bits.LSB;
    dataMSB = DSP_CR201bits.MSB;
    stpm34_send_frame_crc(dev, readAdd, writeAdd, dataLSB, dataMSB);

    // Current gain = 16x on both channels
    stpm34_set_current_gain(dev, 1, STPM_GAIN_16X);
    stpm34_set_current_gain(dev, 2, STPM_GAIN_16X);

    // Read back gains to verify STPM is alive
    stpm34_read_frame(dev, 0x18, dev->read_buffer);
    bool success = stpm34_check_gain(dev, 1, dev->read_buffer);

    stpm34_read_frame(dev, 0x1A, dev->read_buffer);
    success = success && stpm34_check_gain(dev, 2, dev->read_buffer);

    // Enable AUTO_LATCH (DSP latches every 128us, no per-read latch needed),
    // then disable CRC
    stpm34_auto_latch_control(dev, true);
    stpm34_crc_control(dev, false);

    return success;
}

void stpm34_init_handle(stpm34_t *dev,
                        gpio_num_t enrst,
                        gpio_num_t cs,
                        gpio_num_t syn,
                        int net_freq_hz)
{
    memset(dev, 0, sizeof(*dev));
    dev->pin_enrst = enrst;
    dev->pin_cs = cs;
    dev->pin_syn = syn;
    dev->auto_latch = false;
    dev->crc_enabled = true;
    dev->net_freq_hz = net_freq_hz;
    dev->gain1 = STPM_GAIN_2X;
    dev->gain2 = STPM_GAIN_2X;
    dev->debug = true;
    for (int i = 0; i < 3; i++)
    {
        dev->calibration[i][STPM34_CAL_V] = 1.0f;
        dev->calibration[i][STPM34_CAL_I] = 1.0f;
    }
    // dev->reset_fn / dev->reset_ctx already zeroed by the memset above,
    // so the default is "drive pin_enrst directly as a GPIO".
}

void stpm34_set_reset_callback(stpm34_t *dev, stpm34_reset_fn_t fn, void *ctx)
{
    dev->reset_fn = fn;
    dev->reset_ctx = ctx;
}

esp_err_t stpm34_begin(stpm34_t *dev,
                       spi_host_device_t host,
                       gpio_num_t sck, gpio_num_t miso, gpio_num_t mosi,
                       int clock_hz)
{
    dev->host = host;
    dev->spi_clock_hz = clock_hz; // store for reinit

    gpio_set_direction(dev->pin_cs, GPIO_MODE_OUTPUT);
    cs_high(dev);

    if (dev->pin_syn != GPIO_NUM_NC)
    {
        gpio_set_direction(dev->pin_syn, GPIO_MODE_OUTPUT);
        gpio_set_level(dev->pin_syn, 1);
    }

    if (dev->reset_fn == NULL && dev->pin_enrst != GPIO_NUM_NC)
    {
        gpio_set_direction(dev->pin_enrst, GPIO_MODE_OUTPUT);
    }
    stpm34_drive_enrst(dev, true);

    spi_bus_config_t buscfg = {
        .sclk_io_num = sck,
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0};

    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = clock_hz,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 4,
    };

    err = spi_bus_add_device(host, &devcfg, &dev->spi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t stpm34_reinit_spi(stpm34_t *dev)
{
    esp_err_t err;

    err = spi_bus_remove_device(dev->spi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = dev->spi_clock_hz,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 4,
    };
    err = spi_bus_add_device(dev->host, &devcfg, &dev->spi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "SPI device re‑initialised at %d Hz", dev->spi_clock_hz);
    return ESP_OK;
}

bool stpm34_chip_init(stpm34_t *dev)
{
    // CS low during reset pulse
    cs_low(dev);
    // delay_ms(50);
    stpm34_drive_enrst(dev, false);
    delay_ms(100); // increased from 35 ms
    stpm34_drive_enrst(dev, true);
    delay_ms(100); // increased from 35 ms
    cs_high(dev);
    delay_ms(10); // extra settling time

    // Toggle SYN 3 times (if present)
    if (dev->pin_syn != GPIO_NUM_NC)
    {
        for (int i = 0; i < 3; i++)
        {
            gpio_set_level(dev->pin_syn, 0);
            delay_ms(2);
            gpio_set_level(dev->pin_syn, 1);
            delay_ms(2);
        }
    }

    // delay_ms(2);
    // cs_low(dev);
    // delay_ms(5);
    // cs_high(dev);

    return stpm34_init_regs(dev);
}

void stpm34_set_calibration(stpm34_t *dev, uint8_t channel, float calV, float calI)
{
    if (channel < 1 || channel > 2)
    {
        ESP_LOGE(TAG, "set_calibration: invalid channel %u", channel);
        return;
    }
    dev->calibration[channel][_V] = calV;
    dev->calibration[channel][_I] = calI;
    if (dev->debug)
    {
        ESP_LOGD(TAG, "Calibration ch%u: V=%.3f I=%.3f", channel, calV, calI);
    }
}

float stpm34_read_rms_voltage(stpm34_t *dev, uint8_t channel)
{

    if (channel == 1)
    {
        dev->address = C1_RMS_Data_Address;
    }
    else if (channel == 2)
    {
        dev->address = C2_RMS_Data_Address;
    }
    else
    {
        return -1;
    }
    if (!dev->auto_latch)
        stpm34_latch(dev);

    stpm34_read_frame(dev, dev->address, dev->read_buffer);
    uint16_t raw = buffer0to14(dev->read_buffer);
    float v = calcRmsVolt(raw) * dev->calibration[channel][_V];
    if (dev->debug)
    {
        ESP_LOGD(TAG, "V(ch%u): raw=0x%04X (%u) -> %.3f V", channel, raw, raw, v);
    }
    return v;
}

float stpm34_read_rms_current(stpm34_t *dev, uint8_t channel)
{

    if (channel == 1)
    {
        dev->address = C1_RMS_Data_Address;
    }
    else if (channel == 2)
    {
        dev->address = C2_RMS_Data_Address;
    }
    else
    {
        return -1;
    }
    if (!dev->auto_latch)
        stpm34_latch(dev);

    stpm34_read_frame(dev, dev->address, dev->read_buffer);
    uint32_t raw = buffer15to32(dev->read_buffer);
    float i = calcRmsCurrent(raw) * dev->calibration[channel][_I];
    if (dev->debug)
    {
        ESP_LOGD(TAG, "I(ch%u): raw=0x%06lX (%lu) -> %.6f A", channel, raw, raw, i);
    }
    return i;
}

float stpm34_read_active_power(stpm34_t *dev, uint8_t channel)
{
    if (channel == 1)
    {
        dev->address = PH1_Active_Power_Address;
    }
    else if (channel == 2)
    {
        dev->address = PH2_Active_Power_Address;
    }
    else
    {
        return -1;
    }
    if (!dev->auto_latch)
        stpm34_latch(dev);

    stpm34_read_frame(dev, dev->address, dev->read_buffer);
    int32_t raw = buffer0to28_signed(dev->read_buffer);
    float p = calcPower(raw) * dev->calibration[channel][_V] * dev->calibration[channel][_I];
    if (dev->debug)
    {
        ESP_LOGD(TAG, "P(ch%u): raw=0x%08lX (%ld) -> %.3f W", channel, (uint32_t)raw, raw, p);
    }
    return p;
}

float stpm34_read_reactive_power(stpm34_t *dev, uint8_t channel)
{

    if (channel == 1)
    {
        dev->address = PH1_Reactive_Power_Address;
    }
    else if (channel == 2)
    {
        dev->address = PH2_Reactive_Power_Address;
    }
    else
    {
        return -1;
    }
    if (!dev->auto_latch)
        stpm34_latch(dev);

    stpm34_read_frame(dev, dev->address, dev->read_buffer);
    int32_t raw = buffer0to28_signed(dev->read_buffer);
    float q = calcPower(raw) * dev->calibration[channel][_V] * dev->calibration[channel][_I];
    if (dev->debug)
    {
        ESP_LOGD(TAG, "Q(ch%u): raw=0x%08lX (%ld) -> %.3f var", channel, (uint32_t)raw, raw, q);
    }
    return q;
}

float stpm34_read_apparent_rms_power(stpm34_t *dev, uint8_t channel)
{

    if (channel == 1)
    {
        dev->address = PH1_Apparent_RMS_Power_Address;
    }
    else if (channel == 2)
    {
        dev->address = PH2_Apparent_RMS_Power_Address;
    }
    else
    {
        return -1;
    }
    if (!dev->auto_latch)
        stpm34_latch(dev);

    stpm34_read_frame(dev, dev->address, dev->read_buffer);
    int32_t raw = buffer0to28_signed(dev->read_buffer);
    float s = calcPower(raw) * dev->calibration[channel][_V] * dev->calibration[channel][_I];
    if (dev->debug)
    {
        ESP_LOGD(TAG, "S(ch%u): raw=0x%08lX (%ld) -> %.3f VA", channel, (uint32_t)raw, raw, s);
    }
    return s;
}

float stpm34_read_power_factor(stpm34_t *dev, uint8_t channel)
{
    float s = stpm34_read_apparent_rms_power(dev, channel);
    if (s == 0.0f)
        return 0.0f;
    float p = stpm34_read_active_power(dev, channel);
    float pf = p / s;
    if (pf > 1.0f)
        pf = 1.0f;
    if (pf < -1.0f)
        pf = -1.0f;
    return pf;
}

void stpm34_read_line_frequency(stpm34_t *dev, float *freq_hz1, float *freq_hz2)
{
    if (!dev->auto_latch)
        stpm34_latch(dev);

    uint8_t buffer[4];
    stpm34_read_frame(dev, Period_Address, buffer);

    uint16_t period1 = (buffer[0] | ((buffer[1] & 0x0F) << 8));
    uint16_t period2 = (buffer[2] | ((buffer[3] & 0x0F) << 8));

    if (period1 > 0)
    {
        *freq_hz1 = 1000000.0f / (period1 * 8.0f);
    }
    else
    {
        *freq_hz1 = 0.0f;
    }

    if (period2 > 0)
    {
        *freq_hz2 = 1000000.0f / (period2 * 8.0f);
    }
    else
    {
        *freq_hz2 = 0.0f;
    }

    if (dev->debug)
    {
        ESP_LOGD(TAG, "Period1=%u (%f Hz), Period2=%u (%f Hz)",
                 period1, *freq_hz1, period2, *freq_hz2);
    }
}

float stpm34_read_phase_angle(stpm34_t *dev, uint8_t channel)
{
    if (channel != 1 && channel != 2)
    {
        ESP_LOGE(TAG, "read_phase_angle: invalid channel %u", channel);
        return -1.0f;
    }

    if (!dev->auto_latch)
        stpm34_latch(dev);

    uint8_t addr = (channel == 1) ? C1PHA_SWC1_TIME_Address : C2PHA_SWC2_TIME_Address;
    uint8_t buffer[4];
    stpm34_read_frame(dev, addr, buffer);

    uint16_t phase_raw = (buffer[2] | ((buffer[3] & 0x0F) << 8));

    float angle = phase_raw * (dev->net_freq_hz * 360.0f * 8e-6f);

    if (dev->debug)
    {
        ESP_LOGD(TAG, "Phase ch%u: raw=0x%04X (%u) -> %.2f°",
                 channel, phase_raw, phase_raw, angle);
    }

    return angle;
}