#include "identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "net.h"

static const char *TAG = "identity";

#define NVS_NS "ffeink"
#define NVS_KEY_NAME "name"

static char s_hw_id[13];
static char s_name[NAME_MAX_LEN + 1];
/* The name is read by the status handler and written by the config
 * handler (both on the httpd task today) and read once by net_start; a
 * mutex keeps that true if handlers ever move to other tasks. */
static SemaphoreHandle_t s_lock;

void identity_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_hw_id, sizeof(s_hw_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    size_t len = sizeof(s_name);
    bool loaded = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    if (loaded) {
        loaded = nvs_get_str(h, NVS_KEY_NAME, s_name, &len) == ESP_OK && name_valid(s_name);
        nvs_close(h);
    }
    if (!loaded) {
        snprintf(s_name, sizeof(s_name), "ffeink-%s", s_hw_id + 6);
    }
    ESP_LOGI(TAG, "hw_id %s, name '%s'%s", s_hw_id, s_name, loaded ? "" : " (default)");
}

const char *identity_hw_id(void)
{
    return s_hw_id;
}

void identity_name(char out[NAME_MAX_LEN + 1])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_name, sizeof(s_name));
    xSemaphoreGive(s_lock);
}

esp_err_t identity_set_name(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_NAME, name);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        return err; /* nothing changed: memory and NVS stay consistent */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_name, name, sizeof(s_name));
    xSemaphoreGive(s_lock);
    net_set_hostname(name);
    ESP_LOGI(TAG, "name set to '%s'", name);
    return ESP_OK;
}
