/*
 * GDEP133C02 driver for the Good Display ESP32-133C02-IL board.
 *
 * Register values and command order are facts taken from Good Display's
 * example GDEP133C02.c dated 2026-04-20 (companyfile/2046, lines 14-202),
 * which carries no licence; this file is our own code around those facts.
 * Deliberate departures from the vendor example, each fixing a trap in it:
 *
 *   - BUSY is polled with a task delay and a cap, never a tight spin.
 *   - The SPI device has no hardware CS (spics_io_num = -1). The vendor
 *     leaves it at 0, which drives GPIO0, a boot strap pin.
 *   - CS_M/CS_S are GPIOs held low for a whole DTM half, because toggling CS
 *     resets the controller's data pointer.
 *   - After power-off we send deep sleep (DSLP 0x07 A5, datasheet p.12),
 *     which the vendor never does; the spec asks for sleep or power-off when
 *     idle. Deep sleep is only left by a hardware reset, so every frame
 *     starts with reset + full init.
 *   - Silent-panel check: BUSY must assert within 1 s of DRF and the refresh
 *     must take >= 5 s. A disconnected or dead panel otherwise looks like an
 *     instant successful refresh.
 *   - After any panel error the panel is power-cycled through LOAD_SW, with
 *     its signal lines driven low first so they do not back-power it.
 *
 * Board pins: ESP32-133C02-IL spec rev 2.0 p.5 and the example's pindefine.h.
 * The panel's D/C pin is unused: commands go as an 8-bit SPI command phase.
 */
#include "panel.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "frame.h"

static const char *TAG = "panel";

#define PIN_SCK 9
#define PIN_MOSI 41
#define PIN_CS_M 18
#define PIN_CS_S 17
#define PIN_RST 6
#define PIN_BUSY 7    /* low = busy; 2 kOhm pull-up on the panel side */
#define PIN_LOAD_SW 45 /* TPS22913B load switch, high = panel powered */

#define PANEL_SPI_HOST SPI3_HOST
#define PANEL_SPI_HZ (10 * 1000 * 1000) /* vendor value; datasheet max 20 MHz */

/* SPI3 cannot DMA from PSRAM, so each DTM half goes through this internal
 * bounce buffer: 80 rows x 300 B = 24,000 B per transaction, 20 per half. */
#define ROWS_PER_CHUNK 80
#define CHUNK_BYTES (ROWS_PER_CHUNK * FRAME_HALF_ROW_BYTES)
_Static_assert(FRAME_H % ROWS_PER_CHUNK == 0, "chunks must tile the frame");

/* BUSY caps. DRF is the long one: ~19 s warm, ~47 s seen on a cold panel. */
#define RESET_BUSY_MS 2000
#define PON_POF_BUSY_MS 30000
#define DRF_ASSERT_MS 1000
#define DRF_BUSY_MS 120000
#define DRF_MIN_MS 5000

enum { CS_NONE = 0, CS_M = 1, CS_S = 2, CS_BOTH = 3 };

/* Command codes (datasheet / vendor GDEP133C02.h). */
#define CMD_PSR 0x00
#define CMD_PWR 0x01
#define CMD_POF 0x02
#define CMD_PON 0x04
#define CMD_BTST_N 0x05
#define CMD_BTST_P 0x06
#define CMD_DSLP 0x07
#define CMD_DTM 0x10
#define CMD_DRF 0x12
#define CMD_CDI 0x50
#define CMD_TCON 0x60
#define CMD_TRES 0x61
#define CMD_AN_TM 0x74
#define CMD_AGID 0x86
#define CMD_BUCK_BOOST_VDDN 0xB0
#define CMD_TFT_VCOM_POWER 0xB1
#define CMD_EN_BUF 0xB6
#define CMD_BOOST_VDDP_EN 0xB7
#define CMD_CCSET 0xE0
#define CMD_PWS 0xE3
#define CMD_CMD66 0xF0

typedef struct {
    uint8_t cs;
    uint8_t reg;
    uint8_t len;
    uint8_t val[9];
} init_step_t;

/* Official init, in order. Which IC(s) each step targets matters: steps
 * marked CS_M configure the master's shared power/boost circuitry only. */
static const init_step_t k_init[] = {
    {CS_M, CMD_AN_TM, 9, {0xC0, 0x1E, 0x1E, 0xCE, 0xCE, 0xCE, 0x15, 0x15, 0x55}},
    {CS_BOTH, CMD_CMD66, 6, {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10}},
    {CS_BOTH, CMD_PSR, 2, {0xDF, 0x69}},
    {CS_BOTH, CMD_CDI, 1, {0xF7}},
    {CS_BOTH, CMD_TCON, 2, {0x03, 0x03}},
    {CS_BOTH, CMD_AGID, 1, {0x10}},
    {CS_BOTH, CMD_PWS, 1, {0x22}},
    {CS_BOTH, CMD_CCSET, 1, {0x01}},
    {CS_BOTH, CMD_TRES, 4, {0x04, 0xB0, 0x03, 0x20}}, /* 0x04B0, 0x0320 as in the example */
    {CS_M, CMD_PWR, 6, {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38}},
    {CS_M, CMD_EN_BUF, 1, {0x07}},
    {CS_M, CMD_BTST_P, 2, {0xE8, 0x28}},
    {CS_M, CMD_BOOST_VDDP_EN, 1, {0x01}},
    {CS_M, CMD_BTST_N, 2, {0xE8, 0x28}},
    {CS_M, CMD_BUCK_BOOST_VDDN, 1, {0x01}},
    {CS_M, CMD_TFT_VCOM_POWER, 1, {0x02}},
};

static spi_device_handle_t s_spi;
static DMA_ATTR uint8_t s_dma[CHUNK_BYTES];
static uint8_t *s_frame; /* PSRAM, FRAME_BYTES */
static TaskHandle_t s_task;

/* Status: written by the panel task (and claim/submit), read lock-free by
 * the status endpoint. Result fields are stored before state goes IDLE, so a
 * reader that sees IDLE also sees that frame's outcome. */
static _Atomic int s_state = PANEL_IDLE;
static _Atomic uint32_t s_pending_seq;
static _Atomic uint32_t s_last_seq;
static _Atomic uint32_t s_frames_shown;
static _Atomic uint32_t s_panel_errors;
static _Atomic(const char *) s_last_error = "";
static _Atomic uint32_t s_last_error_seq;
static _Atomic uint32_t s_last_refresh_ms;
static _Atomic uint32_t s_last_frame_ms;
static _Atomic int64_t s_last_shown_us;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void cs_select(int mask)
{
    gpio_set_level(PIN_CS_M, (mask & CS_M) ? 0 : 1);
    gpio_set_level(PIN_CS_S, (mask & CS_S) ? 0 : 1);
}

/* One transaction: optional 8-bit command phase, then `len` bytes from the
 * DMA buffer `data` (which must be s_dma or another internal DMA buffer). */
static esp_err_t spi_tx(bool has_cmd, uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_ext_t t = {0};
    if (has_cmd) {
        t.base.cmd = cmd;
    } else {
        t.base.flags = SPI_TRANS_VARIABLE_CMD;
        t.command_bits = 0;
    }
    t.base.length = len * 8;
    t.base.tx_buffer = len ? data : NULL;
    return spi_device_transmit(s_spi, &t.base);
}

/* Command with up to sizeof(s_dma) parameter bytes, framed by CS. */
static esp_err_t write_reg(int cs, uint8_t reg, const uint8_t *val, size_t len)
{
    memcpy(s_dma, val, len); /* params may live in flash; DMA needs RAM */
    cs_select(cs);
    esp_err_t err = spi_tx(true, reg, s_dma, len);
    cs_select(CS_NONE);
    return err;
}

/* Wait until BUSY reads `level`. Polls each tick (1 ms at HZ=1000). */
static bool wait_busy(int level, uint32_t timeout_ms, uint32_t *elapsed_ms)
{
    uint32_t t0 = now_ms();
    while (gpio_get_level(PIN_BUSY) != level) {
        if (now_ms() - t0 >= timeout_ms) {
            return false;
        }
        vTaskDelay(1);
    }
    if (elapsed_ms) {
        *elapsed_ms = now_ms() - t0;
    }
    return true;
}

static void hw_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t spi_open(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1, /* no reads: the 0xF2 status read is unproven and unused */
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_BYTES,
    };
    const spi_device_interface_config_t dev = {
        .command_bits = 8,
        .mode = 0,
        .clock_speed_hz = PANEL_SPI_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_initialize(PANEL_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        err = spi_bus_add_device(PANEL_SPI_HOST, &dev, &s_spi);
    }
    return err;
}

static void spi_close(void)
{
    spi_bus_remove_device(s_spi);
    spi_bus_free(PANEL_SPI_HOST);
    s_spi = NULL;
}

/* Drive every panel input low (SCK/MOSI become plain GPIOs). Done before the
 * load switch opens, so the unpowered panel is not fed through its inputs. */
static void lines_low(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_SCK) | BIT64(PIN_MOSI) | BIT64(PIN_CS_M) | BIT64(PIN_CS_S)
                        | BIT64(PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_SCK, 0);
    gpio_set_level(PIN_MOSI, 0);
    gpio_set_level(PIN_CS_M, 0);
    gpio_set_level(PIN_CS_S, 0);
    gpio_set_level(PIN_RST, 0);
}

/* Power on with lines low, then idle levels and the SPI bus. */
static esp_err_t power_on(void)
{
    gpio_set_level(PIN_LOAD_SW, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    cs_select(CS_NONE);
    return spi_open();
}

/* Recovery after a panel error. If the bus cannot come back, nothing else
 * can drive the panel either, so restarting the chip is the only recovery. */
static void power_cycle(void)
{
    ESP_LOGW(TAG, "power-cycling panel");
    spi_close();
    lines_low();
    gpio_set_level(PIN_LOAD_SW, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_err_t err = power_on();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI re-init failed (%s); restarting", esp_err_to_name(err));
        esp_restart();
    }
}

static const char *init_panel(void)
{
    hw_reset();
    if (!wait_busy(1, RESET_BUSY_MS, NULL)) {
        return "reset_busy_timeout";
    }
    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        const init_step_t *s = &k_init[i];
        if (write_reg(s->cs, s->reg, s->val, s->len) != ESP_OK) {
            return "spi";
        }
    }
    return NULL;
}

/* DTM to one IC: command, then its 300 B of all 1600 rows, CS held low. */
static const char *send_half(int cs, enum frame_half half)
{
    esp_err_t err;
    cs_select(cs);
    err = spi_tx(true, CMD_DTM, NULL, 0);
    for (unsigned row = 0; err == ESP_OK && row < FRAME_H; row += ROWS_PER_CHUNK) {
        frame_gather_half(s_dma, s_frame, row, ROWS_PER_CHUNK, half);
        err = spi_tx(false, 0, s_dma, CHUNK_BYTES);
    }
    cs_select(CS_NONE);
    return err == ESP_OK ? NULL : "spi";
}

/* POF to both ICs, CS held through the BUSY wait as the vendor does. */
static bool power_off_rails(uint32_t timeout_ms)
{
    static const uint8_t pof = 0x00;
    memcpy(s_dma, &pof, 1);
    cs_select(CS_BOTH);
    bool ok = spi_tx(true, CMD_POF, s_dma, 1) == ESP_OK && wait_busy(1, timeout_ms, NULL);
    cs_select(CS_NONE);
    return ok;
}

/* PON -> DRF -> POF -> DSLP, CS on both ICs held through each BUSY wait as
 * the vendor does. Returns NULL on success, else a static error string.
 * Once PON has succeeded the boost/VCOM rails are up, so any later failure
 * first tries a POF: the load switch should not be the thing that drops
 * live panel rails (datasheet power-off sequence). */
static const char *refresh(uint32_t *refresh_ms)
{
    static const uint8_t drf = 0x01, dslp = 0xA5;

    cs_select(CS_BOTH);
    esp_err_t err = spi_tx(true, CMD_PON, NULL, 0);
    bool ok = err == ESP_OK && wait_busy(1, PON_POF_BUSY_MS, NULL);
    cs_select(CS_NONE);
    if (!ok) {
        return err == ESP_OK ? "pon_timeout" : "spi";
    }

    cs_select(CS_BOTH);
    vTaskDelay(pdMS_TO_TICKS(30));
    memcpy(s_dma, &drf, 1);
    err = spi_tx(true, CMD_DRF, s_dma, 1);
    const char *fail = NULL;
    if (err != ESP_OK) {
        fail = "spi";
    } else if (!wait_busy(0, DRF_ASSERT_MS, NULL)) {
        fail = "drf_busy_never_asserted"; /* silent panel */
    } else if (!wait_busy(1, DRF_BUSY_MS, refresh_ms)) {
        fail = "drf_timeout";
    } else if (*refresh_ms < DRF_MIN_MS) {
        fail = "drf_too_fast"; /* silent panel */
    }
    cs_select(CS_NONE);
    if (fail) {
        /* Best effort before the power cycle; skipped while BUSY is still
         * low (drf_timeout), since the IC takes no commands while busy. */
        if (gpio_get_level(PIN_BUSY) == 1) {
            power_off_rails(5000);
        }
        return fail;
    }

    if (!power_off_rails(PON_POF_BUSY_MS)) {
        return "pof_timeout";
    }
    /* No BUSY wait after DSLP: the IC is asleep until the next reset. */
    return write_reg(CS_BOTH, CMD_DSLP, &dslp, 1) == ESP_OK ? NULL : "spi";
}

static void show_frame(uint32_t seq)
{
    uint32_t t0 = now_ms();
    uint32_t refresh_ms = 0;
    const char *err = init_panel();
    if (!err) err = send_half(CS_M, FRAME_HALF_MASTER);
    if (!err) err = send_half(CS_S, FRAME_HALF_SLAVE);
    if (!err) err = refresh(&refresh_ms);

    if (err) {
        ESP_LOGE(TAG, "frame seq=%lu failed: %s", (unsigned long)seq, err);
        atomic_store(&s_last_error, err);
        atomic_store(&s_last_error_seq, seq);
        atomic_fetch_add(&s_panel_errors, 1);
        power_cycle();
        return;
    }
    uint32_t frame_ms = now_ms() - t0;
    ESP_LOGI(TAG, "frame seq=%lu shown: refresh %lu ms, total %lu ms", (unsigned long)seq,
             (unsigned long)refresh_ms, (unsigned long)frame_ms);
    atomic_store(&s_last_refresh_ms, refresh_ms);
    atomic_store(&s_last_frame_ms, frame_ms);
    atomic_store(&s_last_shown_us, esp_timer_get_time());
    atomic_store(&s_last_seq, seq);
    atomic_fetch_add(&s_frames_shown, 1);
}

static void panel_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        show_frame(atomic_load(&s_pending_seq));
        atomic_store(&s_state, PANEL_IDLE); /* last: publishes the outcome */
    }
}

esp_err_t panel_init(void)
{
    s_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) {
        ESP_LOGE(TAG, "no PSRAM for the %d B frame buffer", FRAME_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame, 0x11, FRAME_BYTES); /* white */

    const gpio_config_t busy = {
        .pin_bit_mask = BIT64(PIN_BUSY),
        .mode = GPIO_MODE_INPUT, /* panel provides the pull-up */
    };
    const gpio_config_t load_sw = {
        .pin_bit_mask = BIT64(PIN_LOAD_SW),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&busy);
    if (err == ESP_OK) err = gpio_config(&load_sw);
    if (err != ESP_OK) {
        return err;
    }
    lines_low();
    gpio_set_level(PIN_LOAD_SW, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    err = power_on();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Below the network and USB tasks: the panel is never latency-critical,
     * and its BUSY waits sleep rather than spin. */
    if (xTaskCreatePinnedToCore(panel_task, "panel", 4096, NULL, 3, &s_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint8_t *panel_claim(void)
{
    int expected = PANEL_IDLE;
    if (!atomic_compare_exchange_strong(&s_state, &expected, PANEL_RECEIVING)) {
        return NULL;
    }
    return s_frame;
}

void panel_abort(void)
{
    atomic_store(&s_state, PANEL_IDLE);
}

void panel_submit(uint32_t seq)
{
    atomic_store(&s_pending_seq, seq);
    atomic_store(&s_state, PANEL_REFRESHING);
    xTaskNotifyGive(s_task);
}

void panel_get_status(panel_status_t *out)
{
    out->state = (panel_state_t)atomic_load(&s_state);
    out->last_seq = atomic_load(&s_last_seq);
    out->frames_shown = atomic_load(&s_frames_shown);
    out->panel_errors = atomic_load(&s_panel_errors);
    out->last_error = atomic_load(&s_last_error);
    out->last_error_seq = atomic_load(&s_last_error_seq);
    out->last_refresh_ms = atomic_load(&s_last_refresh_ms);
    out->last_frame_ms = atomic_load(&s_last_frame_ms);
    out->last_shown_us = atomic_load(&s_last_shown_us);
}
