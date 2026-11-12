/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "restart.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "panel.h"

static const char *TAG = "restart";

void restart_with_cause(const char *cause)
{
    nvs_handle_t h;
    if (nvs_open("ffeink", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "rcause", cause);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGE(TAG, "restarting: %s", cause);
    esp_restart();
}

void restart_when_idle(const char *cause)
{
    for (;;) {
        panel_status_t p;
        panel_get_status(&p);
        if (p.state == PANEL_IDLE) {
            restart_with_cause(cause);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the caller's HTTP response go out */
    restart_when_idle((const char *)arg);
}

void restart_when_idle_async(const char *cause)
{
    /* cause must be a string literal (static lifetime). */
    xTaskCreate(restart_task, "restart", 3072, (void *)cause, 2, NULL);
}
