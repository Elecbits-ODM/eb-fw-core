/*
 * BL0937 basic read example.
 *
 * Starts the driver with the Kconfig defaults and prints one measurement
 * snapshot per second.
 */

#include <stdio.h>

#include "bl0937.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "example";

void app_main(void)
{
    bl0937_config_t cfg = bl0937_config_default();

    /* Anything from Kconfig can be overridden here, for example per-unit
     * calibration factors loaded from NVS at runtime:
     *
     *   cfg.voltage_calibration = nvs_load_float("v_cal");
     *   cfg.current_calibration = nvs_load_float("i_cal");
     *   cfg.power_calibration   = nvs_load_float("p_cal");
     */

    ESP_ERROR_CHECK(bl0937_init(&cfg));
    ESP_ERROR_CHECK(bl0937_start());

    ESP_LOGI(TAG, "BL0937 started on CF=%d CF1=%d SEL=%d",
             cfg.gpio_cf, cfg.gpio_cf1, cfg.gpio_sel);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        bl0937_measurements_t m = bl0937_get();

        ESP_LOGI(TAG,
                 "V=%.1f V%s  I=%.3f A%s  P=%.1f W%s  E=%.3f Wh   "
                 "[cf=%.1fHz cfu=%.1fHz cfi=%.1fHz]",
                 m.voltage_v, m.valid_voltage ? "" : " (invalid)",
                 m.current_a, m.valid_current ? "" : " (invalid)",
                 m.power_w,   m.valid_power   ? "" : " (invalid)",
                 m.energy_wh,
                 m.cf_hz, m.cfu_hz, m.cfi_hz);
    }
}
