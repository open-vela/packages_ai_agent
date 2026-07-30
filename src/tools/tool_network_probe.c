/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tools/tool_network_probe.h"
#include "agent_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#define DNS_PORT 53
#define DNS_PROBE_COUNT 2
#define DNS_PROBE_TIMEOUT_MS 1200
#ifdef CONFIG_NETDB_RESOLVCONF_PATH
#define RESOLV_CONF_PATH CONFIG_NETDB_RESOLVCONF_PATH
#else
#define RESOLV_CONF_PATH "/etc/resolv.conf"
#endif

static const char* const g_dns_candidates[] = {
    "223.5.5.5",
    "119.29.29.29",
    "1.1.1.1",
    "8.8.8.8",
};

static const uint8_t g_dns_query[] = {
    0xab,
    0xcd,
    0x01,
    0x00,
    0x00,
    0x01,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x03,
    'w',
    'w',
    'w',
    0x07,
    'e',
    'x',
    'a',
    'm',
    'p',
    'l',
    'e',
    0x03,
    'c',
    'o',
    'm',
    0x00,
    0x00,
    0x01,
    0x00,
    0x01,
};

struct dns_result_s {
    const char* address;
    long latency_ms;
};

static long timespec_diff_ms(const struct timespec* start,
    const struct timespec* end)
{
    long elapsed;

    elapsed = (end->tv_sec - start->tv_sec) * 1000L;
    elapsed += (end->tv_nsec - start->tv_nsec) / 1000000L;
    return elapsed < 0 ? 0 : elapsed;
}

static long probe_dns_once(const char* address)
{
    struct sockaddr_in server;
    struct timespec start;
    struct timespec end;
    struct timeval timeout;
    fd_set readfds;
    uint8_t response[64];
    ssize_t size;
    int fd;
    int ret;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(DNS_PORT);
    if (inet_pton(AF_INET, address, &server.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    size = sendto(fd, g_dns_query, sizeof(g_dns_query), 0,
        (struct sockaddr*)&server, sizeof(server));
    if (size != (ssize_t)sizeof(g_dns_query)) {
        close(fd);
        return -1;
    }

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    timeout.tv_sec = DNS_PROBE_TIMEOUT_MS / 1000;
    timeout.tv_usec = (DNS_PROBE_TIMEOUT_MS % 1000) * 1000;

    ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
    if (ret <= 0) {
        close(fd);
        return -1;
    }

    size = recv(fd, response, sizeof(response), 0);
    clock_gettime(CLOCK_MONOTONIC, &end);
    close(fd);

    if (size < 2 || response[0] != g_dns_query[0] || response[1] != g_dns_query[1]) {
        return -1;
    }

    return timespec_diff_ms(&start, &end);
}

static long probe_dns(const char* address)
{
    long best = -1;
    int i;

    for (i = 0; i < DNS_PROBE_COUNT; i++) {
        long latency = probe_dns_once(address);

        if (latency >= 0 && (best < 0 || latency < best)) {
            best = latency;
        }
    }

    return best;
}

static int update_resolver(const char* primary, const char* secondary)
{
    FILE* stream;

    stream = fopen(RESOLV_CONF_PATH, "w");
    if (stream == NULL) {
        return -errno;
    }

    fprintf(stream, "nameserver %s\n", primary);
    if (secondary != NULL) {
        fprintf(stream, "nameserver %s\n", secondary);
    }

    if (fclose(stream) != 0) {
        return -errno;
    }

    return OK;
}

static void sort_results(struct dns_result_s* results, size_t count)
{
    size_t i;
    size_t j;

    for (i = 0; i + 1 < count; i++) {
        size_t best = i;

        for (j = i + 1; j < count; j++) {
            if (results[j].latency_ms >= 0 && (results[best].latency_ms < 0 || results[j].latency_ms < results[best].latency_ms)) {
                best = j;
            }
        }

        if (best != i) {
            struct dns_result_s swap = results[i];

            results[i] = results[best];
            results[best] = swap;
        }
    }
}

int tool_network_probe_execute(const char* input_json, char* output,
    size_t output_size)
{
    struct dns_result_s results[nitems(g_dns_candidates)];
    const char* secondary = NULL;
    cJSON* probe_results;
    cJSON* request;
    cJSON* root;
    cJSON* item;
    char* serialized;
    bool apply = false;
    bool updated = false;
    int reachable = 0;
    int update_result = 0;
    size_t i;

    request = cJSON_Parse(input_json != NULL ? input_json : "{}");
    if (request == NULL) {
        snprintf(output, output_size, "{\"error\":\"invalid JSON\"}");
        return ERROR;
    }

    item = cJSON_GetObjectItemCaseSensitive(request, "apply");
    if (cJSON_IsBool(item)) {
        apply = cJSON_IsTrue(item);
    }

    cJSON_Delete(request);

    for (i = 0; i < nitems(g_dns_candidates); i++) {
        results[i].address = g_dns_candidates[i];
        results[i].latency_ms = probe_dns(results[i].address);
        if (results[i].latency_ms >= 0) {
            reachable++;
        }
    }

    sort_results(results, nitems(results));
    if (reachable == 0) {
        snprintf(output, output_size,
            "{\"error\":\"no DNS server responded\",\"reachable\":0}");
        return ERROR;
    }

    if (reachable > 1) {
        secondary = results[1].address;
    }

    if (apply) {
        update_result = update_resolver(results[0].address, secondary);
        updated = update_result == OK;
    }

    root = cJSON_CreateObject();
    probe_results = cJSON_CreateArray();
    if (root == NULL || probe_results == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(probe_results);
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    cJSON_AddNumberToObject(root, "reachable", reachable);
    cJSON_AddStringToObject(root, "recommended_primary", results[0].address);
    if (secondary != NULL) {
        cJSON_AddStringToObject(root, "recommended_secondary", secondary);
    }

    cJSON_AddBoolToObject(root, "apply_requested", apply);
    cJSON_AddBoolToObject(root, "config_updated", updated);
    if (apply && !updated) {
        cJSON_AddNumberToObject(root, "update_error", update_result);
    }

    for (i = 0; i < nitems(results); i++) {
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "server", results[i].address);
        if (results[i].latency_ms >= 0) {
            cJSON_AddNumberToObject(item, "latency_ms",
                results[i].latency_ms);
        } else {
            cJSON_AddNullToObject(item, "latency_ms");
        }

        cJSON_AddItemToArray(probe_results, item);
    }

    cJSON_AddItemToObject(root, "probe_results", probe_results);
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    snprintf(output, output_size, "%s", serialized);
    free(serialized);
    return apply && !updated ? ERROR : OK;
}
