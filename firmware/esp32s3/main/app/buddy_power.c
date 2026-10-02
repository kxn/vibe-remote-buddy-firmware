#include "buddy_power.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "sdkconfig.h"

static temperature_sensor_handle_t sensor;
static esp_err_t sensor_error = ESP_ERR_INVALID_STATE;

void buddy_power_init(void) {
    /* Keep the PLL running and APB at 80 MHz for USB OTG and PSRAM. FreeRTOS raises the CPU
     * to 160 MHz whenever a task is runnable (including voice decoding).
     * Do not use XTAL-only DFS or automatic light sleep with this USB device. */
    const esp_pm_config_t config = {
        .min_freq_mhz = 80,
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&config));
    /* Use one supported S3 sensor range (20..100 C, nominal error +/-2 C). */
    const temperature_sensor_config_t ts = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    /* Temperature is optional diagnostics; sensor failure must not disable HID. */
    sensor_error = temperature_sensor_install(&ts, &sensor);
    if (sensor_error != ESP_OK) sensor = NULL;
}

void buddy_power_stats(cJSON *out) {
    esp_pm_config_t config;
    if (esp_pm_get_configuration(&config) == ESP_OK) {
        cJSON_AddNumberToObject(out, "cpu_min_mhz", config.min_freq_mhz);
        cJSON_AddNumberToObject(out, "cpu_max_mhz", config.max_freq_mhz);
        cJSON_AddBoolToObject(out, "light_sleep", config.light_sleep_enable);
    }
    cJSON_AddBoolToObject(out, "bt_modem_sleep", CONFIG_BT_CTRL_SLEEP_MODE_EFF == 1);
    float celsius = 0;
    esp_err_t result = sensor ? temperature_sensor_enable(sensor) : sensor_error;
    if (result == ESP_OK) {
        result = temperature_sensor_get_celsius(sensor, &celsius);
        esp_err_t stopped = temperature_sensor_disable(sensor);
        if (result == ESP_OK) result = stopped;
    }
    if (result == ESP_OK) cJSON_AddNumberToObject(out, "temperature_c", celsius);
    else cJSON_AddNullToObject(out, "temperature_c");
    cJSON_AddNumberToObject(out, "temperature_error", result);
}
