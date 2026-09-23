#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STPM34_CAL_V 0
#define STPM34_CAL_I 1

#define _V 0
#define _I 1

typedef enum {
    STPM_GAIN_2X      = 0x00,
    STPM_GAIN_4X      = 0x01,
    STPM_GAIN_8X      = 0x02,
    STPM_GAIN_16X     = 0x03
} stpm_gain_t;

/**
 * @brief Optional caller-supplied EN/RST line driver.
 *
 * The STPM34 needs its EN/RST pin toggled to reset the chip. Most boards
 * wire that pin to a plain GPIO, which this driver drives directly via
 * `pin_enrst` (below) - no callback needed. If your board instead drives
 * EN/RST through something that isn't a raw GPIO (an I/O expander pin, a
 * shift register, etc.), register a function of this shape with
 * stpm34_set_reset_callback() and it will be called instead of touching
 * any GPIO.
 *
 * @param level Requested line level: true = high/inactive, false = low/reset asserted.
 * @param ctx   Opaque context pointer, passed through unchanged from stpm34_set_reset_callback().
 */
typedef void (*stpm34_reset_fn_t)(bool level, void *ctx);

typedef struct {
    gpio_num_t pin_enrst;   // Direct GPIO for EN/RST. Set to GPIO_NUM_NC if
                            // EN/RST is driven via reset_fn instead, or if
                            // your board doesn't expose a software reset line at all.
    gpio_num_t pin_cs;
    gpio_num_t pin_syn;

    spi_host_device_t host;
    spi_device_handle_t spi;
    int spi_clock_hz;                   // Store clock for re‑init

    bool auto_latch;
    bool crc_enabled;
    uint8_t crc_checksum;
    uint8_t address;
    int net_freq_hz;

    stpm_gain_t gain1;
    stpm_gain_t gain2;
    float calibration[3][2];   // [channel][V/I]   channel 0=total, 1=ph1, 2=ph2

    uint8_t read_buffer[10];

    bool debug;

    stpm34_reset_fn_t reset_fn;  // Optional; NULL (the default after
                                 // stpm34_init_handle()) means "drive
                                 // pin_enrst directly as a GPIO".
    void *reset_ctx;             // Opaque pointer passed through to reset_fn.
} stpm34_t;

void stpm34_init_handle(stpm34_t *dev,
                        gpio_num_t enrst,
                        gpio_num_t cs,
                        gpio_num_t syn,
                        int net_freq_hz);

/**
 * @brief Register a custom EN/RST line driver, for boards where that pin
 *        isn't a plain GPIO (e.g. driven through an I/O expander).
 *
 * Call this after stpm34_init_handle() and before stpm34_begin(). When a
 * callback is registered, `pin_enrst` is ignored entirely - this driver
 * never touches a GPIO for reset, it only ever calls `fn`.
 *
 * @param dev Device handle, already initialised via stpm34_init_handle().
 * @param fn  Callback to invoke for every EN/RST level change. Pass NULL
 *            to go back to driving pin_enrst directly.
 * @param ctx Opaque pointer passed through to every call of fn.
 */
void stpm34_set_reset_callback(stpm34_t *dev, stpm34_reset_fn_t fn, void *ctx);

esp_err_t stpm34_begin(stpm34_t *dev,
                       spi_host_device_t host,
                       gpio_num_t sck,
                       gpio_num_t miso,
                       gpio_num_t mosi,
                       int clock_hz);

/**
 * @brief Re‑initialise only the SPI device (remove and re‑add).
 *        Uses the stored clock speed.
 */
esp_err_t stpm34_reinit_spi(stpm34_t *dev);

bool stpm34_chip_init(stpm34_t *dev);

void stpm34_set_calibration(stpm34_t *dev, uint8_t channel, float calV, float calI);

float stpm34_read_rms_voltage(stpm34_t *dev, uint8_t channel);
float stpm34_read_rms_current(stpm34_t *dev, uint8_t channel);
float stpm34_read_active_power(stpm34_t *dev, uint8_t channel);
float stpm34_read_reactive_power(stpm34_t *dev, uint8_t channel);
float stpm34_read_apparent_rms_power(stpm34_t *dev, uint8_t channel);
float stpm34_read_power_factor(stpm34_t *dev, uint8_t channel);
void stpm34_read_line_frequency(stpm34_t *dev, float *freq_hz1, float *freq_hz2);
float stpm34_read_phase_angle(stpm34_t *dev, uint8_t channel);

#ifdef __cplusplus
}
#endif