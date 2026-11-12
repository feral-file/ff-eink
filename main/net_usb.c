/*
 * SPDX-License-Identifier: MPL-2.0 AND Apache-2.0
 * Copyright (c) 2026 Feral File
 * Portions Copyright 2024-2026 Espressif Systems (Shanghai) CO LTD (Apache-2.0,
 * see below)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Ethernet over the board's only USB-C port: the ESP32-S3 runs its native
 * USB (Full Speed, 12 Mbit/s) as host, and a "PoE to USB-C" adapter on that
 * port supplies 5 V and a USB NIC. Driver: Espressif iot_usbh_ecm (CDC-ECM,
 * Apache-2.0) on iot_eth, glued to esp_netif with DHCP.
 *
 * Assumption to verify on the bench: the adapter's Realtek RTL815x exposes
 * CDC-ECM as its second USB configuration (configuration 1 is Realtek's
 * vendor protocol, which this firmware does not speak). The enum filter
 * therefore picks configuration 2 whenever a device offers more than one.
 * The VID/PID and configuration count it saw are reported in the status.
 *
 * The bring-up sequence (USB host task with enum filter, CDC driver install,
 * ECM driver, iot_eth, esp_netif glue) and its parameters are adapted from
 * Espressif's example esp-iot-solution examples/usb/host/usb_ecm_4g_module,
 * main/usb_ecm_4g_module.c: Copyright 2024-2026 Espressif Systems (Shanghai)
 * CO LTD, Apache-2.0 (LICENSES/Apache-2.0.txt). Modified: restructured into
 * net_start()/net_get_status(), own event handling, status counters and
 * error handling; modem/AT and Wi-Fi parts not used.
 */
#include "net.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"
#include "usb/usb_host.h"

#include "identity.h"

static const char *TAG = "net";

static iot_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static _Atomic bool s_link_up;
static _Atomic bool s_has_ip;
static _Atomic uint32_t s_ip;
static _Atomic uint32_t s_link_downs;
static _Atomic uint32_t s_ip_acquired;
static _Atomic uint32_t s_usb_id; /* vid << 16 | pid */
static _Atomic uint8_t s_usb_configs;
static uint8_t s_mac[6];          /* written once per link-up, before s_link_up */

/* Runs in the USB host library task during enumeration. */
static bool enum_filter(const usb_device_desc_t *dev, uint8_t *config_value)
{
    *config_value = dev->bNumConfigurations > 1 ? 2 : 1;
    atomic_store(&s_usb_id, ((uint32_t)dev->idVendor << 16) | dev->idProduct);
    atomic_store(&s_usb_configs, dev->bNumConfigurations);
    ESP_LOGI(TAG, "USB %04x:%04x, %d configuration(s), using %d", dev->idVendor,
             dev->idProduct, dev->bNumConfigurations, *config_value);
    return true;
}

/* Owns the USB host library for the life of the process: it is installed
 * here (so the enum filter can be set) and never uninstalled. */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = enum_filter,
    };
    esp_err_t err = usb_host_install(&cfg);
    xTaskNotify((TaskHandle_t)arg, (uint32_t)err, eSetValueWithOverwrite);
    if (err != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IOT_ETH_EVENT && id == IOT_ETH_EVENT_CONNECTED) {
        iot_eth_get_addr(s_eth, s_mac);
        atomic_store(&s_link_up, true);
        ESP_LOGI(TAG, "link up, MAC %02x:%02x:%02x:%02x:%02x:%02x", s_mac[0], s_mac[1],
                 s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    } else if (base == IOT_ETH_EVENT && id == IOT_ETH_EVENT_DISCONNECTED) {
        if (atomic_exchange(&s_link_up, false)) {
            atomic_fetch_add(&s_link_downs, 1);
        }
        atomic_store(&s_has_ip, false);
        atomic_store(&s_ip, 0);
        ESP_LOGW(TAG, "link down");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        /* Lease not renewed with the link still up: without this the link
         * watchdog would never see the outage. */
        atomic_store(&s_has_ip, false);
        atomic_store(&s_ip, 0);
        ESP_LOGW(TAG, "lost IP");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        atomic_store(&s_ip, ev->ip_info.ip.addr);
        atomic_store(&s_has_ip, true);
        atomic_fetch_add(&s_ip_acquired, 1);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

esp_err_t net_start(void)
{
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 5,
                                NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t install_err = ESP_ERR_TIMEOUT;
    xTaskNotifyWait(0, UINT32_MAX, &install_err, pdMS_TO_TICKS(1000));
    if (install_err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name((esp_err_t)install_err));
        return (esp_err_t)install_err;
    }

    const usbh_cdc_driver_config_t cdc = {
        .task_stack_size = 4096,
        .task_priority = configMAX_PRIORITIES - 1,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    err = usbh_cdc_driver_install(&cdc);
    if (err != ESP_OK) {
        return err;
    }

    /* Any VID/PID: the bench adapter's chip is unverified, and a non-ECM
     * device simply never connects (visible as usb_vid/pid with link down).
     * The driver keeps a pointer to this list, so it must stay static. */
    static const usb_device_match_id_t match[] = {
        {.match_flags = USB_DEVICE_ID_MATCH_VID_PID,
         .idVendor = USB_DEVICE_VENDOR_ANY,
         .idProduct = USB_DEVICE_PRODUCT_ANY},
        {0},
    };
    const iot_usbh_ecm_config_t ecm_cfg = {.match_id_list = match};
    iot_eth_driver_t *drv = NULL;
    err = iot_eth_new_usb_ecm(&ecm_cfg, &drv);
    if (err != ESP_OK) {
        return err;
    }
    const iot_eth_config_t eth_cfg = {.driver = drv};
    err = iot_eth_install(&eth_cfg, &s_eth);
    if (err != ESP_OK) {
        return err;
    }

    const esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    const esp_netif_config_t netif_cfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    iot_eth_netif_glue_handle_t glue = netif ? iot_eth_new_netif_glue(s_eth) : NULL;
    if (!glue) {
        return ESP_ERR_NO_MEM;
    }
    s_netif = netif;
    /* Before the first DHCP DISCOVER, so the lease is registered under the
     * panel's name from the start. */
    char name[NAME_MAX_LEN + 1];
    identity_name(name);
    net_set_hostname(name);
    err = esp_netif_attach(netif, glue);
    if (err == ESP_OK) err = iot_eth_start(s_eth);
    return err;
}

void net_get_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->link_up = atomic_load(&s_link_up);
    out->has_ip = atomic_load(&s_has_ip);
    out->ip = atomic_load(&s_ip);
    if (out->link_up) {
        memcpy(out->mac, s_mac, sizeof(s_mac));
    }
    uint32_t id = atomic_load(&s_usb_id);
    out->usb_vid = id >> 16;
    out->usb_pid = id & 0xFFFF;
    out->usb_configs = atomic_load(&s_usb_configs);
    out->link_downs = atomic_load(&s_link_downs);
    out->ip_acquired = atomic_load(&s_ip_acquired);
}

void net_set_hostname(const char *name)
{
    if (s_netif) {
        esp_err_t err = esp_netif_set_hostname(s_netif, name);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set hostname '%s': %s", name, esp_err_to_name(err));
        }
    }
}
