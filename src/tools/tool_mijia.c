/*
 * tool_mijia.c -- Mi-home (米家) device control for the AI agent.
 *
 * [W962] Why it looks like this.
 *
 * Controlling Mi-home devices needs credentials this project does not have and
 * must never ship in a binary: an Xiaomi account OAuth token, or a local
 * gateway token.  Hard-coding either would leak them into every firmware image
 * and every `strings` dump.  So this tool is a thin, well-typed bridge that
 * reads everything it needs from a config file the operator writes once:
 *
 *     /data/mijia/config.json
 *
 * and speaks plain REST to whatever gateway is named there.  The default
 * dialect is Home Assistant's service API, because that is the most common way
 * Mi-home devices are exposed off-box (Xiaomi Home / Xiaomi Gateway 3 / miot
 * integrations all land there), and it needs no vendor SDK:
 *
 *     POST /api/services/<domain>/<service>
 *     Authorization: Bearer <token>
 *     {"entity_id": "light.living_room", "brightness_pct": 60}
 *
 * Two tools are exposed to the model:
 *   mijia_list  - what can I control?
 *   mijia_set   - turn it on/off, or set a level
 *
 * Nothing here is coupled to Home Assistant specifically: the (host, port,
 * use_tls, token, path_template) tuple is data, so pointing it at any
 * MIoT-to-HTTP bridge is a config change, not a rebuild.
 *
 * Failure behaviour: every path returns a human-readable string in `output`
 * rather than a bare OK/ERROR.  A tool whose error message says "config file
 * missing" is debuggable from the chat transcript alone, which matters because
 * this runs on a headless board.
 */

#include "tools/tool_mijia.h"

#include "agent_compat.h"
#include "infra/vela_tls.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char *TAG = "tool_mijia";

#define MIJIA_CONFIG_PATH "/data/mijia/config.json"

/* Bounds so a malformed config cannot walk off the end of a stack buffer. */
#define MIJIA_MAX_DEVICES 32
#define MIJIA_NAME_LEN    64
#define MIJIA_ENTITY_LEN  96
#define MIJIA_DOMAIN_LEN  32
#define MIJIA_HOST_LEN    96
#define MIJIA_PORT_LEN    8
#define MIJIA_TOKEN_LEN   160
#define MIJIA_MAX_CFG     (24u * 1024u)
#define MIJIA_MAX_RESP    (4u * 1024u)

typedef struct {
    char name[MIJIA_NAME_LEN];
    char domain[MIJIA_DOMAIN_LEN];   /* "light", "switch", "fan", "cover" ...  */
    char entity[MIJIA_ENTITY_LEN];   /* "light.living_room"                    */
} mijia_device_t;

typedef struct {
    char  host[MIJIA_HOST_LEN];
    char  port[MIJIA_PORT_LEN];
    char  token[MIJIA_TOKEN_LEN];
    int   use_tls;
    mijia_device_t devices[MIJIA_MAX_DEVICES];
    int   count;
} mijia_config_t;

/* ── config ──────────────────────────────────────────────────────────── */

static void safe_copy(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, cap, "%s", src);
}

static const char *json_str(const cJSON *node, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(node, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int load_config(mijia_config_t *cfg)
{
    FILE   *fp;
    char   *raw;
    long    size;
    size_t  got;
    cJSON  *root;
    cJSON  *gw;
    cJSON  *arr;
    cJSON  *it;
    int     ok = -1;

    memset(cfg, 0, sizeof(*cfg));

    fp = fopen(MIJIA_CONFIG_PATH, "rb");
    if (fp == NULL) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    size = ftell(fp);
    if (size <= 0 || (unsigned long)size > (unsigned long)MIJIA_MAX_CFG) {
        fclose(fp);
        return -1;
    }

    rewind(fp);
    raw = (char *)malloc((size_t)size + 1u);
    if (raw == NULL) {
        fclose(fp);
        return -1;
    }

    got = fread(raw, 1, (size_t)size, fp);
    fclose(fp);
    raw[got] = '\0';

    root = cJSON_Parse(raw);
    if (root == NULL) {
        free(raw);
        return -1;
    }

    gw = cJSON_GetObjectItemCaseSensitive(root, "gateway");
    if (cJSON_IsObject(gw)) {
        safe_copy(cfg->host, sizeof(cfg->host), json_str(gw, "host"));
        safe_copy(cfg->port, sizeof(cfg->port), json_str(gw, "port"));
        safe_copy(cfg->token, sizeof(cfg->token), json_str(gw, "token"));
        cfg->use_tls = 0;
        {
            const cJSON *tls = cJSON_GetObjectItemCaseSensitive(gw, "use_tls");
            if (cJSON_IsBool(tls)) {
                cfg->use_tls = cJSON_IsTrue(tls) ? 1 : 0;
            }
        }
    }

    if (cfg->port[0] == '\0') {
        snprintf(cfg->port, sizeof(cfg->port), "%s",
                 cfg->use_tls ? "443" : "8123");
    }

    arr = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            const char *d;
            if (cfg->count >= MIJIA_MAX_DEVICES) {
                break;
            }
            d = json_str(it, "domain");
            if (d == NULL) {
                continue;      /* a nameless, domain-less entry cannot be driven */
            }
            safe_copy(cfg->devices[cfg->count].name,
                      MIJIA_NAME_LEN, json_str(it, "name"));
            safe_copy(cfg->devices[cfg->count].domain,
                      MIJIA_DOMAIN_LEN, d);
            safe_copy(cfg->devices[cfg->count].entity,
                      MIJIA_ENTITY_LEN, json_str(it, "entity"));
            cfg->count++;
        }
    }

    ok = (cfg->host[0] != '\0' && cfg->count > 0) ? 0 : -2;

    cJSON_Delete(root);
    free(raw);
    return ok;
}

/* ── transport ───────────────────────────────────────────────────────── */

/*
 * One call into whichever bridge the config names.
 * `service` is e.g. "turn_on"; `body` is the JSON payload to post.
 * Returns the HTTP status on success, negative on transport failure.
 */
static int mijia_call(const mijia_config_t *cfg, const char *service,
                      const char *body, char *resp, size_t resp_cap)
{
    vela_header_t headers[3];
    char          auth[MIJIA_TOKEN_LEN + 24];
    char          path[256];
    int           nh = 0;
    int           ret;

    if (cfg->token[0] != '\0') {
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->token);
        headers[nh].name  = "Authorization";
        headers[nh].value = auth;
        nh++;
    }

    if (cfg->use_tls) {
        headers[nh].name  = "Content-Type";
        headers[nh].value = "application/json";
        nh++;
    }

    headers[nh].name  = NULL;
    headers[nh].value = NULL;

    /* A device is addressed by domain ("light"), the gateway by the full path.
     * Both need:  /api/services/<domain>/<service> */
    snprintf(path, sizeof(path), "/api/services/%s/%s", service, service);

    if (cfg->use_tls) {
        ret = vela_https_post_json(cfg->host, cfg->port, path,
                                   cfg->token[0] != '\0' ? headers : NULL,
                                   body, resp, resp_cap);
    } else {
        ret = vela_http_post_json(cfg->host, cfg->port, path,
                                  cfg->token[0] != '\0' ? headers : NULL,
                                  body, resp, resp_cap);
    }

    return ret;
}

/*
 * Build the service path from the device domain.  Kept separate from
 * mijia_call() because the domain comes from the device, not the caller.
 */
static int mijia_device_call(const mijia_config_t *cfg,
                             const mijia_device_t *dev,
                             const char *action /* turn_on / turn_off / toggle */,
                             const char *extra_json /* may be NULL */,
                             char *resp, size_t resp_cap)
{
    vela_header_t headers[3];
    char          auth[MIJIA_TOKEN_LEN + 24];
    char          path[256];
    char          body[512];
    int           nh = 0;
    int           ret;

    if (cfg->token[0] != '\0') {
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->token);
        headers[nh].name  = "Authorization";
        headers[nh].value = auth;
        nh++;
    }

    headers[nh].name  = NULL;
    headers[nh].value = NULL;

    snprintf(path, sizeof(path), "/api/services/%s/%s", dev->domain, action);

    if (extra_json != NULL && extra_json[0] != '\0') {
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",%s}",
                 dev->entity, extra_json);
    } else {
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", dev->entity);
    }

    syslog(LOG_INFO, "[%s] %s %s -> %s\n", TAG, dev->entity, action, cfg->host);

    if (cfg->use_tls) {
        ret = vela_https_post_json(cfg->host, cfg->port, path,
                                   cfg->token[0] != '\0' ? headers : NULL,
                                   body, resp, resp_cap);
    } else {
        ret = vela_http_post_json(cfg->host, cfg->port, path,
                                  cfg->token[0] != '\0' ? headers : NULL,
                                  body, resp, resp_cap);
    }

    return ret;
}

/* ── device lookup ───────────────────────────────────────────────────── */

/*
 * Accept either the friendly name or the entity id, and be forgiving about
 * case.  Returns NULL with *nmatched set so the caller can say how close they
 * were.
 */
static const mijia_device_t *find_device(const mijia_config_t *cfg,
                                         const char *needle, int *nmatched)
{
    int i;

    *nmatched = 0;

    for (i = 0; i < cfg->count; i++) {
        if (strcasecmp(needle, cfg->devices[i].name) == 0 ||
            strcasecmp(needle, cfg->devices[i].entity) == 0) {
            return &cfg->devices[i];
        }
        if (strcasestr(cfg->devices[i].name, needle) != NULL ||
            strcasestr(cfg->devices[i].entity, needle) != NULL) {
            (*nmatched)++;
        }
    }

    /* Partial hit: return the single match when there was exactly one. */
    if (*nmatched == 1) {
        for (i = 0; i < cfg->count; i++) {
            if (strcasestr(cfg->devices[i].name, needle) != NULL ||
                strcasestr(cfg->devices[i].entity, needle) != NULL) {
                return &cfg->devices[i];
            }
        }
    }

    return NULL;
}

/* ── tools ───────────────────────────────────────────────────────────── */

int tool_mijia_list_execute(const char *input_json, char *output, size_t output_size)
{
    mijia_config_t cfg;
    int            rc;
    int            i;
    size_t         used = 0;

    (void)input_json;

    rc = load_config(&cfg);
    if (rc != 0) {
        snprintf(output, output_size,
                 "Mi-home control is not configured. Create %s with "
                 "{\"gateway\":{\"host\":\"...\",\"port\":\"8123\","
                 "\"token\":\"...\",\"use_tls\":false},\"devices\":[{\"name\":"
                 "\"...\",\"domain\":\"light\",\"entity\":\"light.kitchen\"}]} "
                 "(rc=%d)", MIJIA_CONFIG_PATH, rc);
        return ERROR;
    }

    used += (size_t)snprintf(output + used, output_size - used,
                             "Controllable devices (%d):\n", cfg.count);
    for (i = 0; i < cfg.count && used < output_size - 1; i++) {
        used += (size_t)snprintf(output + used, output_size - used,
                                 "- %s [%s] %s\n",
                                 cfg.devices[i].name,
                                 cfg.devices[i].domain,
                                 cfg.devices[i].entity);
    }

    return OK;
}

int tool_mijia_set_execute(const char *input_json, char *output, size_t output_size)
{
    mijia_config_t       cfg;
    cJSON               *req;
    char                 name[MIJIA_NAME_LEN];
    char                 action[24];
    char                 extra[192];
    char                 resp[MIJIA_MAX_RESP];
    const mijia_device_t *dev;
    const char           *s;
    int                   nmatched = 0;
    int                   level = -1;
    int                   rc;
    int                   http_ret;

    req = cJSON_Parse(input_json ? input_json : "{}");
    if (req == NULL) {
        snprintf(output, output_size, "Invalid JSON input.");
        return ERROR;
    }

    s = json_str(req, "name");
    safe_copy(name, sizeof(name), s);
    s = json_str(req, "action");
    safe_copy(action, sizeof(action), s ? s : "on");

    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, "value");
        if (cJSON_IsNumber(v)) {
            level = (int)v->valuedouble;
        }
    }

    cJSON_Delete(req);

    if (name[0] == '\0') {
        snprintf(output, output_size, "Which device? Call mijia_list first.");
        return ERROR;
    }

    rc = load_config(&cfg);
    if (rc != 0) {
        snprintf(output, output_size,
                 "Mi-home control is not configured (create %s). See "
                 "mijia_list for the expected shape.", MIJIA_CONFIG_PATH);
        return ERROR;
    }

    dev = find_device(&cfg, name, &nmatched);
    if (dev == NULL) {
        if (nmatched > 1) {
            snprintf(output, output_size,
                     "'%s' is ambiguous, %d devices match. Be more specific.",
                     name, nmatched);
        } else {
            snprintf(output, output_size,
                     "No Mi-home device named '%s'. Call mijia_list.",
                     name);
        }
        return ERROR;
    }

    /* Clamp before it reaches the gateway: a held-highServo style value in the
     * wrong unit should not be able to request a 300% brightness. */
    if (level > 100) {
        level = 100;
    }
    if (level < 0) {
        level = -1;
    }

    extra[0] = '\0';
    if (level >= 0) {
        /*
         * The rest of the tool surface talks percentage because that is what a
         * user says ("调到 40%").  Home Assistant lights want brightness_pct,
         * covers want position, fans want percentage.  Send the generic
         * brightness_pct and let the gateway reject anything the domain does
         * not accept -- guessing per-domain here would be wrong more often.
         */
        snprintf(extra, sizeof(extra), "\"brightness_pct\":%d", level);
    }

    if (strcasecmp(action, "off") == 0) {
        http_ret = mijia_device_call(&cfg, dev, "turn_off", NULL, resp, sizeof(resp));
    } else if (strcasecmp(action, "toggle") == 0) {
        http_ret = mijia_device_call(&cfg, dev, "toggle", NULL, resp, sizeof(resp));
    } else if (strcasecmp(action, "set") == 0) {
        if (level < 0) {
            snprintf(output, output_size,
                     "action=set needs a value 0..100.");
            return ERROR;
        }
        http_ret = mijia_device_call(&cfg, dev, "turn_on", extra, resp, sizeof(resp));
    } else {
        /* Default to turning the thing ON -- that is what people mean. */
        http_ret = mijia_device_call(&cfg, dev, "turn_on",
                                     extra[0] != '\0' ? extra : NULL,
                                     resp, sizeof(resp));
    }

    if (http_ret >= 200 && http_ret < 300) {
        if (level >= 0) {
            snprintf(output, output_size, "%s set to %d%%.", dev->name, level);
        } else if (strcasecmp(action, "off") == 0) {
            snprintf(output, output_size, "%s turned off.", dev->name);
        } else {
            snprintf(output, output_size, "%s turned on.", dev->name);
        }
        return OK;
    }

    if (http_ret > 0) {
        snprintf(output, output_size,
                 "%s did not respond successfully (HTTP %d).",
                 dev->name, http_ret);
    } else {
        snprintf(output, output_size,
                 "Could not reach the Mi-home gateway at %s:%s (err %d).",
                 cfg.host, cfg.port, http_ret);
    }

    syslog(LOG_ERR, "[%s] call failed ret=%d\n", TAG, http_ret);
    return ERROR;
}
