/**
 * stpm34 - reference example
 *
 * Minimal init + read loop showing how to bring up one STPM34 channel and
 * read back its measurements. This file is for reference only - it is not
 * added to the component's own CMakeLists.txt, so it does not get built as
 * part of this library. Drop its contents into your own task/main.c and
 * adjust the pins/SPI host/calibration values for your board.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "stpm34.h"

static const char *TAG = "STPM34_EXAMPLE";
static stpm34_t s_meter;

/*
 * Optional: only needed if your board drives EN/RST through something
 * other than a plain GPIO (an I/O expander pin, a shift register, etc.).
 * If EN/RST is wired straight to an ESP32 GPIO, skip this entirely and
 * just pass that GPIO number to stpm34_init_handle() below - see the
 * commented-out stpm34_set_reset_callback() call in stpm34_example_init().
 *
 *   static void stpm34_reset_via_expander(bool level, void *ctx)
 *   {
 *       (void)ctx;
 *       my_expander_write(STPM34_EN_PIN, level); // e.g. io_write() from
 *                                                 // a separate io_expander
 *                                                 // component - stpm34
 *                                                 // itself has no idea
 *                                                 // this exists.
 *   }
 */

static void stpm34_example_init(void)
{
    // 1. Bind the logical pins (EN/RST, CS, SYN) and mains frequency.
    //    EN/RST is a plain GPIO in the common case - pass GPIO_NUM_NC here
    //    only if you'll drive it via a callback instead (see step 1b).
    stpm34_init_handle(&s_meter,
                        /*enrst=*/ GPIO_NUM_2,
                        /*cs=*/    GPIO_NUM_5,
                        /*syn=*/   GPIO_NUM_NC,
                        /*net_freq_hz=*/ 50);

    // 1b. Optional - only if EN/RST isn't a plain GPIO on your board.
    //     Call this before stpm34_begin(), with enrst above set to
    //     GPIO_NUM_NC. When a callback is registered it fully replaces
    //     GPIO control of EN/RST.
    // stpm34_set_reset_callback(&s_meter, stpm34_reset_via_expander, NULL);

    // 2. Bring up the SPI bus.
    ESP_ERROR_CHECK(stpm34_begin(&s_meter, SPI2_HOST,
                                 /*sck=*/  GPIO_NUM_6,
                                 /*miso=*/ GPIO_NUM_4,
                                 /*mosi=*/ GPIO_NUM_7,
                                 /*clock_hz=*/ 1000000));

    // 3. Push per-channel calibration (from your own calibration procedure).
    stpm34_set_calibration(&s_meter, /*channel=*/ 1, /*calV=*/ 0.9982f, /*calI=*/ 3.0538f);

    // 4. Initialize chip registers (gain, CRC, auto-latch, etc.). This is
    //    what actually pulses EN/RST, via whichever mechanism was set up
    //    in steps 1/1b.
    if (!stpm34_chip_init(&s_meter))
    {
        ESP_LOGE(TAG, "STPM34 chip init failed");
    }
}

static void stpm34_example_read_once(void)
{
    float voltage = stpm34_read_rms_voltage(&s_meter, 1);
    float current = stpm34_read_rms_current(&s_meter, 1);
    float power = stpm34_read_active_power(&s_meter, 1);
    float pf = stpm34_read_power_factor(&s_meter, 1);

    float freq1 = 0.0f, freq2 = 0.0f;
    stpm34_read_line_frequency(&s_meter, &freq1, &freq2);

    ESP_LOGI(TAG, "V=%.2fV I=%.3fA P=%.1fW PF=%.2f f=%.2fHz",
             voltage, current, power, pf, freq1);
}

void stpm34_example_task(void *arg)
{
    stpm34_example_init();

    while (1)
    {
        stpm34_example_read_once();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
