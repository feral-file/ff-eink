/*
 * ff-eink spike firmware: show frames pushed over wired Ethernet on a
 * Good Display GDEP133C02 (13.3" Spectra 6) + ESP32-133C02-IL board.
 *
 * Boot sequence and why it is ordered this way:
 *   1. NVS counters (boot_count, brownouts, previous restart cause): the
 *      pusher attributes missed frames to reboots from these.
 *   2. Maintenance check: SW2 (IO12) held at reset keeps the native USB port
 *      on USB-Serial-JTAG, so `idf.py flash` and logs work. The board's only
 *      USB-C becomes a USB *host* port in normal mode, which removes the
 *      serial/JTAG device and with it esptool's auto-reset into download
 *      mode; no BOOT button is documented. This check is the way back, so it
 *      runs before anything that could hang or take the PHY.
 *   3. Panel (frame buffer, SPI, power) and the SW3 test-pattern button.
 *   4. Normal mode only: a 15 s grace window, then USB host + ECM network,
 *      HTTP API, link watchdog. During the window USB-Serial-JTAG is still
 *      up, so esptool can reset the chip into download mode (press Reset,
 *      then flash). That is the second way back, and the only one that does
 *      not depend on SW2's wiring matching the spec: it makes the very first
 *      flash safe before maintenance mode has been seen working.
 */
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "api.h"
#include "net.h"
#include "panel.h"
#include "patterns.h"

static const char *TAG = "main";

#define PIN_SW2_MAINT 12   /* active-high (spec p.5) */
#define PIN_SW3_PATTERN 13 /* active-high */

/* No IP for this long -> restart. A USB NIC that wedged without a
 * disconnect event, or a host stack stuck after a replug, has no other
 * recovery path; a restart costs at most a missed frame or two. */
#define LINK_WATCHDOG_US (10LL * 60 * 1000 * 1000)

#define FLASH_GRACE_MS 15000

#define NVS_NS "ffeink"
#define RESTART_LINK_WATCHDOG "link_watchdog"

static api_boot_info_t s_boot;
static char s_restart_cause[24];

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}

/* Counters are best-effort: a failed NVS write costs evidence, not function,
 * so errors are logged and boot continues. */
static void boot_counters(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    esp_reset_reason_t reason = esp_reset_reason();
    s_boot.reset_reason = reset_reason_name(reason);
    s_boot.restart_cause = s_restart_cause;

    nvs_handle_t h;
    if (err != ESP_OK || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); counters not kept", esp_err_to_name(err));
        return;
    }
    uint32_t boots = 0, brownouts = 0;
    nvs_get_u32(h, "boots", &boots);
    nvs_get_u32(h, "brownouts", &brownouts);
    size_t len = sizeof(s_restart_cause);
    if (nvs_get_str(h, "rcause", s_restart_cause, &len) != ESP_OK) {
        s_restart_cause[0] = '\0';
    }
    boots++;
    if (reason == ESP_RST_BROWNOUT) {
        brownouts++;
    }
    nvs_set_u32(h, "boots", boots);
    nvs_set_u32(h, "brownouts", brownouts);
    nvs_erase_key(h, "rcause"); /* the cause describes one restart only */
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed");
    }
    nvs_close(h);
    s_boot.boot_count = boots;
    s_boot.brownouts = brownouts;
    ESP_LOGI(TAG, "boot %lu, reset %s, previous restart cause '%s', brownouts %lu",
             (unsigned long)boots, s_boot.reset_reason, s_restart_cause,
             (unsigned long)brownouts);
}

static void restart_with_cause(const char *cause)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "rcause", cause);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGE(TAG, "restarting: %s", cause);
    esp_restart();
}

static void buttons_init(void)
{
    /* Internal pull-downs keep an active-high button low if the board has no
     * pull-down of its own; harmless if it does. */
    const gpio_config_t in = {
        .pin_bit_mask = BIT64(PIN_SW2_MAINT) | BIT64(PIN_SW3_PATTERN),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&in);
}

/* SW2 must read high on every sample for 50 ms, so a glitch cannot drop the
 * board into maintenance (no network) by accident. */
static bool maintenance_requested(void)
{
    for (int i = 0; i < 10; i++) {
        if (gpio_get_level(PIN_SW2_MAINT) != 1) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

/* SW3 press: show the next test pattern (gate G1, no network needed). Seq 0
 * marks a local pattern; pushed frames use the pusher's seq. */
static void pattern_button_task(void *arg)
{
    int prev = 0;
    pattern_t next = PATTERN_CHART;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int level = gpio_get_level(PIN_SW3_PATTERN);
        if (level && !prev) {
            uint8_t *fb = panel_claim();
            if (fb) {
                ESP_LOGI(TAG, "SW3: pattern %s", pattern_name(next));
                pattern_draw(fb, next);
                panel_submit(0);
                next = (pattern_t)((next + 1) % PATTERN_COUNT);
            } else {
                ESP_LOGW(TAG, "SW3: panel busy");
            }
        }
        prev = level;
    }
}

static void link_watchdog(void)
{
    int64_t last_ip = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        net_status_t n;
        net_get_status(&n);
        int64_t now = esp_timer_get_time();
        if (n.has_ip) {
            last_ip = now;
            continue;
        }
        if (now - last_ip < LINK_WATCHDOG_US) {
            continue;
        }
        /* Never cut a refresh short: an interrupted waveform leaves the panel
         * in an undefined state. A refresh ends within ~2 min (panel.c caps). */
        panel_status_t p;
        panel_get_status(&p);
        if (p.state == PANEL_IDLE) {
            restart_with_cause(RESTART_LINK_WATCHDOG);
        }
    }
}

void app_main(void)
{
    boot_counters();
    buttons_init();
    bool maintenance = maintenance_requested();
    ESP_LOGI(TAG, "SW2=%d SW3=%d -> %s mode", gpio_get_level(PIN_SW2_MAINT),
             gpio_get_level(PIN_SW3_PATTERN), maintenance ? "maintenance" : "normal");

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t err = panel_init();
    if (err != ESP_OK) {
        /* Without a frame buffer or SPI bus the device has no purpose; the
         * restart loop shows up as a climbing boot_count. USB host has not
         * started yet, so the board stays flashable meanwhile. */
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(10000));
        restart_with_cause("panel_init");
    }
    xTaskCreatePinnedToCore(pattern_button_task, "sw3", 3072, NULL, 2, NULL, 1);

    if (maintenance) {
        ESP_LOGW(TAG, "maintenance mode (SW2 held): USB-Serial-JTAG kept, no network. "
                      "Flash with idf.py flash; SW3 shows test patterns.");
        return;
    }

    ESP_LOGI(TAG, "USB host starts in %d s; flash now if needed", FLASH_GRACE_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(FLASH_GRACE_MS));
    ESP_LOGI(TAG, "starting USB host network; USB-Serial-JTAG goes away now");
    err = net_start();
    if (err != ESP_OK) {
        /* The link watchdog restarts us in 10 min; nothing better to do. */
        ESP_LOGE(TAG, "network start failed: %s", esp_err_to_name(err));
    }
    err = api_start(&s_boot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
    }
    link_watchdog();
}
