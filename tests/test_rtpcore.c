/**
 * Host unit tests for app/rtpcore.c. Build and run with `make -C tests`.
 * SPDX-License-Identifier: MIT
 */
#include "rtpcore.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(cond)) {                                                                  \
            g_failures++;                                                               \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, __func__, #cond); \
        }                                                                               \
    } while (0)

#define CHECK_STR(a, b)                                                                   \
    do {                                                                                  \
        const char* a_ = (a);                                                             \
        const char* b_ = (b);                                                             \
        g_checks++;                                                                       \
        if (a_ == NULL || b_ == NULL || strcmp(a_, b_) != 0) {                            \
            g_failures++;                                                                 \
            fprintf(stderr, "%s:%d: %s: \"%s\" != \"%s\"\n", __FILE__, __LINE__, __func__, \
                    a_ ? a_ : "(null)", b_ ? b_ : "(null)");                              \
        }                                                                                 \
    } while (0)

/* ---------------------------------------------------------------- A-law */

/* Reference decoder: Sun Microsystems g711.c alaw2linear, the standard
 * companion to the encoder under test. */
static int alaw_to_linear(uint8_t a_val) {
    int t;
    int seg;

    a_val ^= 0x55;
    t   = (a_val & 0x0F) << 4;
    seg = (a_val & 0x70) >> 4;
    switch (seg) {
    case 0:
        t += 8;
        break;
    case 1:
        t += 0x108;
        break;
    default:
        t += 0x108;
        t <<= seg - 1;
    }
    return (a_val & 0x80) ? t : -t;
}

static void test_alaw_known_values(void) {
    CHECK(linear_to_alaw(0) == 0xD5);
    CHECK(linear_to_alaw(-1) == 0x55);
    CHECK(linear_to_alaw(-8) == 0x55);
    CHECK(linear_to_alaw(32767) == 0xAA);
    CHECK(linear_to_alaw(-32768) == 0x2A);
    CHECK(alaw_to_linear(0xD5) == 8);
    CHECK(alaw_to_linear(0x55) == -8);
    CHECK(alaw_to_linear(0xAA) == 32256);
    CHECK(alaw_to_linear(0x2A) == -32256);
}

/* Every one of the 256 codes decodes to a level that encodes back to itself. */
static void test_alaw_codes_round_trip(void) {
    int code;
    int bad = 0;

    for (code = 0; code < 256; code++) {
        int linear = alaw_to_linear((uint8_t)code);
        if (linear_to_alaw((int16_t)linear) != (uint8_t)code) {
            if (bad++ < 5) {
                fprintf(stderr, "  code 0x%02X -> %d -> 0x%02X\n", code, linear, linear_to_alaw((int16_t)linear));
            }
        }
    }
    CHECK(bad == 0);
}

/* Across all 65,536 inputs the encoder is monotonic and the quantization
 * error stays within A-law's step size for that segment. */
static void test_alaw_all_inputs(void) {
    int x;
    int prev      = -40000;
    int bad_order = 0;
    int bad_error = 0;
    int bad_sign  = 0;

    for (x = -32768; x <= 32767; x++) {
        int y     = alaw_to_linear(linear_to_alaw((int16_t)x));
        int err   = abs(y - x);
        int limit = abs(x) / 16 + 16;
        if (y < prev) {
            bad_order++;
        }
        if (err > limit) {
            if (bad_error++ < 5) {
                fprintf(stderr, "  %d -> %d, error %d > %d\n", x, y, err, limit);
            }
        }
        if ((x >= 8 && y <= 0) || (x < -8 && y >= 0)) {
            bad_sign++;
        }
        prev = y;
    }
    CHECK(bad_order == 0);
    CHECK(bad_error == 0);
    CHECK(bad_sign == 0);
}

/* ---------------------------------------------------------------- validation */

static void test_valid_ipv4(void) {
    CHECK(valid_ipv4("192.168.1.10"));
    CHECK(valid_ipv4("10.0.0.1"));
    CHECK(valid_ipv4("1.0.0.0"));
    CHECK(valid_ipv4("223.255.255.254"));
    CHECK(valid_ipv4("192.168.1.255")); /* may be a host on a wider subnet */

    CHECK(!valid_ipv4(NULL));
    CHECK(!valid_ipv4(""));
    CHECK(!valid_ipv4("0.0.0.0"));
    CHECK(!valid_ipv4("0.1.2.3"));
    CHECK(!valid_ipv4("127.0.0.1")); /* loopback: would send to the camera itself */
    CHECK(!valid_ipv4("127.255.255.254"));
    CHECK(!valid_ipv4("224.0.0.1"));       /* multicast */
    CHECK(!valid_ipv4("239.255.255.250")); /* multicast */
    CHECK(!valid_ipv4("240.0.0.1"));       /* reserved */
    CHECK(!valid_ipv4("255.255.255.255")); /* broadcast */
    CHECK(!valid_ipv4("256.1.1.1"));
    CHECK(!valid_ipv4("1.2.3.256"));
    CHECK(!valid_ipv4("192.168.01.1")); /* leading zero */
    CHECK(!valid_ipv4("1.2.3"));
    CHECK(!valid_ipv4("1.2.3.4.5"));
    CHECK(!valid_ipv4("1.2.3.4 "));
    CHECK(!valid_ipv4(" 1.2.3.4"));
    CHECK(!valid_ipv4("1.2.3.4x"));
    CHECK(!valid_ipv4("-1.2.3.4"));
    CHECK(!valid_ipv4("a.b.c.d"));
    CHECK(!valid_ipv4("::1"));
}

static void test_valid_node_name(void) {
    char longest[NAME_MAX_LEN + 1];
    char too_long[NAME_MAX_LEN + 2];

    memset(longest, 'a', NAME_MAX_LEN);
    longest[NAME_MAX_LEN] = '\0';
    memset(too_long, 'a', NAME_MAX_LEN + 1);
    too_long[NAME_MAX_LEN + 1] = '\0';

    CHECK(valid_node_name("AudioDevice0Input0.Unprocessed"));
    CHECK(valid_node_name("a"));
    CHECK(valid_node_name("A-b_c.9"));
    CHECK(valid_node_name(longest));

    CHECK(!valid_node_name(NULL));
    CHECK(!valid_node_name(""));
    CHECK(!valid_node_name(too_long));
    CHECK(!valid_node_name("has space"));
    CHECK(!valid_node_name("semi;colon"));
    CHECK(!valid_node_name("slash/node"));
    CHECK(!valid_node_name("quote\""));
}

static void test_percent_to_db(void) {
    CHECK(fabs(percent_to_db(100.0)) < 1e-9);
    CHECK(fabs(percent_to_db(10.0) - -20.0) < 1e-9);
    CHECK(fabs(percent_to_db(1.0) - -40.0) < 1e-9);
    CHECK(percent_to_db(0.0) <= -120.0);
    CHECK(percent_to_db(-5.0) <= -120.0);
}

/* ---------------------------------------------------------------- gate */

/* Linear 0-100 peak for a dBFS level. */
static double pct(double db) {
    return 100.0 * pow(10.0, db / 20.0);
}

/* Defaults from manifest.json, enabled with a destination and a live capture. */
static struct gate_input base_input(void) {
    struct gate_input in;

    memset(&in, 0, sizeof in);
    in.enabled              = true;
    in.have_destination     = true;
    in.format_ready         = true;
    in.port_active          = -1;
    in.level_percent        = 0.0;
    in.threshold_db         = -45;
    in.margin_db            = 6;
    in.activation_delay_ms  = 200;
    in.deactivation_delay_s = 2;
    in.max_on_s             = 0;
    return in;
}

/* Steps the gate every 10 ms (the ACAP's timer period) from *now up to and
 * including `until`, and returns the first non-NONE action. */
static struct gate_result run_until(struct gate* g, const struct gate_input* in, int64_t* now, int64_t until) {
    struct gate_result first;

    first.action  = GATE_NONE;
    first.reason  = NULL;
    first.rearmed = false;
    while (*now <= until) {
        struct gate_result r = gate_step(g, in, *now);
        if (first.action == GATE_NONE && r.action != GATE_NONE) {
            first = r;
        }
        *now += 10;
    }
    return first;
}

static void test_gate_disabled_or_no_destination(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.enabled       = false;
    in.level_percent = 100.0;
    r                = run_until(&g, &in, &now, 5000);
    CHECK(r.action == GATE_NONE);
    CHECK(!g.streaming);

    in.enabled          = true;
    in.have_destination = false;
    r                   = run_until(&g, &in, &now, 9000);
    CHECK(r.action == GATE_NONE);
    CHECK(!g.streaming);
}

static void test_gate_level_opens_after_activation_delay(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.level_percent = pct(-30.0);

    r = gate_step(&g, &in, now);
    CHECK(r.action == GATE_NONE);
    CHECK_STR(gate_state(&g, false), "pending");

    r = gate_step(&g, &in, now + 190);
    CHECK(r.action == GATE_NONE);

    r = gate_step(&g, &in, now + 200);
    CHECK(r.action == GATE_START);
    CHECK_STR(r.reason, "level");
    CHECK(g.streaming);
    CHECK(g.active_since == now + 200);
    CHECK_STR(gate_state(&g, false), "active");
}

static void test_gate_level_at_threshold_counts_as_hot(void) {
    struct gate g;
    struct gate_input in = base_input();
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.level_percent       = pct(-45.0) * 1.000001;
    CHECK(gate_step(&g, &in, now).action == GATE_START);

    memset(&g, 0, sizeof g);
    in.level_percent = pct(-45.5);
    CHECK(gate_step(&g, &in, now).action == GATE_NONE);
}

/* Current behaviour (AUDIT.md M5): one quiet tick during the activation
 * delay restarts the delay. */
static void test_gate_dip_restarts_activation_delay(void) {
    struct gate g;
    struct gate_input in = base_input();
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.level_percent = pct(-30.0);
    CHECK(gate_step(&g, &in, 1000).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 1150).action == GATE_NONE);
    in.level_percent = pct(-60.0);
    CHECK(gate_step(&g, &in, 1160).action == GATE_NONE);
    CHECK(g.hot_since == 0);
    CHECK_STR(gate_state(&g, false), "inactive");
    in.level_percent = pct(-30.0);
    CHECK(gate_step(&g, &in, 1170).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 1250).action == GATE_NONE); /* 200 ms after 1000, but delay restarted */
    CHECK(gate_step(&g, &in, 1370).action == GATE_START);
    (void)now;
}

static void test_gate_hysteresis_and_release_hold(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.level_percent       = pct(-30.0);
    CHECK(gate_step(&g, &in, now).action == GATE_START);

    /* Between the release level (-51) and the threshold (-45): keeps sending, no hold. */
    in.level_percent = pct(-48.0);
    r                = run_until(&g, &in, &now, 10000);
    CHECK(r.action == GATE_NONE);
    CHECK(g.streaming);
    CHECK(g.quiet_since == 0);
    CHECK_STR(gate_state(&g, false), "active");

    /* Below the release level: holds for the deactivation delay, then closes. */
    in.level_percent = pct(-60.0);
    r                = gate_step(&g, &in, 20000);
    CHECK(r.action == GATE_NONE);
    CHECK_STR(gate_state(&g, false), "holding");
    CHECK(gate_step(&g, &in, 21990).action == GATE_NONE);
    r = gate_step(&g, &in, 22000);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "quiet");
    CHECK(!g.streaming);
    CHECK_STR(gate_state(&g, false), "inactive");
}

static void test_gate_sound_during_hold_cancels_it(void) {
    struct gate g;
    struct gate_input in = base_input();
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.level_percent       = pct(-30.0);
    CHECK(gate_step(&g, &in, now).action == GATE_START);
    in.level_percent = pct(-80.0);
    CHECK(gate_step(&g, &in, 2000).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 3500).action == GATE_NONE);
    in.level_percent = pct(-30.0);
    CHECK(gate_step(&g, &in, 3600).action == GATE_NONE);
    CHECK(g.quiet_since == 0);
    in.level_percent = pct(-80.0);
    CHECK(gate_step(&g, &in, 3700).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 5690).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 5700).action == GATE_STOP);
}

/* Threshold -88, margin 6: the release level would be -94 but is floored at
 * -90, so digital silence (0 %, -120 dBFS) still closes the gate. */
static void test_gate_release_floor(void) {
    struct gate g;
    struct gate_input in = base_input();
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.threshold_db         = -88;
    in.margin_db            = 6;
    in.activation_delay_ms  = 0;
    in.deactivation_delay_s = 0;
    in.level_percent        = pct(-80.0);
    CHECK(gate_step(&g, &in, now).action == GATE_START);
    in.level_percent = pct(-91.0);
    CHECK(gate_step(&g, &in, now + 10).action == GATE_STOP);

    memset(&g, 0, sizeof g);
    in.level_percent = pct(-80.0);
    CHECK(gate_step(&g, &in, now).action == GATE_START);
    in.level_percent = 0.0;
    CHECK(gate_step(&g, &in, now + 10).action == GATE_STOP);

    /* -89 is above the floored release level, so the gate stays open. */
    memset(&g, 0, sizeof g);
    in.level_percent = pct(-80.0);
    CHECK(gate_step(&g, &in, now).action == GATE_START);
    in.level_percent = pct(-89.0);
    CHECK(gate_step(&g, &in, now + 10).action == GATE_NONE);
    CHECK(g.streaming);
}

static void test_gate_max_on_locks_out_until_idle(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.max_on_s            = 5;
    in.level_percent       = pct(-30.0);
    CHECK(gate_step(&g, &in, 1000).action == GATE_START);
    CHECK(gate_step(&g, &in, 5990).action == GATE_NONE);
    r = gate_step(&g, &in, 6000);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "max on time");
    CHECK(g.waiting_for_clear);
    CHECK_STR(gate_state(&g, false), "waiting");

    /* Still loud: stays locked out, no restart. */
    now = 6010;
    r   = run_until(&g, &in, &now, 20000);
    CHECK(r.action == GATE_NONE);
    CHECK(!g.streaming);
    CHECK(g.waiting_for_clear);

    /* Quiet: re-arms on the first idle tick. */
    in.level_percent = pct(-80.0);
    r                = gate_step(&g, &in, 20010);
    CHECK(r.action == GATE_NONE);
    CHECK(r.rearmed);
    CHECK(!g.waiting_for_clear);
    CHECK_STR(gate_state(&g, false), "inactive");

    /* Loud again: opens normally. */
    in.level_percent = pct(-30.0);
    r                = gate_step(&g, &in, 20020);
    CHECK(r.action == GATE_START);
    CHECK(!r.rearmed);
}

/* Between release and threshold is neither hot nor idle, so it doesn't re-arm. */
static void test_gate_max_on_needs_true_idle(void) {
    struct gate g;
    struct gate_input in = base_input();

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.max_on_s            = 1;
    in.level_percent       = pct(-30.0);
    CHECK(gate_step(&g, &in, 1000).action == GATE_START);
    CHECK(gate_step(&g, &in, 2000).action == GATE_STOP);
    in.level_percent = pct(-48.0);
    CHECK(!gate_step(&g, &in, 2010).rearmed);
    CHECK(g.waiting_for_clear);
}

static void test_gate_max_on_disabled_by_zero(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.level_percent       = pct(-30.0);
    r                      = run_until(&g, &in, &now, 3600 * 1000);
    CHECK(r.action == GATE_START);
    CHECK(g.streaming);
}

static void test_gate_force_on(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;
    int64_t now = 1000;

    memset(&g, 0, sizeof g);
    in.force_on = true;
    in.max_on_s = 1;
    r           = gate_step(&g, &in, now);
    CHECK(r.action == GATE_START);
    CHECK_STR(r.reason, "force on");
    /* Silent and past max-on: force on keeps sending (AUDIT.md L9). */
    r = run_until(&g, &in, &now, 10000);
    CHECK(r.action == GATE_NONE);
    CHECK(g.streaming);

    /* Needs a capture stream. */
    in.format_ready = false;
    r               = gate_step(&g, &in, 20000);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "no capture");
}

static void test_gate_force_on_clears_max_on_lockout(void) {
    struct gate g;
    struct gate_input in = base_input();

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.max_on_s            = 1;
    in.level_percent       = pct(-30.0);
    CHECK(gate_step(&g, &in, 1000).action == GATE_START);
    CHECK(gate_step(&g, &in, 2000).action == GATE_STOP);
    CHECK(g.waiting_for_clear);
    in.force_on = true;
    CHECK(gate_step(&g, &in, 2010).action == GATE_START);
    CHECK(!g.waiting_for_clear);
}

static void test_gate_test_mode(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;

    memset(&g, 0, sizeof g);
    in.test_mode    = true;
    in.format_ready = false; /* the tone doesn't need capture */
    r               = gate_step(&g, &in, 1000);
    CHECK(r.action == GATE_START);
    CHECK_STR(r.reason, "test tone");
    CHECK_STR(gate_state(&g, true), "test");
    CHECK(gate_step(&g, &in, 1010).action == GATE_NONE);

    in.enabled = false;
    r          = gate_step(&g, &in, 1020);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "disabled");
}

static void test_gate_no_capture(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;

    memset(&g, 0, sizeof g);
    in.activation_delay_ms = 0;
    in.level_percent       = pct(-30.0);
    CHECK(gate_step(&g, &in, 1000).action == GATE_START);
    in.format_ready = false;
    r               = gate_step(&g, &in, 1010);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "no capture");
    CHECK(gate_step(&g, &in, 1020).action == GATE_NONE);
}

static void test_gate_port_trigger(void) {
    struct gate g;
    struct gate_input in = base_input();
    struct gate_result r;

    memset(&g, 0, sizeof g);
    in.trigger_port  = 1;
    in.level_percent = 100.0; /* ignored for a port trigger */

    in.port_active = -1;
    CHECK(gate_step(&g, &in, 1000).action == GATE_NONE);
    CHECK(!g.streaming);

    in.port_active = 0;
    CHECK(gate_step(&g, &in, 1010).action == GATE_NONE);

    in.port_active = 1;
    CHECK(gate_step(&g, &in, 1020).action == GATE_NONE);
    r = gate_step(&g, &in, 1220);
    CHECK(r.action == GATE_START);
    CHECK_STR(r.reason, "input");

    in.port_active = 0;
    CHECK(gate_step(&g, &in, 1230).action == GATE_NONE);
    CHECK_STR(gate_state(&g, false), "holding");
    r = gate_step(&g, &in, 3230);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "quiet");

    /* Port state lost while sending: stop. */
    in.port_active = 1;
    CHECK(gate_step(&g, &in, 4000).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 4200).action == GATE_START);
    in.port_active = -1;
    r              = gate_step(&g, &in, 4210);
    CHECK(r.action == GATE_STOP);
    CHECK_STR(r.reason, "input state unknown");
}

static void test_gate_stop_helper(void) {
    struct gate g;

    memset(&g, 0, sizeof g);
    CHECK(!gate_stop(&g));
    gate_start(&g, 500);
    CHECK(g.streaming);
    CHECK(g.active_since == 500);
    g.quiet_since = 600;
    CHECK(gate_stop(&g));
    CHECK(!g.streaming);
    CHECK(g.quiet_since == 0);
    CHECK(g.hot_since == 0);
    CHECK(!gate_stop(&g));
}

/* Disabling while the activation delay runs abandons it: the page shows
 * "inactive", and re-enabling waits the full delay again. */
static void test_gate_disable_forgets_pending(void) {
    struct gate g;
    struct gate_input in = base_input();

    memset(&g, 0, sizeof g);
    in.level_percent = pct(-30.0);
    CHECK(gate_step(&g, &in, 1000).action == GATE_NONE);
    CHECK_STR(gate_state(&g, false), "pending");
    in.enabled = false;
    CHECK(gate_step(&g, &in, 1100).action == GATE_NONE);
    CHECK(g.hot_since == 0);
    CHECK_STR(gate_state(&g, false), "inactive");
    in.enabled = true;
    CHECK(gate_step(&g, &in, 5000).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 5190).action == GATE_NONE);
    CHECK(gate_step(&g, &in, 5200).action == GATE_START);

    /* Same for losing the capture stream. */
    memset(&g, 0, sizeof g);
    CHECK(gate_step(&g, &in, 1000).action == GATE_NONE);
    in.format_ready = false;
    CHECK(gate_step(&g, &in, 1010).action == GATE_NONE);
    CHECK_STR(gate_state(&g, false), "inactive");
}

/* ---------------------------------------------------------------- RTP */

static uint32_t header_ts(const uint8_t* h) {
    return ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
}

static void test_rtp_header_layout(void) {
    struct rtp_tx tx;
    uint8_t h[RTP_HEADER_LEN];

    rtp_tx_init(&tx, 0x1234, 0xA1B2C3D4, 0xDEADBEEF);
    rtp_tx_header(&tx, h);
    CHECK(h[0] == 0x80);        /* V=2, P=0, X=0, CC=0 */
    CHECK(h[1] == (0x80 | 8));  /* marker on the first packet, PT 8 = PCMA */
    CHECK(h[2] == 0x12 && h[3] == 0x34);
    CHECK(h[4] == 0xA1 && h[5] == 0xB2 && h[6] == 0xC3 && h[7] == 0xD4);
    CHECK(h[8] == 0xDE && h[9] == 0xAD && h[10] == 0xBE && h[11] == 0xEF);

    rtp_tx_advance(&tx, 160);
    rtp_tx_header(&tx, h);
    CHECK(h[1] == 8); /* marker only on the first packet of a talkspurt */
    CHECK(h[2] == 0x12 && h[3] == 0x35);
    CHECK(header_ts(h) == 0xA1B2C3D4u + 160u);
}

static void test_rtp_sequence_and_timestamp_wrap(void) {
    struct rtp_tx tx;

    rtp_tx_init(&tx, 0xFFFF, 0xFFFFFFF0u, 1);
    rtp_tx_advance(&tx, 160);
    CHECK(tx.seq == 0);
    CHECK(tx.timestamp == 0x90u);
}

/* AUDIT.md M1: after a quiet gap the timestamp jumps by the gap, less the
 * pre-roll that was captured before the gate opened. */
static void test_rtp_timestamp_tracks_silence(void) {
    struct rtp_tx tx;
    int i;

    rtp_tx_init(&tx, 100, 1000, 7);
    rtp_tx_talkspurt(&tx, 5000, 1600); /* first talkspurt: nothing to skip */
    CHECK(tx.timestamp == 1000);
    CHECK(tx.marker);
    for (i = 0; i < 5; i++) {
        rtp_tx_advance(&tx, 160);
    }
    CHECK(tx.timestamp == 1800);
    CHECK(!tx.marker);

    rtp_tx_stop(&tx, 10000);
    rtp_tx_talkspurt(&tx, 13000, 1600); /* 3 s gap = 24,000 samples, 1,600 of them pre-roll */
    CHECK(tx.timestamp == 1800 + 24000 - 1600);
    CHECK(tx.marker);
    CHECK(tx.seq == 105); /* sequence numbers don't skip: no packets were lost */

    /* A gap shorter than the pre-roll leaves the timestamp alone. */
    rtp_tx_stop(&tx, 20000);
    rtp_tx_talkspurt(&tx, 20100, 1600);
    CHECK(tx.timestamp == 1800 + 24000 - 1600);

    /* Long gaps wrap modulo 2^32. */
    rtp_tx_init(&tx, 0, 0xFFFFFF00u, 7);
    rtp_tx_stop(&tx, 0);
    rtp_tx_talkspurt(&tx, 1000, 0);
    CHECK(tx.timestamp == (uint32_t)(0xFFFFFF00u + 8000u));
}

/* AUDIT.md M8: the tone follows wall-clock time, not the number of ticks. */
static void test_clock_samples_due(void) {
    CHECK(clock_samples_due(1000, 1000, 8000, 0) == 0);
    CHECK(clock_samples_due(1000, 990, 8000, 0) == 0);
    CHECK(clock_samples_due(1000, 1010, 8000, 0) == 80);
    CHECK(clock_samples_due(1000, 1010, 8000, 80) == 0);
    CHECK(clock_samples_due(1000, 1037, 8000, 80) == 216);
    CHECK(clock_samples_due(1000, 1000 + 3600000, 8000, 0) == 28800000u);
    CHECK(clock_samples_due(1000, 1010, 8000, 500) == 0);
}

/* ---------------------------------------------------------------- decimator */

static struct decimator g_dec;

/* Gain in dB for a sine of `freq` Hz at `rate` Hz decimated to `out` Hz, from the RMS of the output
 * once the filter has settled. A peak would depend on where the 8 kHz samples
 * land on the wave. */
static double decimator_gain_db(unsigned rate, unsigned out, double freq) {
    const double pi = 3.14159265358979323846;
    const double amp = 0.5;
    unsigned n;
    unsigned outputs = 0;
    unsigned counted = 0;
    double sumsq     = 0.0;

    if (!decimator_init(&g_dec, rate, out)) {
        return 999.0;
    }
    for (n = 0; n < rate; n++) { /* one second */
        float out;
        if (decimator_push(&g_dec, (float)(amp * sin(2.0 * pi * freq * n / rate)), &out)) {
            outputs++;
            if (outputs > (unsigned)g_dec.taps) {
                sumsq += (double)out * (double)out;
                counted++;
            }
        }
    }
    if (counted == 0 || sumsq <= 0.0) {
        return -200.0;
    }
    return 20.0 * log10(sqrt(2.0 * sumsq / counted) / amp);
}

static void test_decimator_rates(void) {
    CHECK(decimator_init(&g_dec, 48000, 8000) && g_dec.factor == 6 && g_dec.taps == 289);
    CHECK(decimator_init(&g_dec, 16000, 8000) && g_dec.factor == 2);
    CHECK(decimator_init(&g_dec, 192000, 8000) && g_dec.factor == 24 && g_dec.taps == DECIM_MAX_TAPS);
    CHECK(decimator_init(&g_dec, 8000, 8000) && g_dec.factor == 1 && g_dec.taps == 1);
    CHECK(decimator_init(&g_dec, 48000, 16000) && g_dec.factor == 3 && g_dec.taps == 145);
    CHECK(decimator_init(&g_dec, 48000, 48000) && g_dec.factor == 1 && g_dec.taps == 1);
    CHECK(!decimator_init(&g_dec, 44100, 8000) && g_dec.factor == 0);
    CHECK(!decimator_init(&g_dec, 48000, 32000)); /* 1.5x isn't a whole factor */
    CHECK(!decimator_init(&g_dec, 16000, 48000)); /* no upsampling */
    CHECK(!decimator_init(&g_dec, 0, 8000));
    CHECK(!decimator_init(&g_dec, 4000, 8000));
    CHECK(!decimator_init(&g_dec, 200000, 8000));
}

static void test_decimator_output_rate(void) {
    unsigned n;
    unsigned outputs = 0;
    float out;

    CHECK(decimator_init(&g_dec, 48000, 8000));
    for (n = 0; n < 48000; n++) {
        if (decimator_push(&g_dec, 0.f, &out)) {
            outputs++;
        }
    }
    CHECK(outputs == 8000);

    memset(&g_dec, 0, sizeof g_dec);
    CHECK(!decimator_push(&g_dec, 1.f, &out)); /* unconfigured: no output */
}

static void test_decimator_dc_and_reset(void) {
    unsigned n;
    float out  = 0.f;
    float last = 0.f;

    CHECK(decimator_init(&g_dec, 48000, 8000));
    for (n = 0; n < 4800; n++) {
        if (decimator_push(&g_dec, 0.25f, &out)) {
            last = out;
        }
    }
    CHECK(fabsf(last - 0.25f) < 1e-4f);

    decimator_reset(&g_dec);
    for (n = 0; n < 6; n++) {
        if (decimator_push(&g_dec, 0.f, &out)) {
            last = out;
        }
    }
    CHECK(fabsf(last) < 1e-9f);
}

/* AUDIT.md M4. The old 6-sample average passed 5 kHz at about -6 dB, which
 * aliased it to 3 kHz. */
static void test_decimator_response_48k(void) {
    double g1k  = decimator_gain_db(48000, 8000, 1000.0);
    double g3k  = decimator_gain_db(48000, 8000, 3000.0);
    double g34k = decimator_gain_db(48000, 8000, 3400.0);
    double g4k  = decimator_gain_db(48000, 8000, 4000.0);
    double g44k = decimator_gain_db(48000, 8000, 4400.0);
    double g5k  = decimator_gain_db(48000, 8000, 5000.0);
    double g7k  = decimator_gain_db(48000, 8000, 7000.0);
    double g12k = decimator_gain_db(48000, 8000, 12000.0);
    double g20k = decimator_gain_db(48000, 8000, 20000.0);

    printf("  48 kHz: 1k %.2f dB, 3k %.2f, 5k %.1f, 7k %.1f, 12k %.1f, 20k %.1f\n", g1k, g3k, g5k, g7k, g12k, g20k);
    printf("  48 kHz: 3.4k %.2f dB, 4k %.1f, 4.4k %.1f\n", g34k, g4k, g44k);
    CHECK(fabs(g1k) < 0.1);
    CHECK(fabs(g3k) < 0.5);
    CHECK(fabs(g34k) < 0.5); /* the telephone band is intact */
    CHECK(g4k < -30.0);      /* at Nyquist, where anything left folds onto itself */
    CHECK(g44k < -60.0);     /* would fold to 3.6 kHz */
    CHECK(g5k < -60.0);
    CHECK(g7k < -60.0);
    CHECK(g12k < -60.0);
    CHECK(g20k < -60.0);
}

static void test_decimator_response_16k(void) {
    double g1k = decimator_gain_db(16000, 8000, 1000.0);
    double g5k = decimator_gain_db(16000, 8000, 5000.0);
    double g7k = decimator_gain_db(16000, 8000, 7000.0);

    printf("  16 kHz: 1k %.2f dB, 5k %.1f, 7k %.1f\n", g1k, g5k, g7k);
    CHECK(fabs(g1k) < 0.1);
    CHECK(g5k < -60.0);
    CHECK(g7k < -60.0);
}

/* L16 16 kHz from a 48 kHz capture: flat to about 6.8 kHz, and what would
 * fold back from above 8 kHz is gone. */
static void test_decimator_response_48k_to_16k(void) {
    double g1k  = decimator_gain_db(48000, 16000, 1000.0);
    double g6k  = decimator_gain_db(48000, 16000, 6000.0);
    double g10k = decimator_gain_db(48000, 16000, 10000.0);
    double g15k = decimator_gain_db(48000, 16000, 15000.0);

    printf("  48 -> 16 kHz: 1k %.2f dB, 6k %.2f, 10k %.1f, 15k %.1f\n", g1k, g6k, g10k, g15k);
    CHECK(fabs(g1k) < 0.1);
    CHECK(fabs(g6k) < 0.5);
    CHECK(g10k < -60.0);
    CHECK(g15k < -60.0);
}

/* L16 48 kHz from a 48 kHz capture is untouched. */
static void test_decimator_passthrough(void) {
    float out = 0.f;

    CHECK(decimator_init(&g_dec, 48000, 48000));
    CHECK(decimator_push(&g_dec, 0.375f, &out));
    CHECK(fabsf(out - 0.375f) < 1e-7f);
    CHECK(fabs(decimator_gain_db(48000, 48000, 15000.0)) < 0.01);
}

/* ---------------------------------------------------------------- codecs */

static void test_rtp_format(void) {
    struct rtp_format f;

    CHECK(rtp_format_make(&f, CODEC_PCMA, 48000, 100)); /* rate and PT ignored */
    CHECK(f.codec == CODEC_PCMA && f.rate == 8000 && f.payload_type == 8);
    CHECK(f.packet_ms == 20 && f.packet_samples == 160);

    CHECK(rtp_format_make(&f, CODEC_L16, 48000, 96));
    CHECK(f.codec == CODEC_L16 && f.rate == 48000 && f.payload_type == 96);
    CHECK(f.packet_ms == 10 && f.packet_samples == 480);
    CHECK(f.packet_samples * 2 <= RTP_MAX_PAYLOAD); /* 960 bytes: no IP fragmentation */

    CHECK(rtp_format_make(&f, CODEC_L16, 16000, 127));
    CHECK(f.rate == 16000 && f.packet_ms == 20 && f.packet_samples == 320 && f.payload_type == 127);

    CHECK(!rtp_format_make(&f, CODEC_L16, 44100, 96));
    CHECK(f.codec == CODEC_PCMA); /* left as PCMA */
    CHECK(!rtp_format_make(&f, CODEC_L16, 32000, 96));
    CHECK(!rtp_format_make(&f, CODEC_L16, 48000, 95));
    CHECK(!rtp_format_make(&f, CODEC_L16, 48000, 128));
    CHECK(!rtp_format_make(&f, CODEC_L16, 48000, 11));
}

static void test_rtp_encode(void) {
    struct rtp_format f;
    const int16_t pcm[4] = {0, 1, -1, -32768};
    uint8_t out[8];

    CHECK(rtp_format_make(&f, CODEC_L16, 48000, 96));
    CHECK(rtp_encode(&f, pcm, 4, out) == 8);
    CHECK(out[0] == 0x00 && out[1] == 0x00);
    CHECK(out[2] == 0x00 && out[3] == 0x01); /* big-endian */
    CHECK(out[4] == 0xFF && out[5] == 0xFF);
    CHECK(out[6] == 0x80 && out[7] == 0x00);

    CHECK(rtp_format_make(&f, CODEC_PCMA, 0, 0));
    CHECK(rtp_encode(&f, pcm, 4, out) == 4);
    CHECK(out[0] == linear_to_alaw(0) && out[3] == linear_to_alaw(-32768));
}

static void test_rtp_set_format(void) {
    struct rtp_tx tx;
    uint8_t h[RTP_HEADER_LEN];

    rtp_tx_init(&tx, 10, 5000, 1);
    rtp_tx_advance(&tx, 160);
    rtp_tx_stop(&tx, 1000);
    rtp_tx_set_format(&tx, 96, 48000, 0xCAFEF00D);
    rtp_tx_header(&tx, h);
    CHECK(h[1] == (0x80 | 96)); /* marker and the new payload type */
    CHECK(h[8] == 0xCA && h[9] == 0xFE && h[10] == 0xF0 && h[11] == 0x0D);

    /* The silent-gap jump uses the new clock: 1 s at 48 kHz. */
    rtp_tx_advance(&tx, 480);
    rtp_tx_stop(&tx, 2000);
    rtp_tx_talkspurt(&tx, 3000, 0);
    CHECK(tx.timestamp == 5000u + 160u + 480u + 48000u);
}

int main(void) {
    test_alaw_known_values();
    test_alaw_codes_round_trip();
    test_alaw_all_inputs();
    test_valid_ipv4();
    test_valid_node_name();
    test_percent_to_db();
    test_gate_disabled_or_no_destination();
    test_gate_level_opens_after_activation_delay();
    test_gate_level_at_threshold_counts_as_hot();
    test_gate_dip_restarts_activation_delay();
    test_gate_hysteresis_and_release_hold();
    test_gate_sound_during_hold_cancels_it();
    test_gate_release_floor();
    test_gate_max_on_locks_out_until_idle();
    test_gate_max_on_needs_true_idle();
    test_gate_max_on_disabled_by_zero();
    test_gate_force_on();
    test_gate_force_on_clears_max_on_lockout();
    test_gate_test_mode();
    test_gate_no_capture();
    test_gate_port_trigger();
    test_gate_stop_helper();
    test_gate_disable_forgets_pending();
    test_rtp_header_layout();
    test_rtp_sequence_and_timestamp_wrap();
    test_rtp_timestamp_tracks_silence();
    test_clock_samples_due();
    test_decimator_rates();
    test_decimator_output_rate();
    test_decimator_dc_and_reset();
    test_decimator_response_48k();
    test_decimator_response_16k();
    test_decimator_response_48k_to_16k();
    test_decimator_passthrough();
    test_rtp_format();
    test_rtp_encode();
    test_rtp_set_format();

    if (g_failures != 0) {
        fprintf(stderr, "FAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("OK: %d checks\n", g_checks);
    return 0;
}
