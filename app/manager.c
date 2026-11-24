/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 */
#include "manager.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <curl/curl.h>
#include <gio/gio.h>
#include <jansson.h>

/* Local VAPIX for ACAP applications: plain HTTP on this loopback address,
 * with credentials the system hands out over D-Bus. */
#define VAPIX_HOST "http://127.0.0.12"
#define PASS_MS 15000       /* a full look at the device */
#define REPLY_MAX (512 * 1024)
#define GIVE_UP_AFTER 3     /* failed first looks before Auto falls back to Pro */
#define PARAM_NAME_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"

#define SOAP_HEAD                                                                             \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"                                              \
    "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""                   \
    " xmlns:aa=\"http://www.axis.com/vapix/ws/action1\""                                      \
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""                                      \
    " xmlns:tns1=\"http://www.onvif.org/ver10/topics\""                                       \
    " xmlns:tnsaxis=\"http://www.axis.com/2009/event/topics\"><soap:Body>"
#define SOAP_TAIL "</soap:Body></soap:Envelope>"

struct reply {
    char* data;
    size_t len;
};

static pthread_mutex_t g_mgr_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mgr_cond;
static pthread_t g_mgr_thread;
static bool g_mgr_started;
/* Guarded by g_mgr_mu. */
static bool g_mgr_stop;
static enum mgr_mode g_mode;
static bool g_mode_changed;
static bool g_look_now;
static int64_t g_source_check_at; /* 0 = none asked for */
static struct mgr_snapshot g_snap;
/* Manager thread only. */
static char g_credentials[128];
static int g_failed_looks;

static int64_t mono_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------- local VAPIX */

static bool fetch_credentials(char* err, size_t err_len) {
    GError* error               = NULL;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    GVariant* result;
    const char* text = NULL;

    if (connection == NULL) {
        snprintf(err, err_len, "No system bus: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
        return false;
    }
    result = g_dbus_connection_call_sync(connection, "com.axis.HTTPConf1", "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
                                         "com.axis.HTTPConf1.VAPIXServiceAccounts1", "GetCredentials",
                                         g_variant_new("(s)", "lineinrtp"), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                         &error);
    g_object_unref(connection);
    if (result == NULL) {
        snprintf(err, err_len, "No VAPIX credentials: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
        return false;
    }
    g_variant_get(result, "(&s)", &text);
    if (text == NULL || strchr(text, ':') == NULL || strlen(text) >= sizeof g_credentials) {
        snprintf(err, err_len, "Unexpected VAPIX credentials format");
        g_variant_unref(result);
        return false;
    }
    snprintf(g_credentials, sizeof g_credentials, "%s", text);
    g_variant_unref(result);
    return true;
}

static size_t on_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    struct reply* r = userdata;
    size_t add      = size * nmemb;
    char* grown;

    if (r->len + add > REPLY_MAX) {
        return 0; /* aborts the transfer */
    }
    grown = realloc(r->data, r->len + add + 1);
    if (grown == NULL) {
        return 0;
    }
    r->data = grown;
    memcpy(r->data + r->len, ptr, add);
    r->len += add;
    r->data[r->len] = '\0';
    return add;
}

/* One request to the device's own web server. Returns the HTTP status, or -1
 * if there was no usable answer. *out is always a string the caller frees. */
static long vapix(const char* path, const char* content_type, const char* body, char** out) {
    struct reply r           = {NULL, 0};
    struct curl_slist* hdrs  = NULL;
    CURL* curl               = curl_easy_init();
    long code                = -1;
    char url[256];

    *out = NULL;
    if (curl == NULL) {
        return -1;
    }
    snprintf(url, sizeof url, "%s%s", VAPIX_HOST, path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, g_credentials);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 6000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    if (body != NULL) {
        char header[96];
        snprintf(header, sizeof header, "Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, header);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (r.data == NULL) {
        r.data = calloc(1, 1);
    }
    *out = r.data;
    if (code == 401) {
        g_credentials[0] = '\0'; /* ask for fresh ones on the next pass */
    }
    return r.data != NULL ? code : -1;
}

static long soap(const char* body, char** out) {
    char* envelope;
    size_t len = strlen(SOAP_HEAD) + strlen(body) + strlen(SOAP_TAIL) + 1;
    long code;

    *out     = NULL;
    envelope = malloc(len);
    if (envelope == NULL) {
        return -1;
    }
    snprintf(envelope, len, "%s%s%s", SOAP_HEAD, body, SOAP_TAIL);
    code = vapix("/vapix/services", "application/soap+xml", envelope, out);
    free(envelope);
    return code;
}

/* ------------------------------------------------------------ detection */

/* 1 in a site, 0 not, -1 couldn't tell. */
static int look_for_site(struct mgr_snapshot* s) {
    char* text = NULL;
    long code  = vapix("/vapix/audio-site/v3.0/sites", NULL, NULL, &text);
    int found  = -1;

    if (code == 200) {
        json_t* root = json_loads(text, 0, NULL);
        if (json_is_array(root)) {
            const char* name = json_array_size(root) > 0
                                   ? json_string_value(json_object_get(json_array_get(root, 0), "niceName"))
                                   : NULL;
            found = json_array_size(root) > 0 ? 1 : 0;
            snprintf(s->site_name, sizeof s->site_name, "%s", name != NULL ? name : "");
        }
        json_decref(root);
    }
    free(text);
    if (found >= 0) {
        return found;
    }
    /* Not there, or not what was expected: ask the documented Edge API. */
    code = vapix("/vapix/aam-edge/api/v1.0/targets", NULL, NULL, &text);
    if (code == 200) {
        json_t* root = json_loads(text, 0, NULL);
        if (json_is_array(root)) {
            size_t i;
            found = 0;
            for (i = 0; i < json_array_size(root); i++) {
                const char* type = json_string_value(json_object_get(json_array_get(root, i), "type"));
                if (type != NULL && strcmp(type, "device") == 0) {
                    found = 1;
                }
            }
            s->site_name[0] = '\0';
        }
        json_decref(root);
    } else if (code >= 400 && code < 500 && code != 401) {
        found           = 0; /* the API answers but has no site to show */
        s->site_name[0] = '\0';
    }
    free(text);
    return found;
}

/* 1 running, 0 not, -1 couldn't tell. */
static int look_for_pro(struct mgr_snapshot* s) {
    char* text = NULL;
    long code  = vapix("/axis-cgi/applications/list.cgi", NULL, NULL, &text);
    int found  = -1;

    if (code == 200 && strstr(text, "<reply") != NULL) {
        bool running     = false;
        s->pro_installed = app_list_find_pro(text, s->pro_app, sizeof s->pro_app, &running);
        found            = s->pro_installed && running ? 1 : 0;
    }
    free(text);
    /* The Pro app keeps its server's address in a parameter of its own. It
     * is a likely RTP destination, so the settings page offers it. */
    if (found >= 0) {
        s->pro_server[0] = '\0';
    }
    if (found >= 0 && s->pro_installed && strspn(s->pro_app, PARAM_NAME_CHARS) == strlen(s->pro_app)) {
        char path[160];
        snprintf(path, sizeof path, "/axis-cgi/param.cgi?action=list&group=root.%s.PrimaryServerIpAddress", s->pro_app);
        if (vapix(path, NULL, NULL, &text) == 200) {
            param_value(text, s->pro_server, sizeof s->pro_server);
        }
        free(text);
    }
    return found;
}

/* Reads how Edge sees the line input: whether it is a paging source at all,
 * and whether it is active now. */
static void look_at_source(struct mgr_snapshot* s) {
    char* text = NULL;
    long code  = vapix("/vapix/audio-site/v3.0/sources", NULL, NULL, &text);

    s->line_in       = -1;
    s->source_active = -1;
    if (code == 200) {
        json_t* root = json_loads(text, 0, NULL);
        if (json_is_array(root)) {
            size_t i;
            s->line_in = 0;
            for (i = 0; i < json_array_size(root); i++) {
                json_t* src       = json_array_get(root, i);
                const char* name  = json_string_value(json_object_get(src, "registeredName"));
                const char* klass = json_string_value(json_object_get(src, "class"));
                json_t* active    = json_object_get(json_object_get(src, "status"), "active");

                if (name == NULL || strcmp(name, EDGE_SOURCE) != 0) {
                    continue;
                }
                if (klass != NULL && strcmp(klass, "paging") == 0 && !json_is_false(json_object_get(src, "enabled"))) {
                    s->line_in       = 1;
                    s->source_active = json_is_boolean(active) ? (json_is_true(active) ? 1 : 0) : -1;
                    break; /* a site lists every device's input; a paging one is enough */
                }
            }
        }
        json_decref(root);
    }
    s->source_checked_ms = mono_ms();
    free(text);
}

/* ------------------------------------------------------------ Edge rule */

/* Pulls the number out of <aa:tag>123</aa:tag> in a reply. */
static int reply_id(const char* xml, const char* local) {
    const char* cursor = xml;
    struct xml_el el;
    char text[16];

    if (!xml_next(&cursor, xml + strlen(xml), local, &el) || el.body_len == 0 || el.body_len >= sizeof text) {
        return -1;
    }
    memcpy(text, el.body, el.body_len);
    text[el.body_len] = '\0';
    return atoi(text);
}

static bool create_rule(int action_id, char* err, size_t err_len) {
    char body[1600];
    char* text = NULL;
    long code;
    bool ok;

    if (action_id < 0) {
        code = soap("<aa:AddActionConfiguration><aa:NewActionConfiguration>"
                    "<aa:Name>" EDGE_ACTION_NAME "</aa:Name>"
                    "<aa:TemplateToken>" EDGE_ACTION_TEMPLATE "</aa:TemplateToken>"
                    "<aa:Parameters>"
                    "<aa:Parameter Name=\"activate\" Value=\"1\"/>"
                    "<aa:Parameter Name=\"source\" Value=\"" EDGE_SOURCE "\"/>"
                    "</aa:Parameters>"
                    "</aa:NewActionConfiguration></aa:AddActionConfiguration>",
                    &text);
        action_id = code == 200 && text != NULL ? reply_id(text, "ConfigurationID") : -1;
        free(text);
        text = NULL;
        if (action_id < 0) {
            snprintf(err, err_len, "Could not add the Line-in action (HTTP %ld)", code);
            return false;
        }
    }
    snprintf(body, sizeof body,
             "<aa:AddActionRule><aa:NewActionRule>"
             "<aa:Name>" EDGE_RULE_NAME "</aa:Name>"
             "<aa:Enabled>true</aa:Enabled>"
             "<aa:Conditions><aa:Condition>"
             "<wsnt:TopicExpression Dialect=\"http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete\">"
             EDGE_EVENT_TOPIC "</wsnt:TopicExpression>"
             "<wsnt:MessageContent Dialect=\"http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter\">"
             "boolean(//SimpleItem[@Name=\"" EDGE_EVENT_KEY "\" and @Value=\"1\"])</wsnt:MessageContent>"
             "</aa:Condition></aa:Conditions>"
             "<aa:PrimaryAction>%d</aa:PrimaryAction>"
             "</aa:NewActionRule></aa:AddActionRule>",
             action_id);
    code = soap(body, &text);
    ok   = code == 200 && text != NULL && reply_id(text, "RuleID") >= 0;
    free(text);
    if (!ok) {
        snprintf(err, err_len, "Could not add the paging rule (HTTP %ld)", code);
    }
    return ok;
}

static void ensure_rule(struct mgr_snapshot* s) {
    char* rules   = NULL;
    char* actions = NULL;
    long c1       = soap("<aa:GetActionRules/>", &rules);
    long c2       = soap("<aa:GetActionConfigurations/>", &actions);
    struct edge_rules found;

    if (c1 != 200 || c2 != 200) {
        snprintf(s->error, sizeof s->error, "Could not read the device's rules (HTTP %ld)", c1 != 200 ? c1 : c2);
        free(rules);
        free(actions);
        return;
    }
    edge_rules_scan(rules, actions, &found);
    free(rules);
    free(actions);
    if (found.state == RULE_MISSING) {
        if (create_rule(found.action_id, s->error, sizeof s->error)) {
            found.state     = RULE_OK;
            s->rule_created = true;
            syslog(LOG_INFO, "Added the rule \"%s\"", EDGE_RULE_NAME);
        }
    }
    s->rule        = found.state;
    s->other_rules = found.others;
    snprintf(s->other_rule_names, sizeof s->other_rule_names, "%s", found.other_names);
}

/* ------------------------------------------------------------ the thread */

static void full_pass(struct mgr_snapshot* s, enum mgr_mode mode) {
    enum mgr_active before = s->active;
    int site;
    int pro;

    s->mode     = mode;
    s->error[0] = '\0';
    if (g_credentials[0] == '\0' && !fetch_credentials(s->error, sizeof s->error)) {
        site = -1;
        pro  = -1;
    } else {
        site = look_for_site(s);
        pro  = look_for_pro(s);
        if ((site < 0 || pro < 0) && s->error[0] == '\0') {
            snprintf(s->error, sizeof s->error, "The device did not answer the app's %s request",
                     site < 0 ? "Audio Manager Edge" : "installed apps");
        }
    }
    /* A look that fails keeps what was known, so one missed request doesn't
     * flip the mode. Only the very first looks can give up, and then Auto
     * falls back to Pro. */
    if (site >= 0) {
        s->detect.edge_site = site == 1;
    }
    if (pro >= 0) {
        s->detect.pro_app = pro == 1;
    }
    if (site >= 0 && pro >= 0) {
        s->detect.known = true;
        g_failed_looks  = 0;
    } else if (!s->detect.known && ++g_failed_looks >= GIVE_UP_AFTER) {
        s->detect.known = true;
        syslog(LOG_WARNING, "Could not tell which audio manager this device uses: %s", s->error);
    }
    s->active = mgr_resolve(mode, &s->detect);
    if (s->active != before) {
        syslog(LOG_INFO, "Audio manager: %s (setting %s, Edge site %s, Pro app %s)", mgr_active_name(s->active),
               mgr_mode_name(mode), s->detect.edge_site ? "yes" : "no", s->detect.pro_app ? "running" : "no");
    }
    if (s->active == ACTIVE_EDGE && s->detect.edge_site && g_credentials[0] != '\0') {
        look_at_source(s);
        ensure_rule(s);
    } else {
        s->line_in             = -1;
        s->source_active       = -1;
        s->rule                = RULE_UNKNOWN;
        s->other_rules         = 0;
        s->other_rule_names[0] = '\0';
    }
}

static void wait_until(int64_t when) {
    struct timespec ts;
    int64_t ms = when - mono_ms();

    if (ms <= 0) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(ms / 1000);
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&g_mgr_cond, &g_mgr_mu, &ts);
}

static void* mgr_thread(void* unused) {
    struct mgr_snapshot work;
    int64_t next_pass = 0;

    (void)unused;
    pthread_mutex_lock(&g_mgr_mu);
    work = g_snap;
    while (!g_mgr_stop) {
        int64_t now       = mono_ms();
        bool full         = g_mode_changed || g_look_now || now >= next_pass;
        bool source_only  = !full && g_source_check_at != 0 && now >= g_source_check_at;
        enum mgr_mode mode = g_mode;

        if (!full && !source_only) {
            int64_t wake = next_pass;
            if (g_source_check_at != 0 && g_source_check_at < wake) {
                wake = g_source_check_at;
            }
            wait_until(wake);
            continue;
        }
        if (full) {
            g_mode_changed = false;
            g_look_now     = false;
        }
        if (full || source_only) {
            g_source_check_at = 0;
        }
        pthread_mutex_unlock(&g_mgr_mu);

        if (full) {
            full_pass(&work, mode);
            next_pass = mono_ms() + PASS_MS;
        } else if (work.active == ACTIVE_EDGE && work.detect.edge_site && g_credentials[0] != '\0') {
            look_at_source(&work);
        }

        pthread_mutex_lock(&g_mgr_mu);
        /* The setting may have changed while the device was being read. */
        work.mode   = g_mode;
        work.active = mgr_resolve(g_mode, &work.detect);
        g_snap      = work;
    }
    pthread_mutex_unlock(&g_mgr_mu);
    return NULL;
}

bool mgr_start(enum mgr_mode mode) {
    pthread_condattr_t attr;

    memset(&g_snap, 0, sizeof g_snap);
    g_snap.mode          = mode;
    g_snap.active        = mgr_resolve(mode, &g_snap.detect);
    g_snap.line_in       = -1;
    g_snap.source_active = -1;
    g_mode               = mode;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        g_snap.detect.known = true;
        g_snap.active       = mgr_resolve(mode, &g_snap.detect);
        return false;
    }
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_mgr_cond, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&g_mgr_thread, NULL, mgr_thread, NULL) != 0) {
        g_snap.detect.known = true;
        g_snap.active       = mgr_resolve(mode, &g_snap.detect);
        curl_global_cleanup();
        return false;
    }
    g_mgr_started = true;
    return true;
}

void mgr_stop(void) {
    if (!g_mgr_started) {
        return;
    }
    pthread_mutex_lock(&g_mgr_mu);
    g_mgr_stop = true;
    pthread_cond_signal(&g_mgr_cond);
    pthread_mutex_unlock(&g_mgr_mu);
    pthread_join(g_mgr_thread, NULL);
    g_mgr_started = false;
    curl_global_cleanup();
}

void mgr_set_mode(enum mgr_mode mode) {
    pthread_mutex_lock(&g_mgr_mu);
    if (mode != g_mode) {
        g_mode         = mode;
        g_mode_changed = true;
        /* Forced modes, and Auto once the device has been read, apply now. */
        g_snap.mode   = mode;
        g_snap.active = mgr_resolve(mode, &g_snap.detect);
        if (g_mgr_started) {
            pthread_cond_signal(&g_mgr_cond);
        }
    }
    pthread_mutex_unlock(&g_mgr_mu);
}

void mgr_look_now(void) {
    pthread_mutex_lock(&g_mgr_mu);
    if (g_mgr_started && !g_look_now) {
        g_look_now = true;
        pthread_cond_signal(&g_mgr_cond);
    }
    pthread_mutex_unlock(&g_mgr_mu);
}

void mgr_check_source(int delay_ms) {
    int64_t at = mono_ms() + delay_ms;

    pthread_mutex_lock(&g_mgr_mu);
    if (g_mgr_started && (g_source_check_at == 0 || at < g_source_check_at)) {
        g_source_check_at = at;
        pthread_cond_signal(&g_mgr_cond);
    }
    pthread_mutex_unlock(&g_mgr_mu);
}

void mgr_get(struct mgr_snapshot* out) {
    pthread_mutex_lock(&g_mgr_mu);
    *out = g_snap;
    pthread_mutex_unlock(&g_mgr_mu);
}
