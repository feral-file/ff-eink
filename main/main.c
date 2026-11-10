/*
 * ff-eink spike firmware: show frames pushed over wired Ethernet on a
 * Good Display GDEP133C02 (13.3" Spectra 6) + ESP32-133C02-IL board.
 *
 * Boot sequence and why it is ordered this way:
 *   1. NVS counters (boot_count, brownouts, previous restart cause): the
 *      pusher attributes missed frames to reboots from these.
 *   2. Panel (frame buffer, SPI, power).
 *   3. Either the bench build (CONFIG_FF_EINK_BENCH_PATTERNS): show the next
 *      test pattern and stop, never touching USB; or the normal build: a
 *      60 s grace window, then USB host + ECM network, HTTP API, link
 *      watchdog.
 *
 * The grace window is the only way back to reflashing. The board's only
 * USB-C becomes a USB *host* port in the normal build, which removes the
 * USB-Serial-JTAG device and with it esptool's auto-reset into download
 * mode. The board has no BOOT button, and its function buttons SW2-SW4
 * (spec 3.1, IO12-14) are reserved footprints, not fitted: only Reset and
 * Power exist. During the window USB-Serial-JTAG is still up, so pressing
 * Reset and flashing within 60 s always works.
 */
#include <stdatomic.h>
#include <string.h>

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


/* No IP for this long -> restart. A USB NIC that wedged without a
 * disconnect event, or a host stack stuck after a replug, has no other
 * recovery path; a restart costs at most a missed frame or two. */
#define LINK_WATCHDOG_US (10LL * 60 * 1000 * 1000)

/* Long enough for: Reset, the Mac re-enumerating the port, and esptool
 * starting (not building!). A reboot already costs a frame, so the extra
 * boot delay barely matters for the soak. */
#define FLASH_GRACE_MS 60000

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

#if CONFIG_FF_EINK_BENCH_PATTERNS
/* Bench build (gate G1): each boot shows the next generated pattern, so
 * pressing Reset steps chart -> bars -> checker without any network or
 * button. The index survives the reset in NVS. Seq 0 marks a local pattern. */
static void bench_show_next_pattern(void)
{
    uint8_t idx = 0;
    nvs_handle_t h;
    bool nvs_ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (nvs_ok) {
        nvs_get_u8(h, "pattern", &idx);
        nvs_set_u8(h, "pattern", (uint8_t)((idx + 1) % PATTERN_COUNT));
        nvs_commit(h);
        nvs_close(h);
    }
    pattern_t p = (pattern_t)(idx % PATTERN_COUNT);
    uint8_t *fb = panel_claim(); /* just booted: always idle */
    pattern_draw(fb, p);
    panel_submit(0);
    ESP_LOGW(TAG, "bench build: showing pattern '%s' (%d of %d); press Reset for the next. "
                  "No network in this build.", pattern_name(p), p + 1, PATTERN_COUNT);
}
#endif

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

#if CONFIG_FF_EINK_BENCH_PATTERNS
    bench_show_next_pattern();
    return; /* USB-Serial-JTAG stays: flash and monitor work at any time */
#endif

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
