#include <cstdint>
#include <cstddef>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

namespace
{
    constexpr gpio_num_t kFanTach = GPIO_NUM_4;
    constexpr gpio_num_t kFanPwm = GPIO_NUM_5;
    constexpr uint8_t kFanEndpoint = 13;
    constexpr uint32_t kFanPwmHz = 25000;
    constexpr uint16_t kFanRpmCluster = 0xFC10;
    constexpr uint32_t kFanRequestedDutyPercent = 20;
    constexpr uint32_t kTachPulsesPerRev = 2; // Calibration, not a measurement: the tach reports pulses. 
    constexpr uint32_t kTachReportDelayMs = 2000; // Log cadence only; the sampling window is measured, not this value.

    constexpr gpio_num_t kLedGpios[] = {GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_20};
    constexpr uint8_t kLedEndpoints[] = {10, 11, 12};
    constexpr uint32_t kPwmPeriodTicks = 1024;           // LEDC 10-bit timer.
    constexpr uint32_t kLedPwmHz = 1000;

    constexpr uint32_t kAllZigbeeChannels = 0x07FFF800U; // 11..26

    const char *TAG = "KRATKY_C6";

    static volatile uint32_t s_tachPulses = 0;

    bool s_lightOn[3] = {false, false, false};
    uint8_t s_lightLevel[3] = {254, 254, 254};

    bool s_fanOn = true;
    uint8_t s_fanLevel = 51; // ~20%
    uint16_t s_reportedRpm = 0;
    bool s_zigbeeStarted = false;

    // The 2N7000 inverts PWM: GPIO HIGH pulls the fan PWM input LOW.
    void fanSetDutyPercent(uint32_t requestedPercent) {
        if (requestedPercent > 100) requestedPercent = 100;
        const uint32_t gpioHighPercent = 100 - requestedPercent;
        const uint32_t ticks = ((kPwmPeriodTicks - 1) * gpioHighPercent) / 100;
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ticks));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    }

    void IRAM_ATTR fanTachIsr(void *) {
        __atomic_fetch_add(&s_tachPulses, 1U, __ATOMIC_RELAXED);
    }

    void ledApply(size_t i) {
        const uint32_t duty = s_lightOn[i]
            ? (static_cast<uint32_t>(s_lightLevel[i]) * 1023U) / 254U
            : 0U;

        const auto channel = static_cast<ledc_channel_t>(i + 1);
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
    }

    void fanApply() {
        const uint32_t percent = s_fanOn
            ? (static_cast<uint32_t>(s_fanLevel) * 100U + 127U) / 254U
            : 0U;

        fanSetDutyPercent(percent);
    }

    void fanRpmTask(void *) {
        int64_t windowStartUs = esp_timer_get_time();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(kTachReportDelayMs));
            const int64_t nowUs = esp_timer_get_time();
            const uint64_t windowUs = static_cast<uint64_t>(nowUs - windowStartUs);
            windowStartUs = nowUs;
            const uint32_t pulses = __atomic_exchange_n(&s_tachPulses, 0U, __ATOMIC_RELAXED);

            // Pulse count over a measured window is the measurement; RPM only divides the calibration in.
            const uint64_t rpm = (static_cast<uint64_t>(pulses) * 60000000ULL) / (windowUs * kTachPulsesPerRev);
            ESP_LOGI(TAG, "Fan RPM: %lu (%lu pulses over %lu ms measured = %lu.%02lu pulses/s)",
                    static_cast<unsigned long>(rpm), static_cast<unsigned long>(pulses),
                    static_cast<unsigned long>(windowUs / 1000ULL),
                    static_cast<unsigned long>((pulses * 1000000ULL) / windowUs),
                    static_cast<unsigned long>((pulses * 100000000ULL / windowUs) % 100ULL));

            if (__atomic_load_n(&s_zigbeeStarted, __ATOMIC_ACQUIRE)
                && esp_zigbee_lock_acquire(pdMS_TO_TICKS(100)))
            {
                s_reportedRpm = rpm > UINT16_MAX
                    ? UINT16_MAX
                    : static_cast<uint16_t>(rpm);

                const auto status = ezb_zcl_set_attr_value(
                    kFanEndpoint,
                    kFanRpmCluster,
                    EZB_ZCL_CLUSTER_SERVER,
                    0x0000,
                    EZB_ZCL_STD_MANUF_CODE,
                    &s_reportedRpm,
                    false
                );

                if (status != EZB_ZCL_STATUS_SUCCESS)
                    ESP_LOGW(TAG, "RPM attribute update failed: %u", static_cast<unsigned>(status));

                esp_zigbee_lock_release();
            }
        }
    }

    void initHardware() {
        // Fail-safe: outputs LOW before Zigbee is started. Verify MOSFET control polarity.
        for (const auto pin : kLedGpios) {
            ESP_ERROR_CHECK(gpio_set_level(pin, 0));
            gpio_config_t cfg{};
            cfg.pin_bit_mask = (1ULL << pin);
            cfg.mode = GPIO_MODE_OUTPUT;
            cfg.pull_up_en = GPIO_PULLUP_DISABLE;
            cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            cfg.intr_type = GPIO_INTR_DISABLE;
            ESP_ERROR_CHECK(gpio_config(&cfg));
            ESP_ERROR_CHECK(gpio_set_level(pin, 0));
        }

        // Fan tach is open-collector: enable internal pull-up for RPM pulses.
        gpio_config_t tach{};
        tach.pin_bit_mask = (1ULL << kFanTach);
        tach.mode = GPIO_MODE_INPUT;
        tach.pull_up_en = GPIO_PULLUP_ENABLE; // No series 10k resistor needed.
        tach.pull_down_en = GPIO_PULLDOWN_DISABLE;
        tach.intr_type = GPIO_INTR_POSEDGE;
        ESP_ERROR_CHECK(gpio_config(&tach));
        ESP_ERROR_CHECK(gpio_install_isr_service(0));
        ESP_ERROR_CHECK(gpio_isr_handler_add(kFanTach, fanTachIsr, nullptr));

        ledc_timer_config_t timer{};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.timer_num = LEDC_TIMER_0;
        timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.freq_hz = kFanPwmHz;
        timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer));

        ledc_channel_config_t channel{};
        channel.gpio_num = kFanPwm;
        channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = LEDC_CHANNEL_0;
        channel.intr_type = LEDC_INTR_DISABLE;
        channel.timer_sel = LEDC_TIMER_0;
        channel.duty = 0; // GPIO LOW initially -> 2N7000 off -> full fan request.
        channel.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
        fanSetDutyPercent(kFanRequestedDutyPercent);

        ledc_timer_config_t ledTimer{};
        ledTimer.speed_mode = LEDC_LOW_SPEED_MODE;
        ledTimer.timer_num = LEDC_TIMER_1;
        ledTimer.duty_resolution = LEDC_TIMER_10_BIT;
        ledTimer.freq_hz = kLedPwmHz;
        ledTimer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&ledTimer));

        for (size_t i = 0; i < 3; i++) {
            ledc_channel_config_t led{};
            led.gpio_num = kLedGpios[i];
            led.speed_mode = LEDC_LOW_SPEED_MODE;
            led.channel = static_cast<ledc_channel_t>(i + 1);
            led.timer_sel = LEDC_TIMER_1;
            led.duty = 0;
            led.hpoint = 0;
            led.intr_type = LEDC_INTR_DISABLE;

            ESP_ERROR_CHECK(ledc_channel_config(&led));
        }

        xTaskCreate(fanRpmTask, "fan_rpm", 3072, nullptr, 3, nullptr);
    }

    void zigbeeZclCallback(ezb_zcl_core_action_callback_id_t id, void* payload) {
        if (id != EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID || !payload) {
            return;
        }

        auto *msg = static_cast<ezb_zcl_set_attr_value_message_t*>(payload);
        if (msg->info.cluster_role != EZB_ZCL_CLUSTER_SERVER
            || msg->in.attribute.data.value == nullptr) {
            return;
        }

        const uint8_t endpoint = msg->info.dst_ep;
        const uint16_t cluster = msg->info.cluster_id;
        const uint16_t attr = msg->in.attribute.id;
        const uint8_t value = *static_cast<const uint8_t*>(msg->in.attribute.data.value);

        if (endpoint == kFanEndpoint) {
            if (cluster == EZB_ZCL_CLUSTER_ID_ON_OFF
                && attr == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                    s_fanOn = value != 0;
            } else if (cluster == EZB_ZCL_CLUSTER_ID_LEVEL
                && attr == EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID) {
                    s_fanLevel = value > 254 ? 254 : value;
            } else {
                return;
            }

            fanApply();
            ESP_LOGI(TAG, "Fan PWM request: %lu%%", static_cast<unsigned long>(s_fanOn ? (s_fanLevel * 100U / 254U) : 0U));
            return;
        }

        for (size_t i = 0; i < 3; ++i) {
            if (endpoint != kLedEndpoints[i]) {
                continue;
            }

            if (cluster == EZB_ZCL_CLUSTER_ID_ON_OFF && attr == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                s_lightOn[i] = value != 0;
            } else if (cluster == EZB_ZCL_CLUSTER_ID_LEVEL && attr == EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID) {
                s_lightLevel[i] = value > 254 ? 254 : value;
            } else {
                return;
            }

            ledApply(i);
            ESP_LOGI(TAG, "Lamp %u: %s, brightness %u/254", static_cast<unsigned>(i + 1), s_lightOn[i] ? "ON" : "OFF", static_cast<unsigned>(s_lightLevel[i]));
            return;
        }
    }


    bool zigbeeSignalCallback(const ezb_app_signal_t *signal) {
        const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
        switch (type) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            break;
        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            const auto *status = static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(signal));
            if (status && *status == EZB_BDB_STATUS_SUCCESS) {
                if (ezb_bdb_is_factory_new()) {
                    ESP_LOGI(TAG, "Joining an existing Zigbee network");
                    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    ESP_LOGI(TAG, "Rejoined stored Zigbee network");
                }
            } else {
                ESP_LOGW(TAG, "Zigbee initialization unsuccessful; reboot to retry");
            }
            break;
        }
        case EZB_BDB_SIGNAL_STEERING: {
            const auto *status = static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(signal));
            ESP_LOGI(TAG, "Network steering: %s", (status && *status == EZB_BDB_STATUS_SUCCESS) ? "joined" : "failed");
            break;
        }
        default:
            break;
        }
        return true;
    }

    void zigbeeTask(void *) {
        esp_zigbee_config_t cfg{};
        cfg.device_config.device_type = EZB_NWK_DEVICE_TYPE_ROUTER;
        cfg.device_config.install_code_policy = false;
        cfg.device_config.zczr_config.max_children = 10;
        cfg.platform_config.storage_partition_name = "nvs";
        cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;

        ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
        // Router joins your existing Zigbee2MQTT coordinator; it does NOT form a new network.
        (void)ezb_aps_secur_enable_distributed_security(false);
        ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(kAllZigbeeChannels));
        ESP_ERROR_CHECK(ezb_app_signal_add_handler(zigbeeSignalCallback));

        ezb_af_device_desc_t device = ezb_af_create_device_desc();
        static char manufacturer[] = "\x08" "DIYHydro";
        static char model[] = "\x06" "Kratky";
        for (const auto endpoint : kLedEndpoints) {
            ezb_zha_dimmable_light_config_t light = EZB_ZHA_DIMMABLE_LIGHT_CONFIG();
            light.on_off_cfg.on_off = false;
            light.level_cfg.current_level = 254;

            ezb_af_ep_desc_t ep = ezb_zha_create_dimmable_light(endpoint, &light);

            ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(
                ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
            ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
                basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer));
            ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
                basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model));
            ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, ep));
        }

        ezb_zha_dimmable_light_config_t fan = EZB_ZHA_DIMMABLE_LIGHT_CONFIG();
        fan.on_off_cfg.on_off = true;
        fan.level_cfg.current_level = s_fanLevel;

        ezb_af_ep_desc_t fanEp = ezb_zha_create_dimmable_light(kFanEndpoint, &fan);

        ezb_zcl_custom_cluster_config_t rpmCfg{};
        rpmCfg.cluster_id = kFanRpmCluster;

        ezb_zcl_cluster_desc_t rpmCluster = ezb_zcl_custom_create_cluster_desc(&rpmCfg, EZB_ZCL_CLUSTER_SERVER);
        ESP_ERROR_CHECK(ezb_zcl_custom_cluster_desc_add_attr(rpmCluster,
            0x0000,
            EZB_ZCL_ATTR_TYPE_UINT16,
            EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
            &s_reportedRpm));

        ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(fanEp, rpmCluster));
        ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, fanEp));
        
        ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
        ezb_zcl_core_action_handler_register(zigbeeZclCallback);

        ESP_ERROR_CHECK(esp_zigbee_start(false));
        __atomic_store_n(&s_zigbeeStarted, true, __ATOMIC_RELEASE);
        
        ESP_LOGI(TAG, "Zigbee router started with light endpoints 10, 11, 12 and fan endpoint 13");
        esp_zigbee_launch_mainloop();
        esp_zigbee_deinit();
        vTaskDelete(nullptr);
}
} // namespace

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    initHardware();
    xTaskCreate(zigbeeTask, "zigbee", 8192, nullptr, 5, nullptr);
}
