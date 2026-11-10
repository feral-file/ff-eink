/*
 * Wired network bring-up. The only implementation today is net_usb.c (USB
 * host + CDC-ECM NIC); a W5500 fallback would be another file behind this
 * same interface.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool link_up;
    bool has_ip;
    uint32_t ip;          /* network byte order, 0 if none */
    uint8_t mac[6];       /* the NIC's own MAC (ECM iMACAddress), zeros until known */
    uint16_t usb_vid;     /* last USB device enumerated, 0 if none */
    uint16_t usb_pid;
    uint8_t usb_configs;  /* its bNumConfigurations */
    uint32_t link_downs;  /* link-up -> link-down transitions since boot */
    uint32_t ip_acquired; /* DHCP leases obtained since boot */
} net_status_t;

/* Takes the USB PHY from USB-Serial-JTAG: after this the board cannot be
 * reflashed over USB until the next boot's 60 s grace window (see main.c). */
esp_err_t net_start(void);

/* Lock-free snapshot. */
void net_get_status(net_status_t *out);
