/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 *
 * Captures the C8110 line input and runs a gate on it: open while the level
 * stays above a threshold, shaped by delay, hold and maximum-on times.
 *
 * With AXIS Audio Manager Pro the app sends unicast RTP (G.711 A-law or L16)
 * while the gate is open. Pro's RTP sources listen; packets stop when the
 * input goes quiet so the server's activity detector sees the source go idle.
 *
 * With AXIS Audio Manager Edge nothing is sent. The app raises an event while
 * the gate is open, and a rule on the device turns that into "Activate
 * Line-in", so Edge pages from the line input itself. manager.c works out
 * which of the two applies and keeps that rule in place.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

#include <axsdk/axevent.h>
#include <axsdk/axparameter.h>
#include <glib.h>
#include <jansson.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#pragma GCC diagnostic pop

#include "manager.h"
#include "mgrcore.h"
#include "rtpcore.h"

#define APP_NAME "lineinrtp"
#define HTTP_PORT 2011
#define NODE_MAX 32
/* The ring holds 200 ms at the stream's rate: 1,600 samples for G.711,
 * 9,600 for L16 at 48 kHz. That's also the longest pre-roll. */
#define RING_MAX 9600U
#define DEFAULT_NODE "AudioDevice0Input0.Unprocessed"
/* 20 ms entries, about one PipeWire buffer each, so the page's preview
 * replays nearly what the gate saw. 2048 entries is 41 s: the 24 s chart is
 * full on first load with warm-up for the preview. A power of two keeps
 * seq % HIST_LEN continuous across wrap. */
#define HIST_LEN 2048U
#define HIST_INTERVAL_MS 20
#define CFG_RELOAD_MS 1000
#define AUDIO_WATCHDOG_MS 5000
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000

struct cfg {
    bool enabled;
    bool force_on;
    bool test_mode;
    int trigger_port;
    char destination_ip[64];
    int destination_port;
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
    char input_node[NAME_MAX_LEN + 1];
    int codec;        /* enum rtp_codec */
    int sample_rate;  /* L16 only: 16000 or 48000 */
    int payload_type; /* L16 only: 96-127 */
    int manager;      /* enum mgr_mode */
};

/* HTTP thread -> control thread. */
struct pending_cfg {
    bool have;
    unsigned rev;
    struct cfg cfg;
};

/* Control thread -> main loop. The main loop never touches AXParameter
 * itself, so it never blocks on D-Bus while audio is flowing. */
struct ctl_update {
    bool have_cfg;
    struct cfg cfg;
    char cfg_error[160];
    unsigned rev;
    char save_error[160];
};

/* One error slot per subsystem; see set_error(). */
enum err_slot { ERR_CONFIG, ERR_CAPTURE, ERR_NETWORK, ERR_SLOTS };

struct status_snap {
    double level_db;
    char gate[16];
    bool streaming;
    bool enabled;
    bool force_on;
    bool test_mode;
    bool node_connected;
    int trigger_port;
    int port1_active;
    int port2_active;
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
    int destination_port;
    int codec;
    int sample_rate;
    int payload_type;
    struct rtp_format fmt; /* what is on the wire now */
    double level_percent;
    int capture_rate;
    int capture_channels;
    char destination_ip[64];
    char stream_url[96];
    char input_node[NAME_MAX_LEN + 1];
    char connected_node[NAME_MAX_LEN + 1];
    char last_error[160];
    char errors[ERR_SLOTS][160];
    char last_rtp_header[25];
    char config_error[160];
    unsigned config_rev;
    uint64_t packets_sent;
    uint64_t send_errors;
    uint64_t samples_dropped;
    unsigned buffered_samples;
    int manager;             /* the setting, enum mgr_mode */
    struct mgr_snapshot mgr; /* what manager.c found */
    bool edge_paging;        /* the Edge paging event is raised */
    int edge_confirmed;      /* 1: Edge reports the page active; 0: it doesn't; -1: not checked yet */
};

struct node_ref {
    bool used;
    uint32_t id;
    char name[NAME_MAX_LEN + 1];
};

struct impl {
    struct pw_main_loop* loop;
    struct pw_context* context;
    struct pw_core* core;
    struct pw_registry* registry;
    struct spa_hook core_listener;
    bool core_hooked;
    struct spa_hook registry_listener;
    struct spa_source* timer_source;
    struct pw_stream* stream;
    struct spa_hook stream_listener;
    struct spa_audio_info info;
    bool format_ready;
    struct node_ref nodes[NODE_MAX];
    char connected_name[NAME_MAX_LEN + 1];
    uint32_t connected_id;
    AXParameter* params;
    int http_listen_fd;
    pthread_t http_thread;
    pthread_t control_thread;
    bool control_started;
};

static struct impl g_impl;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
/* Guarded by g_mu. */
static pthread_cond_t g_ctl_cond;
static bool g_ctl_stop;
static struct pending_cfg g_pending;
static struct ctl_update g_ctl;
static unsigned g_cfg_rev_requested;
static struct status_snap g_status;
/* Main loop only. */
static unsigned g_cfg_rev_applied;
static char g_cfg_save_error[160];
static bool g_stream_failed;
static int64_t g_retry_at;
static int g_retry_ms = RETRY_MIN_MS;
static int64_t g_last_process_ms;
static int64_t g_connected_ms;
static int g_exit_code = EXIT_SUCCESS;
/* History ring: written by the main loop, read by the HTTP thread, guarded
 * by g_mu. It lives outside g_status so the 10 ms status publish doesn't copy
 * 41 s of history every tick. */
static float g_hist_level[HIST_LEN];
static uint8_t g_hist_gate[HIST_LEN];
static int8_t g_hist_port1[HIST_LEN];
static int8_t g_hist_port2[HIST_LEN];
static uint32_t g_hist_seq;
static int64_t g_hist_next_ms;
static double g_hist_peak;
static struct cfg g_cfg;
static struct gate g_gate;
static struct rtp_tx g_rtp;
static double g_level_db = -120.0;
static double g_level_percent;
static struct decimator g_decim;
static struct rtp_format g_fmt; /* main loop only */
static unsigned g_ring_cap = RTP_CLOCK_HZ / 5;
static int16_t g_ring[RING_MAX];
static unsigned g_ring_r;
static unsigned g_ring_w;
static unsigned g_ring_count;
static int g_rtp_fd = -1;
static char g_rtp_ip[64];
static int g_rtp_port;
static uint64_t g_packets_sent;
static uint64_t g_send_errors;
static uint64_t g_samples_dropped;
static char g_last_rtp_header[25];
static bool g_logged_bad_format;
static double g_tone_phase;
static int64_t g_tone_start_ms;
static uint64_t g_tone_produced;
static bool g_tone_running; /* the tone clock is running for this stretch of test mode */
/* Audio manager, main loop only: the latest snapshot from manager.c. */
static struct mgr_snapshot g_mgr;
static bool g_edge; /* Edge mode: the gate raises the paging event and no RTP is sent */
static int64_t g_page_since;
static int64_t g_source_asked_ms;
static int64_t g_look_asked_ms;
/* The Edge paging event. The main loop sets what it wants; the event thread sends it. */
static volatile sig_atomic_t g_page_want;
static gint g_page_declared;
static volatile sig_atomic_t g_port_active[3] = {-1, -1, -1};
static volatile sig_atomic_t g_port_numbering;
static GMainLoop* g_event_loop;
static pthread_t g_event_thread;
static bool g_event_started;
static gint g_event_ready;
static gint g_event_stop;
static bool g_pw_inited;
static bool g_registry_hooked;
static int port_is_active(int which);
static int64_t g_last_report_ms;

static void publish_status(const char* gate);
static void ensure_stream(struct impl* impl, int64_t now);
static int64_t now_ms(void);

static int64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One error slot per subsystem, each cleared when that subsystem recovers.
 * /api/status reports all of them, and the first one set as lastError.
 * Main loop only. */
static char g_errors[ERR_SLOTS][160];

static void set_error(enum err_slot slot, const char* text) {
    snprintf(g_errors[slot], sizeof g_errors[slot], "%s", text != NULL ? text : "");
}

static void clear_error(enum err_slot slot) {
    g_errors[slot][0] = '\0';
}

static const char* current_error(void) {
    int i;

    for (i = 0; i < ERR_SLOTS; i++) {
        if (g_errors[i][0] != '\0') {
            return g_errors[i];
        }
    }
    return "";
}

/* RTP wants an unpredictable initial sequence number, timestamp and SSRC
 * (RFC 3550 5.1, 8.1). Falls back to the clock if /dev/urandom is missing. */
static void random_bytes(void* out, size_t len) {
    FILE* f = fopen("/dev/urandom", "rb");
    size_t got = 0;

    if (f != NULL) {
        got = fread(out, 1, len, f);
        fclose(f);
    }
    if (got != len) {
        uint64_t seed = (uint64_t)now_ms() * 6364136223846793005ULL + (uint64_t)getpid();
        size_t i;
        for (i = 0; i < len; i++) {
            seed                  = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            ((uint8_t*)out)[i] = (uint8_t)(seed >> 56);
        }
    }
}

static int param_int(AXParameter* handle, const char* name, int fallback, int min_v, int max_v) {
    GError* error = NULL;
    gchar* value  = NULL;
    int out       = fallback;

    if (ax_parameter_get(handle, name, &value, &error) && value != NULL) {
        char* end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end != value && end != NULL && *end == '\0' && parsed >= min_v && parsed <= max_v) {
            out = (int)parsed;
        }
    }
    g_free(value);
    g_clear_error(&error);
    return out;
}

static bool param_yes(AXParameter* handle, const char* name, bool fallback) {
    GError* error = NULL;
    gchar* value  = NULL;
    bool out      = fallback;

    if (ax_parameter_get(handle, name, &value, &error) && value != NULL) {
        out = strcmp(value, "yes") == 0;
    }
    g_free(value);
    g_clear_error(&error);
    return out;
}

static void param_str(AXParameter* handle, const char* name, char* dest, size_t dest_len, const char* fallback) {
    GError* error = NULL;
    gchar* value  = NULL;

    snprintf(dest, dest_len, "%s", fallback != NULL ? fallback : "");
    if (ax_parameter_get(handle, name, &value, &error) && value != NULL) {
        snprintf(dest, dest_len, "%s", value);
    }
    g_free(value);
    g_clear_error(&error);
}

static const char* trigger_name(int trigger_port) {
    return trigger_port == 1 ? "port1" : trigger_port == 2 ? "port2" : "level";
}

/* Reads every app parameter. Blocking D-Bus calls, so only the control thread
 * (and main() before any audio runs) may call it. */
static void read_cfg(AXParameter* params, struct cfg* out, char* err, size_t err_len) {
    struct cfg next;

    memset(&next, 0, sizeof next);
    err[0] = '\0';
    if (params == NULL) {
        next.destination_port    = 49000;
        next.threshold_db        = -45;
        next.margin_db           = 6;
        next.activation_delay_ms = 200;
        next.deactivation_delay_s = 2;
        next.sample_rate         = 48000;
        next.payload_type        = 96;
        snprintf(next.input_node, sizeof next.input_node, "%s", DEFAULT_NODE);
        *out = next;
        return;
    }
    next.enabled              = param_yes(params, "Enabled", false);
    next.force_on             = param_yes(params, "ForceOn", false);
    next.test_mode            = param_yes(params, "TestMode", false);
    {
        char trigger[16];
        param_str(params, "Trigger", trigger, sizeof trigger, "level");
        if (strcmp(trigger, "port1") == 0) {
            next.trigger_port = 1;
        } else if (strcmp(trigger, "port2") == 0) {
            next.trigger_port = 2;
        } else {
            next.trigger_port = 0;
        }
    }
    next.destination_port     = param_int(params, "DestinationPort", 49000, 1, 65535);
    next.threshold_db         = param_int(params, "ThresholdDb", -45, -90, 0);
    next.margin_db            = param_int(params, "MarginDb", 6, 0, 90);
    next.activation_delay_ms  = param_int(params, "ActivationDelayMs", 200, 0, 10000);
    next.deactivation_delay_s = param_int(params, "DeactivationDelayS", 2, 0, 60);
    next.max_on_s             = param_int(params, "MaxOnS", 0, 0, 3600);
    {
        char codec[16];
        param_str(params, "Codec", codec, sizeof codec, "pcma");
        next.codec = strcmp(codec, "l16") == 0 ? CODEC_L16 : CODEC_PCMA;
    }
    {
        char manager[16];
        enum mgr_mode mode = MGR_AUTO;
        param_str(params, "Manager", manager, sizeof manager, "auto");
        mgr_mode_parse(manager, &mode); /* anything else stays Auto */
        next.manager = mode;
    }
    next.sample_rate  = param_int(params, "SampleRate", 48000, 8000, 48000);
    next.payload_type = param_int(params, "PayloadType", 96, 96, 127);
    if (next.sample_rate != 16000 && next.sample_rate != 48000) {
        next.sample_rate = 48000;
    }
    param_str(params, "DestinationIp", next.destination_ip, sizeof next.destination_ip, "");
    param_str(params, "InputNode", next.input_node, sizeof next.input_node, DEFAULT_NODE);
    if (!valid_node_name(next.input_node)) {
        snprintf(next.input_node, sizeof next.input_node, "%s", DEFAULT_NODE);
    }
    if (next.destination_ip[0] != '\0' && !valid_ipv4(next.destination_ip)) {
        snprintf(err, err_len, "Destination IP is not a unicast IPv4 address");
        next.destination_ip[0] = '\0';
    }
    *out = next;
}

static bool set_param(AXParameter* handle, const char* name, const char* value, char* err, size_t err_len) {
    GError* error = NULL;

    if (handle == NULL) {
        snprintf(err, err_len, "Parameters are unavailable");
        return false;
    }
    if (!ax_parameter_set(handle, name, value, TRUE, &error)) {
        syslog(LOG_ERR, "Failed to set %s: %s", name, error != NULL ? error->message : "unknown");
        snprintf(err, err_len, "Could not save %s", name);
        g_clear_error(&error);
        return false;
    }
    return true;
}

static bool put_bool(AXParameter* p, const char* name, bool want, bool have, char* err, size_t err_len) {
    if (want == have) {
        return true;
    }
    return set_param(p, name, want ? "yes" : "no", err, err_len);
}

static bool put_int(AXParameter* p, const char* name, int want, int have, char* err, size_t err_len) {
    char buf[16];

    if (want == have) {
        return true;
    }
    snprintf(buf, sizeof buf, "%d", want);
    return set_param(p, name, buf, err, err_len);
}

/* Writes the fields that differ from the last values read back. Strings are
 * always written, because read_cfg() normalises invalid stored strings. */
static bool write_cfg(AXParameter* p, const struct cfg* want, const struct cfg* have, char* err, size_t err_len) {
    bool ok = true;

    ok = put_bool(p, "Enabled", want->enabled, have->enabled, err, err_len) && ok;
    ok = put_bool(p, "ForceOn", want->force_on, have->force_on, err, err_len) && ok;
    ok = put_bool(p, "TestMode", want->test_mode, have->test_mode, err, err_len) && ok;
    if (want->trigger_port != have->trigger_port) {
        ok = set_param(p, "Trigger", trigger_name(want->trigger_port), err, err_len) && ok;
    }
    /* Each write syncs over D-Bus, so skip values that haven't changed. */
    if (strcmp(want->destination_ip, have->destination_ip) != 0) {
        ok = set_param(p, "DestinationIp", want->destination_ip, err, err_len) && ok;
    }
    ok = put_int(p, "DestinationPort", want->destination_port, have->destination_port, err, err_len) && ok;
    ok = put_int(p, "ThresholdDb", want->threshold_db, have->threshold_db, err, err_len) && ok;
    ok = put_int(p, "MarginDb", want->margin_db, have->margin_db, err, err_len) && ok;
    ok = put_int(p, "ActivationDelayMs", want->activation_delay_ms, have->activation_delay_ms, err, err_len) && ok;
    ok = put_int(p, "DeactivationDelayS", want->deactivation_delay_s, have->deactivation_delay_s, err, err_len) && ok;
    ok = put_int(p, "MaxOnS", want->max_on_s, have->max_on_s, err, err_len) && ok;
    if (want->codec != have->codec) {
        ok = set_param(p, "Codec", want->codec == CODEC_L16 ? "l16" : "pcma", err, err_len) && ok;
    }
    if (want->manager != have->manager) {
        ok = set_param(p, "Manager", mgr_mode_name((enum mgr_mode)want->manager), err, err_len) && ok;
    }
    ok = put_int(p, "SampleRate", want->sample_rate, have->sample_rate, err, err_len) && ok;
    ok = put_int(p, "PayloadType", want->payload_type, have->payload_type, err, err_len) && ok;
    if (valid_node_name(want->input_node) && strcmp(want->input_node, have->input_node) != 0) {
        ok = set_param(p, "InputNode", want->input_node, err, err_len) && ok;
    }
    return ok;
}

static void clear_ring(void) {
    g_ring_r     = 0;
    g_ring_w     = 0;
    g_ring_count = 0;
}

/* While the gate is closed the ring is the pre-roll buffer, and losing its
 * oldest samples is expected. Only an overrun while sending counts as dropped. */
static void push_pcm(int16_t sample) {
    if (g_ring_count == g_ring_cap) {
        g_ring_r = (g_ring_r + 1U) % g_ring_cap;
        g_ring_count--;
        if (g_gate.streaming) {
            g_samples_dropped++;
        }
    }
    g_ring[g_ring_w] = sample;
    g_ring_w         = (g_ring_w + 1U) % g_ring_cap;
    g_ring_count++;
}

static void trim_ring(unsigned keep) {
    while (g_ring_count > keep) {
        g_ring_r = (g_ring_r + 1U) % g_ring_cap;
        g_ring_count--;
    }
}

static int port_is_active(int which) {
    if (which == 1) {
        if (g_port_numbering == 2) {
            return g_port_active[0];
        }
        return g_port_active[1];
    }
    if (which == 2) {
        if (g_port_numbering == 2) {
            return g_port_active[1];
        }
        return g_port_active[2];
    }
    return -1;
}

static void remember_port(int port, gboolean state) {
    if (port < 0 || port > 2) {
        return;
    }
    if (port == 0) {
        g_port_numbering = 2;
    } else if (g_port_numbering == 0) {
        g_port_numbering = 1;
    }
    g_port_active[port] = state ? 1 : 0;
    syslog(LOG_INFO, "Port %d is %s", port, state ? "active" : "inactive");
}

static void port_event(guint subscription, AXEvent* event, void* user_data) {
    const AXEventKeyValueSet* set = ax_event_get_key_value_set(event);
    gint port = -1;
    gboolean state = FALSE;

    (void)subscription;
    (void)user_data;
    if (ax_event_key_value_set_get_integer(set, "port", NULL, &port, NULL)) {
        ax_event_key_value_set_get_boolean(set, "state", NULL, &state, NULL);
        remember_port(port, state);
    }
    ax_event_free(event);
}

static guint subscribe_ports(AXEventHandler* handler, const char* topic2, const char* topic2_ns) {
    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    guint subscription = 0;
    GError* error = NULL;

    ax_event_key_value_set_add_key_values(set, NULL, "topic0", "tns1", "Device", AX_VALUE_TYPE_STRING, "topic1",
                                          "tnsaxis", "IO", AX_VALUE_TYPE_STRING, "topic2", topic2_ns, topic2,
                                          AX_VALUE_TYPE_STRING, "port", NULL, NULL, AX_VALUE_TYPE_INT, "state", NULL,
                                          NULL, AX_VALUE_TYPE_BOOL, NULL);
    if (!ax_event_handler_subscribe(handler, set, &subscription, (AXSubscriptionCallback)port_event, NULL, &error)) {
        syslog(LOG_ERR, "Port subscription failed: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
    }
    ax_event_key_value_set_free(set);
    return subscription;
}

static void page_declared(guint declaration, gpointer user_data) {
    (void)declaration;
    (void)user_data;
    g_atomic_int_set(&g_page_declared, 1);
}

/* A property-state event, false at start: "Line Input RTP: Edge paging".
 * The rule manager.c adds uses it as its condition. */
static guint declare_page_event(AXEventHandler* handler) {
    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    guint declaration       = 0;
    gboolean off            = FALSE;
    GError* error           = NULL;

    ax_event_key_value_set_add_key_value(set, "topic0", "tnsaxis", "CameraApplicationPlatform", AX_VALUE_TYPE_STRING,
                                         NULL);
    ax_event_key_value_set_add_key_value(set, "topic1", "tnsaxis", EDGE_EVENT_TOPIC1, AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_key_value(set, "topic2", "tnsaxis", EDGE_EVENT_TOPIC2, AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_nice_names(set, "topic1", "tnsaxis", NULL, "Line Input RTP", NULL);
    ax_event_key_value_set_add_nice_names(set, "topic2", "tnsaxis", NULL, "Line Input RTP: Edge paging", NULL);
    ax_event_key_value_set_add_key_value(set, EDGE_EVENT_KEY, NULL, &off, AX_VALUE_TYPE_BOOL, NULL);
    ax_event_key_value_set_mark_as_data(set, EDGE_EVENT_KEY, NULL, NULL);
    ax_event_key_value_set_add_nice_names(set, EDGE_EVENT_KEY, NULL, "Paging", NULL, NULL);
    if (!ax_event_handler_declare(handler, set, FALSE, &declaration, page_declared, NULL, &error)) {
        syslog(LOG_ERR, "Could not declare the Edge paging event: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
        declaration = 0;
    }
    ax_event_key_value_set_free(set);
    return declaration;
}

static bool send_page_event(AXEventHandler* handler, guint declaration, gboolean on) {
    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    AXEvent* event;
    GError* error = NULL;
    bool ok;

    ax_event_key_value_set_add_key_value(set, EDGE_EVENT_KEY, NULL, &on, AX_VALUE_TYPE_BOOL, NULL);
    event = ax_event_new2(set, NULL);
    ax_event_key_value_set_free(set);
    ok = ax_event_handler_send_event(handler, declaration, event, &error);
    if (!ok) {
        static unsigned failures;
        if (failures++ % 100 == 0) {
            syslog(LOG_ERR, "Could not send the Edge paging event: %s", error != NULL ? error->message : "unknown");
        }
        g_clear_error(&error);
    }
    ax_event_free(event);
    return ok;
}

/* Main loop: asks the event thread to raise or drop the paging event. */
static void want_page(bool on) {
    int want = on ? 1 : 0;

    if (g_page_want == want) {
        return;
    }
    g_page_want = want;
    if (g_atomic_int_get(&g_event_ready) == 1 && g_event_loop != NULL) {
        g_main_context_wakeup(g_main_loop_get_context(g_event_loop));
    }
}

static void* event_thread(void* unused) {
    AXEventHandler* handler = NULL;
    guint subscription      = 0;
    guint page_decl         = 0;
    int page_sent           = 0; /* the event is declared false */
    GMainLoop* loop;

    (void)unused;
    loop = g_main_loop_new(NULL, FALSE);
    if (loop == NULL) {
        g_atomic_int_set(&g_event_ready, -1);
        return NULL;
    }
    g_event_loop = loop;
    handler      = ax_event_handler_new();
    if (handler != NULL) {
        subscription = subscribe_ports(handler, "Port", "tnsaxis");
        page_decl    = declare_page_event(handler);
    }
    g_atomic_int_set(&g_event_ready, 1);
    while (g_atomic_int_get(&g_event_stop) == 0) {
        int want = g_page_want;

        if (page_decl != 0 && want != page_sent && g_atomic_int_get(&g_page_declared) == 1) {
            if (send_page_event(handler, page_decl, want != 0)) {
                page_sent = want;
            } else {
                /* A page must never be left on: keep trying, ten times a second. */
                g_main_context_iteration(g_main_loop_get_context(loop), FALSE);
                g_usleep(100000);
            }
            continue; /* look again before sleeping: it may have changed meanwhile */
        }
        g_main_context_iteration(g_main_loop_get_context(loop), TRUE);
    }
    /* Don't leave a page running when the app stops. The declaration itself
     * goes away with the process (and the event with it, should the app ever
     * die without getting here); undeclaring by hand only adds a 2 s wait. */
    if (page_decl != 0 && page_sent != 0) {
        int i;
        send_page_event(handler, page_decl, FALSE);
        for (i = 0; i < 20 && g_main_context_iteration(g_main_loop_get_context(loop), FALSE); i++) {
        }
    }
    if (handler != NULL && subscription != 0) {
        ax_event_handler_unsubscribe(handler, subscription, NULL);
    }
    if (handler != NULL) {
        ax_event_handler_free(handler);
    }
    g_main_loop_unref(loop);
    g_event_loop = NULL;
    return NULL;
}

/* The test tone is an RTP feature. In Edge mode the page is the line input
 * itself, so a saved Test tone setting behaves as Auto. */
static bool tone_mode(void) {
    return g_cfg.test_mode && !g_edge;
}

/* Side effects of the gate closing. The gate state itself lives in rtpcore.c. */
static void streaming_stopped(const char* reason) {
    if (g_edge) {
        syslog(LOG_INFO, "Edge paging stopped (%s), level %.1f dBFS", reason, g_level_db);
        want_page(false);
        mgr_check_source(600);
        clear_ring();
        return;
    }
    syslog(LOG_INFO, "RTP stopped (%s), level %.1f dBFS, packets %llu", reason, g_level_db,
           (unsigned long long)g_packets_sent);
    rtp_tx_stop(&g_rtp, now_ms());
    clear_ring();
}

/* Keeps the activation delay's worth of audio as pre-roll, so the receiver
 * hears the sound that opened the gate. Capped by the ring at 200 ms. */
static void streaming_started(const char* reason) {
    int64_t now   = now_ms();
    unsigned keep = (unsigned)((uint64_t)g_cfg.activation_delay_ms * g_fmt.rate / 1000U);

    if (g_edge) {
        /* Edge reads the line input itself from the moment the rule fires,
         * so there is no pre-roll to keep. */
        clear_ring();
        g_page_since      = now;
        g_source_asked_ms = now;
        want_page(true);
        mgr_check_source(400);
        syslog(LOG_INFO, "Edge paging started (%s), level %.1f dBFS", reason, g_level_db);
        return;
    }
    if (keep < g_fmt.packet_samples) {
        keep = g_fmt.packet_samples;
    }
    if (keep > g_ring_cap) {
        keep = g_ring_cap;
    }
    if (tone_mode()) {
        keep = 0; /* the tone starts clean; on_timeout() starts its clock */
    }
    trim_ring(keep);
    rtp_tx_talkspurt(&g_rtp, now, g_ring_count);
    syslog(LOG_INFO, "RTP started (%s), level %.1f dBFS, dest %s:%d", reason, g_level_db,
           g_cfg.destination_ip, g_cfg.destination_port);
}

static void stop_streaming(const char* reason) {
    if (gate_stop(&g_gate)) {
        streaming_stopped(reason);
    }
}

/* Points the decimator from the capture rate to the stream's rate. On failure
 * it stays unconfigured, nothing reaches the ring, and the capture error says why. */
static bool setup_decimator(unsigned in_rate) {
    char text[160];

    if (decimator_init(&g_decim, in_rate, g_fmt.rate)) {
        return true;
    }
    snprintf(text, sizeof text, "Capture rate %u Hz can't be reduced to %u Hz", in_rate, g_fmt.rate);
    set_error(ERR_CAPTURE, text);
    return false;
}

/* Puts the configured codec on the wire. A change of codec, rate or payload
 * type ends the talkspurt and starts a new RTP stream (new SSRC), since the
 * receiver can't follow a clock change inside one. Also points the
 * decimator at the new output rate. */
static void configure_audio(void) {
    struct rtp_format want;

    if (!rtp_format_make(&want, (enum rtp_codec)g_cfg.codec, (unsigned)g_cfg.sample_rate, g_cfg.payload_type)) {
        set_error(ERR_CONFIG, "Unsupported codec settings, sending G.711");
    }
    if (want.codec != g_fmt.codec || want.rate != g_fmt.rate || want.payload_type != g_fmt.payload_type) {
        uint32_t ssrc;
        bool first = g_fmt.rate == 0;

        stop_streaming("codec changed");
        random_bytes(&ssrc, sizeof ssrc);
        g_fmt      = want;
        g_ring_cap = g_fmt.rate / 5U;
        clear_ring();
        rtp_tx_set_format(&g_rtp, g_fmt.payload_type, g_fmt.rate, first ? g_rtp.ssrc : ssrc);
        g_tone_phase = 0.0;
        syslog(LOG_INFO, "Codec %s, %u Hz, payload type %u, %u ms packets", g_fmt.codec == CODEC_L16 ? "L16" : "PCMA",
               g_fmt.rate, g_fmt.payload_type, g_fmt.packet_ms);
    }
    if (g_impl.format_ready) {
        unsigned in_rate = g_impl.info.info.raw.rate;
        if (g_decim.factor == 0 || in_rate != (unsigned)g_decim.factor * g_fmt.rate) {
            setup_decimator(in_rate);
        }
    }
}

static void update_gate(void) {
    struct gate_input in;
    struct gate_result r;

    memset(&in, 0, sizeof in);
    /* Auto holds the gate closed until manager.c has looked at the device. */
    in.enabled              = g_cfg.enabled && g_mgr.active != ACTIVE_UNKNOWN;
    in.have_destination     = g_edge || g_cfg.destination_ip[0] != '\0';
    in.test_mode            = tone_mode();
    in.force_on             = g_cfg.force_on;
    in.format_ready         = g_impl.format_ready;
    in.trigger_port         = g_cfg.trigger_port;
    in.port_active          = g_cfg.trigger_port > 0 ? port_is_active(g_cfg.trigger_port) : -1;
    in.level_percent        = g_level_percent;
    in.threshold_db         = g_cfg.threshold_db;
    in.margin_db            = g_cfg.margin_db;
    in.activation_delay_ms  = g_cfg.activation_delay_ms;
    in.deactivation_delay_s = g_cfg.deactivation_delay_s;
    in.max_on_s             = g_cfg.max_on_s;

    r = gate_step(&g_gate, &in, now_ms());
    if (r.action == GATE_START) {
        streaming_started(r.reason);
    } else if (r.action == GATE_STOP) {
        streaming_stopped(r.reason);
    }
    if (r.rearmed) {
        syslog(LOG_INFO, "Gate re-armed after max-on quiet period");
    }
}

/* Opens and connects the UDP socket for the current destination. After a
 * failure it waits a second before trying again, instead of on every packet. */
static int ensure_socket(void) {
    static int64_t retry_at;
    struct sockaddr_in addr;
    int64_t now = now_ms();

    if (g_rtp_fd >= 0 && strcmp(g_rtp_ip, g_cfg.destination_ip) == 0 && g_rtp_port == g_cfg.destination_port) {
        return g_rtp_fd;
    }
    if (g_rtp_fd >= 0) {
        close(g_rtp_fd);
        g_rtp_fd = -1;
        retry_at = 0; /* a new destination gets a try right away */
    }
    if (now < retry_at) {
        return -1;
    }
    retry_at = now + 1000;
    g_rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rtp_fd < 0) {
        set_error(ERR_NETWORK, "Could not open UDP socket");
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_cfg.destination_port);
    if (inet_pton(AF_INET, g_cfg.destination_ip, &addr.sin_addr) != 1) {
        set_error(ERR_NETWORK, "Destination IP is not a unicast IPv4 address");
        close(g_rtp_fd);
        g_rtp_fd = -1;
        return -1;
    }
    if (connect(g_rtp_fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
        set_error(ERR_NETWORK, "Could not connect UDP socket");
        close(g_rtp_fd);
        g_rtp_fd = -1;
        return -1;
    }
    snprintf(g_rtp_ip, sizeof g_rtp_ip, "%s", g_cfg.destination_ip);
    g_rtp_port = g_cfg.destination_port;
    return g_rtp_fd;
}

static void send_packet(void) {
    uint8_t packet[RTP_HEADER_LEN + RTP_MAX_PAYLOAD];
    int16_t pcm[RTP_MAX_PAYLOAD / 2];
    const unsigned n = g_fmt.packet_samples;
    size_t len;
    unsigned i;
    int fd;
    ssize_t sent;

    if (g_ring_count < n || n > RTP_MAX_PAYLOAD / 2) {
        return;
    }
    /* Without a socket the packet is lost like a failed send: its samples are
     * dropped and sequence and timestamp still advance, so the receiver sees
     * a gap rather than audio spliced across it. */
    fd = ensure_socket();
    rtp_tx_header(&g_rtp, packet);
    for (i = 0; i < n; i++) {
        pcm[i]   = g_ring[g_ring_r];
        g_ring_r = (g_ring_r + 1U) % g_ring_cap;
        g_ring_count--;
    }
    len = RTP_HEADER_LEN + rtp_encode(&g_fmt, pcm, n, packet + RTP_HEADER_LEN);
    for (i = 0; i < RTP_HEADER_LEN; i++) {
        snprintf(g_last_rtp_header + (i * 2), 3, "%02x", packet[i]);
    }
    sent = fd >= 0 ? send(fd, packet, len, MSG_DONTWAIT) : -1;
    /* The samples are gone either way, so the packet counts for sequence and
     * timestamp. A failed send is packet loss to the receiver. */
    rtp_tx_advance(&g_rtp, n);
    if (sent != (ssize_t)len) {
        int err = errno;
        g_send_errors++;
        /* ensure_socket() already reported a missing socket. */
        if (fd >= 0 && (g_send_errors == 1 || (g_send_errors % 100) == 0)) {
            char text[160];
            syslog(LOG_WARNING, "RTP send failed: %s", strerror(err));
            snprintf(text, sizeof text, "RTP send failed: %s", strerror(err));
            set_error(ERR_NETWORK, text);
        }
        return;
    }
    g_packets_sent++;
    clear_error(ERR_NETWORK);
}

/* 1 kHz sine at about -12 dBFS, generated at the stream's rate. */
static void push_test_tone(unsigned samples) {
    unsigned i;

    for (i = 0; i < samples; i++) {
        double sample = sin(g_tone_phase) * (0.25 * 32767.0);
        push_pcm((int16_t)lrint(sample));
        g_tone_phase += 2.0 * 3.141592653589793 * 1000.0 / (double)g_fmt.rate;
        if (g_tone_phase > 2.0 * 3.141592653589793) {
            g_tone_phase -= 2.0 * 3.141592653589793;
        }
    }
    g_level_db = -12.0;
    g_level_percent = 25.0;
}

/* Starts the tone's clock. Called whenever test mode begins with the gate
 * open, including a switch to Test tone in the middle of a talkspurt: any line
 * audio still buffered is dropped and the tone starts a new talkspurt, instead
 * of the clock counting from a stale start and sending a burst. */
static void start_test_tone(int64_t now) {
    clear_ring();
    g_tone_start_ms = now;
    g_tone_produced = 0;
    g_tone_phase    = 0.0;
    g_tone_running  = true;
    g_rtp.marker    = true;
}

/* Generates exactly one second of tone per second of wall-clock time, however
 * late or bunched the timer ticks are, so a receiver never underruns. */
static void run_test_tone(int64_t now) {
    uint64_t due = clock_samples_due(g_tone_start_ms, now, g_fmt.rate, g_tone_produced);

    if (due > g_ring_cap) {
        /* A stall longer than the ring: skip ahead rather than send a burst. */
        g_tone_produced += due - g_ring_cap;
        due = g_ring_cap;
    }
    push_test_tone((unsigned)due);
    g_tone_produced += due;
}

/* Sends up to 4 packets per 10 ms tick. Steady state needs half of one (G.711,
 * 20 ms packets) or one (L16 48 kHz, 10 ms packets), and the headroom drains
 * the pre-roll at 4-8x real time. */
static void drain_audio(void) {
    unsigned sent = 0;

    if (g_edge) {
        clear_ring(); /* nothing is sent: Edge pages from the input itself */
        return;
    }
    if (!g_gate.streaming) {
        if (g_gate.hot_since == 0) {
            clear_ring();
        }
        return;
    }
    while (g_ring_count >= g_fmt.packet_samples && sent < 4U) {
        send_packet();
        sent++;
    }
}

/* Whether Edge itself reports the page this gate asked for: 1 yes, 0 no,
 * -1 not checked since the gate opened (or the gate is closed). */
static int edge_confirmed(void) {
    if (!g_edge || !g_gate.streaming || g_mgr.source_checked_ms < g_page_since + 300) {
        return -1;
    }
    return g_mgr.source_active;
}

static void publish_status(const char* gate) {
    struct status_snap snap;

    memset(&snap, 0, sizeof snap);
    snap.level_db             = g_level_db;
    snap.streaming            = g_gate.streaming;
    snap.enabled              = g_cfg.enabled;
    snap.force_on             = g_cfg.force_on;
    snap.test_mode            = g_cfg.test_mode;
    snap.trigger_port         = g_cfg.trigger_port;
    snap.port1_active         = port_is_active(1);
    snap.port2_active         = port_is_active(2);
    snap.node_connected       = g_impl.format_ready;
    snap.threshold_db         = g_cfg.threshold_db;
    snap.margin_db            = g_cfg.margin_db;
    snap.activation_delay_ms  = g_cfg.activation_delay_ms;
    snap.deactivation_delay_s = g_cfg.deactivation_delay_s;
    snap.max_on_s             = g_cfg.max_on_s;
    snap.codec                = g_cfg.codec;
    snap.sample_rate          = g_cfg.sample_rate;
    snap.payload_type         = g_cfg.payload_type;
    snap.fmt                  = g_fmt;
    snap.destination_port     = g_cfg.destination_port;
    snap.level_percent        = g_level_percent;
    snap.capture_rate         = g_impl.format_ready ? (int)g_impl.info.info.raw.rate : 0;
    snap.capture_channels     = g_impl.format_ready ? (int)g_impl.info.info.raw.channels : 0;
    snap.packets_sent         = g_packets_sent;
    snap.send_errors          = g_send_errors;
    snap.samples_dropped      = g_samples_dropped;
    snap.buffered_samples     = g_ring_count;
    snap.manager              = g_cfg.manager;
    snap.mgr                  = g_mgr;
    snap.edge_paging          = g_edge && g_gate.streaming;
    snap.edge_confirmed       = edge_confirmed();
    snprintf(snap.gate, sizeof snap.gate, "%s", gate);
    snprintf(snap.destination_ip, sizeof snap.destination_ip, "%s", g_cfg.destination_ip);
    if (g_cfg.destination_ip[0] != '\0') {
        snprintf(snap.stream_url, sizeof snap.stream_url, "rtp://%s:%d", g_cfg.destination_ip, g_cfg.destination_port);
    }
    snprintf(snap.input_node, sizeof snap.input_node, "%s", g_cfg.input_node);
    snprintf(snap.connected_node, sizeof snap.connected_node, "%s", g_impl.connected_name);
    snprintf(snap.last_error, sizeof snap.last_error, "%.159s", current_error());
    memcpy(snap.errors, g_errors, sizeof snap.errors);
    snprintf(snap.last_rtp_header, sizeof snap.last_rtp_header, "%s", g_last_rtp_header);
    snprintf(snap.config_error, sizeof snap.config_error, "%s", g_cfg_save_error);
    snap.config_rev = g_cfg_rev_applied;

    pthread_mutex_lock(&g_mu);
    g_status = snap;
    pthread_mutex_unlock(&g_mu);
}

/* Codes shared with the settings page: 0 idle, 1 pending, 2 active,
 * 3 holding, 4 waiting after max-on, 5 test tone. */
static uint8_t gate_code(const char* gate) {
    if (strcmp(gate, "pending") == 0) {
        return 1;
    }
    if (strcmp(gate, "active") == 0) {
        return 2;
    }
    if (strcmp(gate, "holding") == 0) {
        return 3;
    }
    if (strcmp(gate, "waiting") == 0) {
        return 4;
    }
    if (strcmp(gate, "test") == 0) {
        return 5;
    }
    return 0;
}

/* One entry per HIST_INTERVAL_MS: the peak level seen in that window and the
 * gate state at its end. The page merges entries by sequence number, so its chart shows
 * real gate history at a steady rate whatever its polling interval. */
static void record_history(int64_t now, const char* gate) {
    unsigned idx;
    double peak;

    if (g_level_percent > g_hist_peak) {
        g_hist_peak = g_level_percent;
    }
    if (g_hist_next_ms == 0) {
        g_hist_next_ms = now + HIST_INTERVAL_MS;
    }
    if (now < g_hist_next_ms) {
        return;
    }
    peak = g_hist_peak < 0.0 ? 0.0 : g_hist_peak > 100.0 ? 100.0 : g_hist_peak;
    pthread_mutex_lock(&g_mu);
    idx  = (unsigned)(g_hist_seq % HIST_LEN);
    g_hist_level[idx] = (float)peak; /* fractional: quiet input stays visible on a dB scale */
    g_hist_gate[idx]  = gate_code(gate);
    g_hist_port1[idx] = (int8_t)port_is_active(1);
    g_hist_port2[idx] = (int8_t)port_is_active(2);
    g_hist_seq++;
    pthread_mutex_unlock(&g_mu);
    g_hist_peak = 0.0;
    g_hist_next_ms += HIST_INTERVAL_MS;
    if (now >= g_hist_next_ms) {
        g_hist_next_ms = now + HIST_INTERVAL_MS;
    }
}

static void take_control_updates(void) {
    struct ctl_update u;

    pthread_mutex_lock(&g_mu);
    if (!g_ctl.have_cfg) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    u              = g_ctl;
    g_ctl.have_cfg = false;
    pthread_mutex_unlock(&g_mu);

    if (u.have_cfg) {
        /* A network error belongs to the destination it happened with. Once
         * the destination changes, or sending is switched off, it's stale. */
        if (strcmp(u.cfg.destination_ip, g_cfg.destination_ip) != 0 ||
            u.cfg.destination_port != g_cfg.destination_port || (g_cfg.enabled && !u.cfg.enabled)) {
            clear_error(ERR_NETWORK);
        }
        /* The revision and the values it confirms reach /api/status together. */
        g_cfg             = u.cfg;
        g_cfg_rev_applied = u.rev;
        mgr_set_mode((enum mgr_mode)g_cfg.manager);
        snprintf(g_cfg_save_error, sizeof g_cfg_save_error, "%s", u.save_error);
        if (u.cfg_error[0] != '\0') {
            set_error(ERR_CONFIG, u.cfg_error);
        } else {
            clear_error(ERR_CONFIG);
        }
        /* After the config error slot is settled, so a codec error it sets stays. */
        configure_audio();
    }
}

/* Picks up what manager.c found. A change of mode ends whatever the gate was
 * doing in the old one: RTP stops, or the Edge page is dropped. */
static void take_manager_updates(int64_t now) {
    struct mgr_snapshot m;
    bool edge;

    mgr_get(&m);
    edge = m.active == ACTIVE_EDGE;
    if (edge != g_edge) {
        stop_streaming("audio manager changed");
        want_page(false);
        clear_error(ERR_NETWORK);
        g_edge = edge;
    }
    g_mgr = m;
    /* While a page is open and Edge hasn't confirmed it, ask again now and then. */
    if (g_edge && g_gate.streaming && edge_confirmed() != 1 && now - g_source_asked_ms >= 2000) {
        g_source_asked_ms = now;
        mgr_check_source(0);
    }
    /* Edge says it isn't paging: have the rule and the recipient looked at
     * now, so the page can say why, rather than at the next pass. */
    if (edge_confirmed() == 0 && now - g_look_asked_ms >= 5000) {
        g_look_asked_ms = now;
        mgr_look_now();
    }
}

static void destroy_stream(struct impl* impl);

/* Stream errors are terminal in PipeWire, so tear the stream down and let
 * ensure_stream() build a new one after a backoff. Also catches a stream that
 * stays connected but stops delivering buffers. */
static void check_capture(struct impl* impl, int64_t now) {
    if (impl->stream != NULL && !g_stream_failed && now - g_last_process_ms > AUDIO_WATCHDOG_MS) {
        syslog(LOG_WARNING, "No audio from %s for %d ms, reconnecting", impl->connected_name, AUDIO_WATCHDOG_MS);
        set_error(ERR_CAPTURE, "No audio from the input, reconnecting");
        g_stream_failed = true;
    }
    if (g_stream_failed) {
        g_stream_failed = false;
        destroy_stream(impl);
        g_retry_at = now + g_retry_ms;
        syslog(LOG_INFO, "Retrying capture in %d ms", g_retry_ms);
        g_retry_ms = g_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : g_retry_ms * 2;
        return;
    }
    if (impl->stream != NULL && g_retry_ms != RETRY_MIN_MS && now - g_connected_ms > 10000 &&
        now - g_last_process_ms < 1000) {
        g_retry_ms = RETRY_MIN_MS;
    }
}

static void on_timeout(void* data, uint64_t expirations) {
    struct impl* impl = data;
    int64_t now;
    const char* gate;

    (void)expirations;
    now = now_ms();
    take_control_updates();
    take_manager_updates(now);
    check_capture(impl, now);
    ensure_stream(impl, now);
    update_gate();
    if (tone_mode() && g_gate.streaming) {
        if (!g_tone_running) {
            start_test_tone(now);
        }
        run_test_tone(now);
    } else {
        g_tone_running = false;
    }
    drain_audio();
    gate = gate_state(&g_gate, tone_mode());
    record_history(now, gate);
    publish_status(gate);

    if (g_gate.streaming && !g_edge && now - g_last_report_ms >= 10000) {
        g_last_report_ms = now;
        syslog(LOG_INFO, "Streaming level %.1f dBFS packets %llu errors %llu", g_level_db,
               (unsigned long long)g_packets_sent, (unsigned long long)g_send_errors);
    }
}

static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param) {
    struct impl* impl = data;
    int res;

    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }
    res = spa_format_parse(param, &impl->info.media_type, &impl->info.media_subtype);
    if (res < 0) {
        set_error(ERR_CAPTURE, "Could not parse capture format");
        return;
    }
    if (impl->info.media_type != SPA_MEDIA_TYPE_audio || impl->info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        set_error(ERR_CAPTURE, "Capture format is not raw audio");
        return;
    }
    spa_format_audio_raw_parse(param, &impl->info.info.raw);
    impl->format_ready = true;
    setup_decimator(impl->info.info.raw.rate);
    syslog(LOG_INFO, "Capturing %s, %u ch, %u Hz, format %d, decimation %d (%d-tap filter)", impl->connected_name,
           impl->info.info.raw.channels, impl->info.info.raw.rate, impl->info.info.raw.format, g_decim.factor,
           g_decim.taps);
}

static void on_state_changed(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    struct impl* impl = data;

    syslog(LOG_INFO, "PipeWire %s -> %s", pw_stream_state_as_string(old), pw_stream_state_as_string(state));
    if (state == PW_STREAM_STATE_ERROR) {
        syslog(LOG_ERR, "PipeWire stream error: %s", error != NULL ? error : "");
        set_error(ERR_CAPTURE, "PipeWire stream error, reconnecting");
        impl->format_ready = false;
        /* Destroying a stream inside its own callback is not safe. */
        g_stream_failed = true;
    }
    /* Capture errors clear in on_process, once audio actually arrives. */
}

/* Mixes to mono, measures peak and RMS, and feeds the 8 kHz decimator. Planar
 * audio uses stride 1 with a separate right pointer; interleaved audio uses
 * stride = channels with right = left + 1. */
static void consume_frames(const float* left, const float* right, uint32_t frames, uint32_t stride) {
    double sumsq = 0.0;
    float peak   = 0.f;
    uint32_t i;

    if (frames == 0 || left == NULL) {
        return;
    }
    for (i = 0; i < frames; i++) {
        float sample = left[i * stride];
        float amp    = fabsf(sample);
        float out;

        if (right != NULL) {
            float r = right[i * stride];
            amp     = fmaxf(amp, fabsf(r));
            sample  = 0.5f * (sample + r);
        }
        if (amp > peak) {
            peak = amp;
        }
        sumsq += (double)sample * (double)sample;
        if (decimator_push(&g_decim, sample, &out)) {
            if (out > 1.f) {
                out = 1.f;
            } else if (out < -1.f) {
                out = -1.f;
            }
            push_pcm((int16_t)lrintf(out * 32767.f));
        }
    }
    {
        double rms = sqrt(sumsq / (double)frames);
        g_level_db = rms < 1e-9 ? -120.0 : 20.0 * log10(rms);
        if (peak > 1.f) {
            peak = 1.f;
        }
        g_level_percent = (double)peak * 100.0;
    }
}

static const float* audio_samples(const struct spa_data* data, uint32_t* frames) {
    uint32_t offset;
    uint32_t size;

    if (frames == NULL) {
        return NULL;
    }
    *frames = 0;
    if (data == NULL || data->data == NULL || data->chunk == NULL) {
        return NULL;
    }
    offset = data->chunk->offset;
    size   = data->chunk->size;
    if (data->maxsize > 0 && (offset > data->maxsize || size > data->maxsize - offset)) {
        return NULL;
    }
    *frames = size / (uint32_t)sizeof(float);
    return (const float*)((const uint8_t*)data->data + offset);
}

static void on_process(void* data) {
    struct impl* impl = data;
    struct pw_buffer* buffer;
    struct spa_buffer* buf;
    uint32_t count = 0;
    const float* left;
    bool consumed = false;

    buffer = pw_stream_dequeue_buffer(impl->stream);
    if (buffer == NULL) {
        return;
    }
    g_last_process_ms = now_ms();
    if (tone_mode()) {
        pw_stream_queue_buffer(impl->stream, buffer);
        return;
    }
    buf = buffer->buffer;
    if (buf->n_datas < 1) {
        pw_stream_queue_buffer(impl->stream, buffer);
        return;
    }
    left = audio_samples(&buf->datas[0], &count);
    if (left == NULL || count == 0) {
        pw_stream_queue_buffer(impl->stream, buffer);
        return;
    }
    if (impl->info.info.raw.format == SPA_AUDIO_FORMAT_F32P) {
        const float* right = NULL;
        if (impl->info.info.raw.channels > 1 && buf->n_datas > 1) {
            uint32_t right_count = 0;
            right                = audio_samples(&buf->datas[1], &right_count);
            if (right != NULL && right_count < count) {
                count = right_count;
            }
        }
        consume_frames(left, right, count, 1);
        consumed = true;
    } else if (impl->info.info.raw.format == SPA_AUDIO_FORMAT_F32) {
        uint32_t channels = impl->info.info.raw.channels > 0 ? impl->info.info.raw.channels : 1;
        consume_frames(left, channels > 1 ? left + 1 : NULL, count / channels, channels);
        consumed = true;
    } else if (!g_logged_bad_format) {
        g_logged_bad_format = true;
        syslog(LOG_ERR, "Unsupported PipeWire format %d", impl->info.info.raw.format);
        set_error(ERR_CAPTURE, "Unsupported capture format");
    }
    if (consumed && g_decim.factor > 0 && g_errors[ERR_CAPTURE][0] != '\0') {
        clear_error(ERR_CAPTURE); /* audio is flowing and usable again */
    }
    pw_stream_queue_buffer(impl->stream, buffer);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
    .state_changed = on_state_changed,
};

static void destroy_stream(struct impl* impl) {
    if (impl->stream == NULL) {
        return;
    }
    spa_hook_remove(&impl->stream_listener);
    pw_stream_destroy(impl->stream);
    impl->stream         = NULL;
    impl->format_ready   = false;
    impl->connected_id   = 0;
    impl->connected_name[0] = '\0';
    memset(&g_decim, 0, sizeof g_decim);
    stop_streaming("capture stopped");
    clear_ring();
}

static void connect_node(struct impl* impl, uint32_t id, const char* name) {
    struct pw_properties* props;
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
    const struct spa_pod* params[1];
    int res;

    if (impl->stream != NULL && impl->connected_id == id) {
        return;
    }
    destroy_stream(impl);
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_TARGET_OBJECT,
                              name, NULL);
    if (props == NULL) {
        set_error(ERR_CAPTURE, "Could not create PipeWire properties");
        return;
    }
    impl->stream = pw_stream_new(impl->core, "lineinrtp", props);
    if (impl->stream == NULL) {
        set_error(ERR_CAPTURE, "Could not create PipeWire stream");
        return;
    }
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &stream_events, impl);
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
                                           &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32P));
    res = pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    if (res < 0) {
        syslog(LOG_ERR, "PipeWire connect failed for %s: %s", name, strerror(-res));
        set_error(ERR_CAPTURE, "PipeWire connect failed");
        destroy_stream(impl);
        return;
    }
    impl->connected_id = id;
    snprintf(impl->connected_name, sizeof impl->connected_name, "%s", name);
    g_connected_ms    = now_ms();
    g_last_process_ms = g_connected_ms;
    syslog(LOG_INFO, "Connecting to input node %s id %u", name, id);
}

static void remember_node(struct impl* impl, uint32_t id, const char* name) {
    unsigned i;
    unsigned free_slot = NODE_MAX;

    for (i = 0; i < NODE_MAX; i++) {
        if (impl->nodes[i].used && impl->nodes[i].id == id) {
            snprintf(impl->nodes[i].name, sizeof impl->nodes[i].name, "%s", name);
            return;
        }
        if (!impl->nodes[i].used && free_slot == NODE_MAX) {
            free_slot = i;
        }
    }
    if (free_slot == NODE_MAX) {
        return;
    }
    impl->nodes[free_slot].used = true;
    impl->nodes[free_slot].id   = id;
    snprintf(impl->nodes[free_slot].name, sizeof impl->nodes[free_slot].name, "%s", name);
}

static void ensure_stream(struct impl* impl, int64_t now) {
    unsigned i;

    if (impl->stream != NULL && strcmp(impl->connected_name, g_cfg.input_node) == 0) {
        return;
    }
    if (impl->stream == NULL && now < g_retry_at) {
        return;
    }
    for (i = 0; i < NODE_MAX; i++) {
        if (impl->nodes[i].used && strcmp(impl->nodes[i].name, g_cfg.input_node) == 0) {
            connect_node(impl, impl->nodes[i].id, impl->nodes[i].name);
            return;
        }
    }
    if (impl->stream != NULL) {
        destroy_stream(impl);
    }
}

static void registry_event_global(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                                  const struct spa_dict* props) {
    struct impl* impl = data;
    const char* media_class;
    const char* name;

    (void)permissions;
    (void)version;
    if (!spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        return;
    }
    media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    name        = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name == NULL) {
        return;
    }
    syslog(LOG_INFO, "PipeWire node %s class %s id %u", name, media_class != NULL ? media_class : "-", id);
    if (media_class != NULL && strcmp(media_class, "Audio/Source") != 0) {
        return;
    }
    remember_node(impl, id, name);
    if (strcmp(name, g_cfg.input_node) == 0) {
        connect_node(impl, id, name);
    }
}

static void registry_event_global_remove(void* data, uint32_t id) {
    struct impl* impl = data;
    unsigned i;

    for (i = 0; i < NODE_MAX; i++) {
        if (impl->nodes[i].used && impl->nodes[i].id == id) {
            impl->nodes[i].used = false;
        }
    }
    if (impl->connected_id == id) {
        syslog(LOG_WARNING, "Input node %s disappeared", impl->connected_name);
        destroy_stream(impl);
        set_error(ERR_CAPTURE, "Input node disappeared");
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static void on_signal(void* data, int signal_num) {
    struct impl* impl = data;

    syslog(LOG_INFO, "Caught signal %d", signal_num);
    pw_main_loop_quit(impl->loop);
}

/* If the PipeWire daemon goes away the core proxy is dead for good. Exit with
 * an error so runMode "respawn" starts a fresh process that reconnects. */
static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
    struct impl* impl = data;

    (void)seq;
    syslog(LOG_ERR, "PipeWire error on %u: %s (%d)", id, message != NULL ? message : "", res);
    if (id == PW_ID_CORE && res == -EPIPE) {
        syslog(LOG_ERR, "Lost the PipeWire connection, exiting so the app restarts");
        g_exit_code = EXIT_FAILURE;
        pw_main_loop_quit(impl->loop);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

/* `since` is the last history sequence number the page already has, or -1.
 * Only newer entries are sent, so a steady poll carries a few entries instead
 * of the whole buffer. A proxy that drops the query string just gets the
 * full buffer, which the page merges the same way. */
static char* status_json(long long since) {
    /* Static: the HTTP server handles one request at a time. */
    static float level[HIST_LEN];
    static uint8_t gates[HIST_LEN];
    static int8_t port1[HIST_LEN];
    static int8_t port2[HIST_LEN];
    static const char* const err_area[ERR_SLOTS] = {"config", "capture", "network"};
    struct status_snap snap;
    uint32_t hist_seq;
    unsigned hist_count;
    unsigned skip = 0;
    json_t* errors;
    json_t* root;
    json_t* hist;
    json_t* hist_level;
    json_t* hist_gate;
    json_t* hist_port1;
    json_t* hist_port2;
    unsigned i;
    char* text;

    pthread_mutex_lock(&g_mu);
    snap       = g_status;
    hist_seq   = g_hist_seq;
    hist_count = hist_seq < HIST_LEN ? hist_seq : HIST_LEN;
    if (since >= 0) {
        long long first = (long long)hist_seq - (long long)hist_count;
        long long want  = since + 1 - first;
        skip            = want <= 0 ? 0 : want >= (long long)hist_count ? hist_count : (unsigned)want;
    }
    for (i = skip; i < hist_count; i++) {
        unsigned idx = (unsigned)((hist_seq - hist_count + i) % HIST_LEN);
        level[i]     = g_hist_level[idx];
        gates[i]     = g_hist_gate[idx];
        port1[i]     = g_hist_port1[idx];
        port2[i]     = g_hist_port2[idx];
    }
    pthread_mutex_unlock(&g_mu);

    root = json_object();
    json_object_set_new(root, "configRev", json_integer(snap.config_rev));
    json_object_set_new(root, "configError", json_string(snap.config_error));
    hist       = json_object();
    hist_level = json_array();
    hist_gate  = json_array();
    hist_port1 = json_array();
    hist_port2 = json_array();
    for (i = skip; i < hist_count; i++) {
        json_array_append_new(hist_level, json_real((double)level[i]));
        json_array_append_new(hist_gate, json_integer(gates[i]));
        json_array_append_new(hist_port1, json_integer(port1[i]));
        json_array_append_new(hist_port2, json_integer(port2[i]));
    }
    json_object_set_new(hist, "seq", json_integer((json_int_t)hist_seq));
    json_object_set_new(hist, "intervalMs", json_integer(HIST_INTERVAL_MS));
    json_object_set_new(hist, "level", hist_level);
    json_object_set_new(hist, "gate", hist_gate);
    json_object_set_new(hist, "port1", hist_port1);
    json_object_set_new(hist, "port2", hist_port2);
    json_object_set_new(root, "history", hist);
    json_object_set_new(root, "levelDb", json_real(snap.level_db));
    json_object_set_new(root, "levelPercent", json_real(snap.level_percent));
    json_object_set_new(root, "gate", json_string(snap.gate));
    json_object_set_new(root, "streaming", json_boolean(snap.streaming));
    json_object_set_new(root, "enabled", json_boolean(snap.enabled));
    json_object_set_new(root, "forceOn", json_boolean(snap.force_on));
    json_object_set_new(root, "testMode", json_boolean(snap.test_mode));
    json_object_set_new(root, "trigger",
                        json_string(snap.trigger_port == 1 ? "port1" : snap.trigger_port == 2 ? "port2" : "level"));
    json_object_set_new(root, "port1Active", json_integer(snap.port1_active));
    json_object_set_new(root, "port2Active", json_integer(snap.port2_active));
    json_object_set_new(root, "streamUrl", json_string(snap.stream_url));
    json_object_set_new(root, "nodeConnected", json_boolean(snap.node_connected));
    json_object_set_new(root, "thresholdDb", json_integer(snap.threshold_db));
    json_object_set_new(root, "marginDb", json_integer(snap.margin_db));
    json_object_set_new(root, "activationDelayMs", json_integer(snap.activation_delay_ms));
    json_object_set_new(root, "deactivationDelayS", json_integer(snap.deactivation_delay_s));
    json_object_set_new(root, "maxOnS", json_integer(snap.max_on_s));
    json_object_set_new(root, "destinationIp", json_string(snap.destination_ip));
    json_object_set_new(root, "destinationPort", json_integer(snap.destination_port));
    json_object_set_new(root, "inputNode", json_string(snap.input_node));
    json_object_set_new(root, "connectedNode", json_string(snap.connected_node));
    json_object_set_new(root, "captureRate", json_integer(snap.capture_rate));
    json_object_set_new(root, "captureChannels", json_integer(snap.capture_channels));
    /* Settings, as saved. */
    json_object_set_new(root, "codec", json_string(snap.codec == CODEC_L16 ? "l16" : "pcma"));
    json_object_set_new(root, "sampleRate", json_integer(snap.sample_rate));
    json_object_set_new(root, "payloadType", json_integer(snap.payload_type));
    /* What is on the wire now. */
    json_object_set_new(root, "rtpCodec", json_string(snap.fmt.codec == CODEC_L16 ? "L16" : "PCMA"));
    json_object_set_new(root, "rtpPayloadType", json_integer(snap.fmt.payload_type));
    json_object_set_new(root, "rtpRate", json_integer(snap.fmt.rate));
    json_object_set_new(root, "packetMs", json_integer(snap.fmt.packet_ms));
    json_object_set_new(root, "packetsSent", json_integer((json_int_t)snap.packets_sent));
    json_object_set_new(root, "sendErrors", json_integer((json_int_t)snap.send_errors));
    json_object_set_new(root, "samplesDropped", json_integer((json_int_t)snap.samples_dropped));
    json_object_set_new(root, "bufferedSamples", json_integer(snap.buffered_samples));
    json_object_set_new(root, "lastError", json_string(snap.last_error));
    errors = json_array();
    for (i = 0; i < ERR_SLOTS; i++) {
        if (snap.errors[i][0] != '\0') {
            json_t* e = json_object();
            json_object_set_new(e, "area", json_string(err_area[i]));
            json_object_set_new(e, "message", json_string(snap.errors[i]));
            json_array_append_new(errors, e);
        }
    }
    json_object_set_new(root, "errors", errors);
    json_object_set_new(root, "lastRtpHeader", json_string(snap.last_rtp_header));
    /* Which audio manager: the setting, what the device looks like, and what
     * the app is doing as a result. */
    json_object_set_new(root, "manager", json_string(mgr_mode_name((enum mgr_mode)snap.manager)));
    json_object_set_new(root, "activeMode", json_string(mgr_active_name(snap.mgr.active)));
    {
        json_t* detect = json_object();
        json_t* edge   = json_object();

        json_object_set_new(detect, "known", json_boolean(snap.mgr.detect.known));
        json_object_set_new(detect, "edgeSite", json_boolean(snap.mgr.detect.edge_site));
        json_object_set_new(detect, "siteName", json_string(snap.mgr.site_name));
        json_object_set_new(detect, "proApp", json_boolean(snap.mgr.detect.pro_app));
        json_object_set_new(detect, "proInstalled", json_boolean(snap.mgr.pro_installed));
        json_object_set_new(detect, "proAppName", json_string(snap.mgr.pro_app));
        json_object_set_new(detect, "proServer", json_string(snap.mgr.pro_server));
        json_object_set_new(detect, "error", json_string(snap.mgr.error));
        json_object_set_new(root, "detect", detect);
        json_object_set_new(edge, "paging", json_boolean(snap.edge_paging));
        json_object_set_new(edge, "confirmed", json_integer(snap.edge_confirmed));
        json_object_set_new(edge, "lineIn", json_integer(snap.mgr.line_in));
        json_object_set_new(edge, "sourceActive", json_integer(snap.mgr.source_active));
        json_object_set_new(edge, "rule", json_string(edge_rule_state_name(snap.mgr.rule)));
        json_object_set_new(edge, "ruleCreated", json_boolean(snap.mgr.rule_created));
        json_object_set_new(edge, "ruleName", json_string(EDGE_RULE_NAME));
        json_object_set_new(edge, "otherRules", json_integer(snap.mgr.other_rules));
        json_object_set_new(edge, "otherRuleNames", json_string(snap.mgr.other_rule_names));
        json_object_set_new(root, "edge", edge);
    }
    /* Four significant digits keep quiet levels exact enough on a dB scale
     * (0.003162 is -90.0 dBFS) without jansson's default 17. */
    text = json_dumps(root, JSON_COMPACT | JSON_REAL_PRECISION(4));
    json_decref(root);
    return text;
}

static int json_int_field(json_t* root, const char* key, int fallback, int min_v, int max_v, bool* ok, char* err,
                          size_t err_len) {
    json_t* value = json_object_get(root, key);
    json_int_t parsed;

    if (value == NULL) {
        return fallback;
    }
    if (!json_is_integer(value)) {
        snprintf(err, err_len, "%s must be an integer", key);
        *ok = false;
        return fallback;
    }
    parsed = json_integer_value(value);
    if (parsed < min_v || parsed > max_v) {
        snprintf(err, err_len, "%s is out of range", key);
        *ok = false;
        return fallback;
    }
    return (int)parsed;
}

static bool json_bool_field(json_t* root, const char* key, bool fallback, bool* ok, char* err, size_t err_len) {
    json_t* value = json_object_get(root, key);

    if (value == NULL) {
        return fallback;
    }
    if (!json_is_boolean(value)) {
        snprintf(err, err_len, "%s must be a boolean", key);
        *ok = false;
        return fallback;
    }
    return json_is_true(value);
}

static bool json_has_config(json_t* root) {
    static const char* keys[] = {
        "enabled", "forceOn", "testMode", "trigger", "destinationIp", "destinationPort", "thresholdDb",
        "marginDb", "activationDelayMs", "deactivationDelayS", "maxOnS", "inputNode", "codec", "sampleRate",
        "payloadType", "manager", NULL,
    };
    const char** key;

    for (key = keys; *key != NULL; key++) {
        if (json_object_get(root, *key) != NULL) {
            return true;
        }
    }
    return false;
}

static char* json_ok(bool ok, const char* error) {
    json_t* reply = json_object();
    char* text;

    json_object_set_new(reply, "ok", json_boolean(ok));
    if (error != NULL) {
        json_object_set_new(reply, "error", json_string(error));
    }
    text = json_dumps(reply, JSON_COMPACT);
    json_decref(reply);
    return text;
}

static char* config_from_body(const char* body, size_t length, int* http_code) {
    json_error_t error;
    json_t* root;
    struct status_snap snap;
    struct cfg cfg;
    bool ok = true;
    char err[160];
    json_t* ip_value;
    json_t* node_value;
    unsigned rev        = 0;
    bool have_cfg;
    const char* ip;
    const char* node;
    char* text;

    err[0]       = '\0';
    *http_code   = 400;
    root         = json_loadb(body, length, 0, &error);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return json_ok(false, "Body must be a JSON object");
    }
    have_cfg = json_has_config(root);

    pthread_mutex_lock(&g_mu);
    snap = g_status;
    pthread_mutex_unlock(&g_mu);
    memset(&cfg, 0, sizeof cfg);
    cfg.enabled               = snap.enabled;
    cfg.force_on              = snap.force_on;
    cfg.test_mode             = snap.test_mode;
    cfg.trigger_port          = snap.trigger_port;
    cfg.destination_port      = snap.destination_port > 0 ? snap.destination_port : 49000;
    cfg.threshold_db          = snap.threshold_db;
    cfg.margin_db             = snap.margin_db;
    cfg.activation_delay_ms   = snap.activation_delay_ms;
    cfg.deactivation_delay_s  = snap.deactivation_delay_s;
    cfg.max_on_s              = snap.max_on_s;
    cfg.codec                 = snap.codec;
    cfg.sample_rate           = snap.sample_rate > 0 ? snap.sample_rate : 48000;
    cfg.payload_type          = snap.payload_type > 0 ? snap.payload_type : 96;
    cfg.manager               = snap.manager;
    snprintf(cfg.destination_ip, sizeof cfg.destination_ip, "%s", snap.destination_ip);
    snprintf(cfg.input_node, sizeof cfg.input_node, "%s", snap.input_node[0] != '\0' ? snap.input_node : DEFAULT_NODE);

    cfg.enabled              = json_bool_field(root, "enabled", cfg.enabled, &ok, err, sizeof err);
    cfg.force_on             = json_bool_field(root, "forceOn", cfg.force_on, &ok, err, sizeof err);
    cfg.test_mode            = json_bool_field(root, "testMode", cfg.test_mode, &ok, err, sizeof err);
    {
        json_t* trigger = json_object_get(root, "trigger");
        if (trigger != NULL) {
            const char* value = json_string_value(trigger);
            if (!json_is_string(trigger) || value == NULL) {
                ok = false;
                snprintf(err, sizeof err, "trigger must be a string");
            } else if (strcmp(value, "port1") == 0) {
                cfg.trigger_port = 1;
            } else if (strcmp(value, "port2") == 0) {
                cfg.trigger_port = 2;
            } else if (strcmp(value, "level") == 0) {
                cfg.trigger_port = 0;
            } else {
                ok = false;
                snprintf(err, sizeof err, "trigger must be level, port1, or port2");
            }
        }
    }
    cfg.destination_port     = json_int_field(root, "destinationPort", cfg.destination_port, 1, 65535, &ok, err, sizeof err);
    cfg.threshold_db         = json_int_field(root, "thresholdDb", cfg.threshold_db, -90, 0, &ok, err, sizeof err);
    cfg.margin_db            = json_int_field(root, "marginDb", cfg.margin_db, 0, 90, &ok, err, sizeof err);
    cfg.activation_delay_ms  = json_int_field(root, "activationDelayMs", cfg.activation_delay_ms, 0, 10000, &ok, err, sizeof err);
    cfg.deactivation_delay_s = json_int_field(root, "deactivationDelayS", cfg.deactivation_delay_s, 0, 60, &ok, err, sizeof err);
    cfg.max_on_s             = json_int_field(root, "maxOnS", cfg.max_on_s, 0, 3600, &ok, err, sizeof err);
    cfg.payload_type         = json_int_field(root, "payloadType", cfg.payload_type, 96, 127, &ok, err, sizeof err);
    cfg.sample_rate          = json_int_field(root, "sampleRate", cfg.sample_rate, 16000, 48000, &ok, err, sizeof err);
    if (cfg.sample_rate != 16000 && cfg.sample_rate != 48000) {
        ok = false;
        snprintf(err, sizeof err, "sampleRate must be 16000 or 48000");
    }
    {
        json_t* codec = json_object_get(root, "codec");
        if (codec != NULL) {
            const char* value = json_string_value(codec);
            if (value != NULL && strcmp(value, "pcma") == 0) {
                cfg.codec = CODEC_PCMA;
            } else if (value != NULL && strcmp(value, "l16") == 0) {
                cfg.codec = CODEC_L16;
            } else {
                ok = false;
                snprintf(err, sizeof err, "codec must be pcma or l16");
            }
        }
    }

    {
        json_t* manager = json_object_get(root, "manager");
        if (manager != NULL) {
            enum mgr_mode mode;
            if (mgr_mode_parse(json_string_value(manager), &mode)) {
                cfg.manager = mode;
            } else {
                ok = false;
                snprintf(err, sizeof err, "manager must be auto, pro or edge");
            }
        }
    }

    ip_value = json_object_get(root, "destinationIp");
    if (ip_value != NULL) {
        if (!json_is_string(ip_value)) {
            ok = false;
            snprintf(err, sizeof err, "destinationIp must be a string");
        } else {
            ip = json_string_value(ip_value);
            if (ip[0] != '\0' && !valid_ipv4(ip)) {
                ok = false;
                snprintf(err, sizeof err, "destinationIp must be a unicast IPv4 address");
            } else {
                snprintf(cfg.destination_ip, sizeof cfg.destination_ip, "%s", ip);
            }
        }
    }
    node_value = json_object_get(root, "inputNode");
    if (node_value != NULL) {
        if (!json_is_string(node_value) || !valid_node_name(json_string_value(node_value))) {
            ok = false;
            snprintf(err, sizeof err, "inputNode has unsupported characters");
        } else {
            node = json_string_value(node_value);
            snprintf(cfg.input_node, sizeof cfg.input_node, "%s", node);
        }
    }
    json_decref(root);

    if (!ok) {
        return json_ok(false, err[0] != '\0' ? err : "Invalid configuration");
    }

    pthread_mutex_lock(&g_mu);
    if (have_cfg) {
        g_pending.cfg  = cfg;
        g_pending.have = true;
        g_pending.rev  = ++g_cfg_rev_requested;
        rev            = g_pending.rev;
    }
    pthread_cond_signal(&g_ctl_cond);
    pthread_mutex_unlock(&g_mu);

    /* "rev" lets the page wait until /api/status reports configRev >= rev,
     * which means the values were written and read back (or configError). */
    *http_code = 200;
    {
        json_t* reply = json_object();
        json_object_set_new(reply, "ok", json_true());
        if (rev > 0) {
            json_object_set_new(reply, "rev", json_integer(rev));
        }
        text = json_dumps(reply, JSON_COMPACT);
        json_decref(reply);
    }
    return text;
}

static void http_send(int fd, int code, const char* reason, const char* body) {
    char header[256];
    int header_len;

    if (body == NULL) {
        body = "";
    }
    header_len = snprintf(header, sizeof header,
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n\r\n",
                          code, reason, strlen(body));
    if (header_len > 0) {
        send(fd, header, (size_t)header_len, MSG_NOSIGNAL);
        send(fd, body, strlen(body), MSG_NOSIGNAL);
    }
}

static const char* header_token(const char* headers, const char* name) {
    size_t name_len = strlen(name);
    const char* line = headers;

    while (line != NULL && line[0] != '\0') {
        const char* next = strstr(line, "\r\n");
        size_t line_len  = next != NULL ? (size_t)(next - line) : strlen(line);

        if (line_len > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            return line + name_len + 1;
        }
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }
    return NULL;
}

static void handle_client(int fd) {
    char request[8192];
    size_t used = 0;
    char* header_end;
    char method[8];
    char path[256];
    const char* length_hdr;
    long content_length = 0;
    char* body;
    struct timeval tv;

    /* The AXIS reverse proxy on loopback sends a whole request at once. A short
     * timeout keeps one stalled client from blocking the page's status polls. */
    tv.tv_sec  = 0;
    tv.tv_usec = 500000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    while (used + 1 < sizeof request) {
        ssize_t got = recv(fd, request + used, sizeof request - 1 - used, 0);
        if (got <= 0) {
            break;
        }
        used += (size_t)got;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL) {
            break;
        }
    }
    request[used] = '\0';
    header_end    = strstr(request, "\r\n\r\n");
    if (sscanf(request, "%7s %255s", method, path) != 2 || header_end == NULL) {
        http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"Bad request\"}");
        return;
    }
    body          = header_end + 4;
    header_end[0] = '\0';
    length_hdr    = header_token(request, "Content-Length");
    header_end[0] = '\r';
    if (length_hdr != NULL) {
        content_length = strtol(length_hdr, NULL, 10);
        if (content_length < 0 || content_length > 4096) {
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"Body is too large\"}");
            return;
        }
        while ((size_t)(body - request) + (size_t)content_length > used && used + 1 < sizeof request) {
            ssize_t got = recv(fd, request + used, sizeof request - 1 - used, 0);
            if (got <= 0) {
                break;
            }
            used += (size_t)got;
            request[used] = '\0';
        }
        if ((size_t)(body - request) < used) {
            size_t have = used - (size_t)(body - request);
            if ((size_t)content_length > have) {
                content_length = (long)have;
            }
        } else {
            content_length = 0;
        }
    }
    if (strcmp(method, "GET") == 0 && strstr(path, "/api/status") != NULL) {
        const char* q    = strstr(path, "since=");
        long long since  = q != NULL ? strtoll(q + 6, NULL, 10) : -1;
        char* json       = status_json(since < 0 ? -1 : since);
        http_send(fd, 200, "OK", json != NULL ? json : "{}");
        free(json);
        return;
    }
    if (strcmp(method, "POST") == 0 && strstr(path, "/api/config") != NULL) {
        int code   = 400;
        char* json = config_from_body(body, (size_t)content_length, &code);
        http_send(fd, code, code == 200 ? "OK" : "Bad Request", json != NULL ? json : "{\"ok\":false}");
        free(json);
        return;
    }
    http_send(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"Not found\"}");
}

static void* http_thread(void* arg) {
    struct impl* impl = arg;

    while (impl->http_listen_fd >= 0) {
        int client = accept(impl->http_listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EBADF || errno == EINVAL) {
                break;
            }
            continue;
        }
        handle_client(client);
        close(client);
    }
    return NULL;
}

static int start_http(struct impl* impl) {
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        syslog(LOG_ERR, "Status server failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    impl->http_listen_fd = fd;
    if (pthread_create(&impl->http_thread, NULL, http_thread, impl) != 0) {
        close(fd);
        impl->http_listen_fd = -1;
        return -1;
    }
    syslog(LOG_INFO, "Status server on 127.0.0.1:%d", HTTP_PORT);
    return 0;
}

/* ---- Control thread: parameters, off the audio loop -------------------- */

static void deadline_in(struct timespec* ts, int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(ms / 1000);
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void* control_thread(void* arg) {
    struct impl* impl = arg;
    struct cfg current;
    char err[160];
    int64_t next_load = 0;

    read_cfg(impl->params, &current, err, sizeof err);
    pthread_mutex_lock(&g_mu);
    while (!g_ctl_stop) {
        struct pending_cfg job;
        int64_t now = now_ms();
        bool save_ok = true;
        char save_err[160];

        if (!g_pending.have && now < next_load) {
            struct timespec ts;
            deadline_in(&ts, next_load - now);
            pthread_cond_timedwait(&g_ctl_cond, &g_mu, &ts);
            continue;
        }
        job            = g_pending;
        g_pending.have = false;
        pthread_mutex_unlock(&g_mu);

        save_err[0] = '\0';
        if (job.have) {
            save_ok = write_cfg(impl->params, &job.cfg, &current, save_err, sizeof save_err);
        }
        if (job.have || now >= next_load) {
            read_cfg(impl->params, &current, err, sizeof err);
            next_load = now_ms() + CFG_RELOAD_MS;
            pthread_mutex_lock(&g_mu);
            g_ctl.cfg      = current;
            g_ctl.have_cfg = true;
            snprintf(g_ctl.cfg_error, sizeof g_ctl.cfg_error, "%s", err);
            if (job.have) {
                g_ctl.rev = job.rev;
                snprintf(g_ctl.save_error, sizeof g_ctl.save_error, "%s", save_ok ? "" : save_err);
            }
            pthread_mutex_unlock(&g_mu);
        }
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static int start_control(struct impl* impl) {
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_ctl_cond, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&impl->control_thread, NULL, control_thread, impl) != 0) {
        return -1;
    }
    impl->control_started = true;
    return 0;
}

static void stop_control(struct impl* impl) {
    if (!impl->control_started) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    g_ctl_stop = true;
    pthread_cond_signal(&g_ctl_cond);
    pthread_mutex_unlock(&g_mu);
    pthread_join(impl->control_thread, NULL);
    impl->control_started = false;
}

static void shutdown_app(struct impl* impl) {
    if (g_event_started) {
        while (g_atomic_int_get(&g_event_ready) == 0) {
            usleep(1000);
        }
        g_atomic_int_set(&g_event_stop, 1);
        if (g_event_loop != NULL) {
            g_main_context_wakeup(g_main_loop_get_context(g_event_loop));
        }
        pthread_join(g_event_thread, NULL);
        g_event_started = false;
    }
    if (impl->http_listen_fd >= 0) {
        int fd               = impl->http_listen_fd;
        impl->http_listen_fd = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        pthread_join(impl->http_thread, NULL);
    }
    stop_control(impl);
    mgr_stop();
    if (g_rtp_fd >= 0) {
        close(g_rtp_fd);
        g_rtp_fd = -1;
    }
    destroy_stream(impl);
    if (g_registry_hooked) {
        spa_hook_remove(&impl->registry_listener);
        g_registry_hooked = false;
    }
    if (impl->registry != NULL) {
        pw_proxy_destroy((struct pw_proxy*)impl->registry);
        impl->registry = NULL;
    }
    if (impl->core_hooked) {
        spa_hook_remove(&impl->core_listener);
        impl->core_hooked = false;
    }
    if (impl->core != NULL) {
        pw_core_disconnect(impl->core);
        impl->core = NULL;
    }
    if (impl->context != NULL) {
        pw_context_destroy(impl->context);
        impl->context = NULL;
    }
    if (impl->loop != NULL) {
        pw_main_loop_destroy(impl->loop);
        impl->loop = NULL;
    }
    if (g_pw_inited) {
        pw_deinit();
        g_pw_inited = false;
    }
    if (impl->params != NULL) {
        ax_parameter_free(impl->params);
        impl->params = NULL;
    }
    closelog();
}

int main(int argc, char* argv[]) {
    struct impl* impl = &g_impl;
    struct pw_loop* loop;
    struct timespec interval;
    GError* error = NULL;
    char cfg_err[160];
    int res;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    memset(impl, 0, sizeof *impl);
    impl->http_listen_fd = -1;
    /* Block the signals the loop handles before any thread exists. Library
     * threads (GDBus starts one inside ax_parameter_new) then inherit the
     * mask, so SIGTERM always reaches on_signal() and the app shuts down in
     * order, instead of dying on whichever thread the signal landed on. */
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &set, NULL);
    }
    {
        uint8_t rnd[10];
        random_bytes(rnd, sizeof rnd);
        rtp_tx_init(&g_rtp, (uint16_t)((rnd[0] << 8) | rnd[1]),
                    ((uint32_t)rnd[2] << 24) | ((uint32_t)rnd[3] << 16) | ((uint32_t)rnd[4] << 8) | rnd[5],
                    ((uint32_t)rnd[6] << 24) | ((uint32_t)rnd[7] << 16) | ((uint32_t)rnd[8] << 8) | rnd[9]);
    }

    impl->params = ax_parameter_new(APP_NAME, &error);
    if (impl->params == NULL) {
        syslog(LOG_ERR, "AXParameter failed: %s", error != NULL ? error->message : "unknown");
        g_clear_error(&error);
    }
    read_cfg(impl->params, &g_cfg, cfg_err, sizeof cfg_err);
    if (cfg_err[0] != '\0') {
        set_error(ERR_CONFIG, cfg_err);
    }
    publish_status("inactive");

    setenv("PIPEWIRE_DEBUG", "2", 1);
    pw_init(&argc, &argv);
    g_pw_inited = true;
    impl->loop  = pw_main_loop_new(NULL);
    if (impl->loop == NULL) {
        syslog(LOG_ERR, "Could not create the PipeWire loop");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    loop = pw_main_loop_get_loop(impl->loop);
    pw_loop_add_signal(loop, SIGINT, on_signal, impl);
    pw_loop_add_signal(loop, SIGTERM, on_signal, impl);

    impl->context = pw_context_new(loop, NULL, 0);
    impl->core    = impl->context != NULL ? pw_context_connect(impl->context, NULL, 0) : NULL;
    if (impl->core == NULL) {
        syslog(LOG_ERR, "Could not connect to PipeWire");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    pw_core_add_listener(impl->core, &impl->core_listener, &core_events, impl);
    impl->core_hooked = true;
    impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
    if (impl->registry == NULL) {
        syslog(LOG_ERR, "Could not open the PipeWire registry");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    pw_registry_add_listener(impl->registry, &impl->registry_listener, &registry_events, impl);
    g_registry_hooked = true;

    impl->timer_source = pw_loop_add_timer(loop, on_timeout, impl);
    interval.tv_sec    = 0;
    interval.tv_nsec   = 10000000L;
    res = impl->timer_source != NULL ? pw_loop_update_timer(loop, impl->timer_source, NULL, &interval, false) : -1;
    if (impl->timer_source == NULL || res < 0) {
        syslog(LOG_ERR, "Could not start the timer%s%s", res < 0 && impl->timer_source != NULL ? ": " : "",
               res < 0 && impl->timer_source != NULL ? strerror(-res) : "");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    if (start_control(impl) != 0) {
        syslog(LOG_ERR, "Could not start the control thread");
        shutdown_app(impl);
        return EXIT_FAILURE;
    }
    if (start_http(impl) != 0) {
        syslog(LOG_ERR, "Continuing without the status server");
    }
    if (pthread_create(&g_event_thread, NULL, event_thread, NULL) == 0) {
        g_event_started = true;
    } else {
        syslog(LOG_ERR, "Input events unavailable");
    }
    if (!mgr_start((enum mgr_mode)g_cfg.manager)) {
        syslog(LOG_ERR, "Could not start the audio manager check; Auto behaves as Audio Manager Pro");
    }
    mgr_get(&g_mgr);
    g_edge = g_mgr.active == ACTIVE_EDGE;

    configure_audio();
    syslog(LOG_INFO, "Line Input RTP started. Node %s, threshold %d dBFS, release %d dB, audio manager %s",
           g_cfg.input_node, g_cfg.threshold_db, g_cfg.margin_db, mgr_mode_name((enum mgr_mode)g_cfg.manager));
    pw_main_loop_run(impl->loop);

    syslog(LOG_INFO, "Line Input RTP stopped");
    shutdown_app(impl);
    return g_exit_code;
}
