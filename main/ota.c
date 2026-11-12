/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "ota.h"

#include <stdatomic.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net.h"
#include "panel.h"

static const char *TAG = "ota";

#define VALID_AFTER_IP_US (60LL * 1000 * 1000)
#define GIVE_UP_US (10LL * 60 * 1000 * 1000)

static _Atomic(const char *) s_state = "unknown";
static bool s_rolled_back;

const char *ota_version(void)
{
    return esp_app_get_description()->version;
}

/* Computed once in ota_start_validation from otadata alone. Not
 * esp_ota_get_last_invalid_partition(): that re-verifies the whole image,
 * signature included (SHA-256 over the slot plus RSA), which is far too
 * heavy for every status poll on the httpd task. */
bool ota_rolled_back(void)
{
    return s_rolled_back;
}

const char *ota_state(void)
{
    return atomic_load(&s_state);
}

/* Roll back without cutting a refresh. Does not return. */
static void roll_back(const char *why)
{
    ESP_LOGE(TAG, "new image failed validation (%s): rolling back", why);
    for (;;) {
        panel_status_t p;
        panel_get_status(&p);
        if (p.state == PANEL_IDLE) {
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void rollback_task(void *why)
{
    roll_back((const char *)why);
}

static void validate_task(void *arg)
{
    int64_t start = esp_timer_get_time(), ip_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        net_status_t n;
        net_get_status(&n);
        int64_t now = esp_timer_get_time();
        if (n.has_ip) {
            if (!ip_since) {
                ip_since = now;
            }
            if (now - ip_since >= VALID_AFTER_IP_US) {
                /* Only report "valid" once otadata says so: if the write
                 * failed, the slot is still pending and the next power cycle
                 * would revert it, so keep trying instead. */
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    atomic_store(&s_state, "valid");
                    ESP_LOGI(TAG, "image %s marked valid", ota_version());
                    vTaskDelete(NULL);
                }
                ESP_LOGE(TAG, "mark valid failed: %s; retrying", esp_err_to_name(err));
            }
        } else {
            ip_since = 0;
            if (now - start >= GIVE_UP_US) {
                roll_back("no IP for 10 min");
            }
        }
    }
}

void ota_start_validation(bool api_ok)
{
    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    s_rolled_back = other && esp_ota_get_state_partition(other, &st) == ESP_OK
                    && (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED);
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) {
        /* Flashed over USB: otadata has no state for this slot. Nothing to
         * validate, and the bootloader never rolls it back. */
        atomic_store(&s_state, "valid");
        return;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        atomic_store(&s_state, "valid");
        return;
    }
    atomic_store(&s_state, "pending");
    ESP_LOGW(TAG, "image %s is pending verification", ota_version());
    /* Own 8 KB tasks, not the caller's (app_main has 3.5 KB): rolling back
     * re-verifies the previous image, SHA-256 and RSA signature check on
     * the stack. */
    if (!api_ok) {
        xTaskCreate(rollback_task, "ota_rollback", 8192, (void *)"HTTP server did not start",
                    2, NULL);
        return;
    }
    xTaskCreate(validate_task, "ota_valid", 8192, NULL, 2, NULL);
}
