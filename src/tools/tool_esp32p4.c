/*
 * Copyright (C) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "tools/tool_esp32p4.h"
#include "agent_compat.h"

#include <arpa/inet.h>
#include <malloc.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "cJSON.h"
#include "netutils/netlib.h"

#define ESP32P4_NETDEV "eth0"

static int json_write(cJSON *root, char *output, size_t output_size)
{
    char *json;

    if (!root || !output || output_size == 0) {
        cJSON_Delete(root);
        return ERROR;
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    strlcpy(output, json, output_size);
    free(json);
    return OK;
}

static void add_ipv4(cJSON *root, const char *name, struct in_addr *addr,
                     int status)
{
    char text[INET_ADDRSTRLEN];

    if (status == OK && inet_ntop(AF_INET, addr, text, sizeof(text))) {
        cJSON_AddStringToObject(root, name, text);
    } else {
        cJSON_AddNullToObject(root, name);
    }
}

int tool_device_info_execute(const char *input_json, char *output,
                             size_t output_size)
{
    struct mallinfo memory;
    struct timespec uptime;
    struct utsname system;
    cJSON *root;

    (void)input_json;

    if (uname(&system) < 0 || clock_gettime(CLOCK_MONOTONIC, &uptime) < 0) {
        snprintf(output, output_size,
                 "{\"error\":\"failed to read system information\"}");
        return ERROR;
    }

    memory = mallinfo();
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "board", "esp32p4-function-ev-board");
    cJSON_AddStringToObject(root, "os", "openvela");
    cJSON_AddStringToObject(root, "kernel", system.sysname);
    cJSON_AddStringToObject(root, "release", system.release);
    cJSON_AddStringToObject(root, "version", system.version);
    cJSON_AddStringToObject(root, "architecture", system.machine);
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)uptime.tv_sec);
    cJSON_AddNumberToObject(root, "heap_total_bytes", memory.arena);
    cJSON_AddNumberToObject(root, "heap_used_bytes", memory.uordblks);
    cJSON_AddNumberToObject(root, "heap_free_bytes", memory.fordblks);
    cJSON_AddNumberToObject(root, "heap_largest_free_bytes", memory.mxordblk);

    return json_write(root, output, output_size);
}

int tool_network_status_execute(const char *input_json, char *output,
                                size_t output_size)
{
    struct in_addr address;
    struct in_addr gateway;
    struct in_addr netmask;
    uint8_t mac[6];
    uint8_t flags = 0;
    int address_status;
    int gateway_status;
    int netmask_status;
    int mac_status;
    int flags_status;
    cJSON *root;
    char mac_text[18];

    (void)input_json;

    flags_status = netlib_getifstatus(ESP32P4_NETDEV, &flags);
    address_status = netlib_get_ipv4addr(ESP32P4_NETDEV, &address);
    netmask_status = netlib_get_ipv4netmask(ESP32P4_NETDEV, &netmask);
    gateway_status = netlib_get_dripv4addr(ESP32P4_NETDEV, &gateway);
    mac_status = netlib_getmacaddr(ESP32P4_NETDEV, mac);

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "interface", ESP32P4_NETDEV);
    cJSON_AddBoolToObject(root, "registered", flags_status == OK);
    cJSON_AddBoolToObject(root, "up",
                          flags_status == OK && (flags & IFF_UP) != 0);
    cJSON_AddBoolToObject(root, "running",
                          flags_status == OK && (flags & IFF_RUNNING) != 0);

    if (mac_status == OK) {
        snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(root, "mac", mac_text);
    } else {
        cJSON_AddNullToObject(root, "mac");
    }

    add_ipv4(root, "ipv4", &address, address_status);
    add_ipv4(root, "netmask", &netmask, netmask_status);
    add_ipv4(root, "gateway", &gateway, gateway_status);
    cJSON_AddBoolToObject(root, "has_ipv4",
                          address_status == OK && address.s_addr != INADDR_ANY);

    return json_write(root, output, output_size);
}
