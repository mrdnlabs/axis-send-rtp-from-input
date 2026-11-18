/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 */
#include "rtpcore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

uint8_t linear_to_alaw(int16_t pcm) {
    static const int16_t seg_end[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int mask;
    int seg;
    int aval;
    int value;
    int i;

    value = pcm >> 3;
    if (value >= 0) {
        mask = 0xD5;
    } else {
        mask  = 0x55;
        value = -value - 1;
        if (value < 0) {
            value = 0;
        }
    }
    seg = 8;
    for (i = 0; i < 8; i++) {
        if (value <= seg_end[i]) {
            seg = i;
            break;
        }
    }
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    aval = seg << 4;
    if (seg < 2) {
        aval |= (value >> 1) & 0x0F;
    } else {
        aval |= (value >> seg) & 0x0F;
    }
    return (uint8_t)(aval ^ mask);
}

/* Rejects leading zeros, 0.0.0.0/8, loopback 127.0.0.0/8 (it would send to
 * the camera itself), multicast 224.0.0.0/4, and the reserved 240.0.0.0/4
 * block, which includes 255.255.255.255. A subnet-directed broadcast such as
 * x.x.x.255 can't be told apart from a host without the netmask, so it is
 * allowed. */
bool valid_ipv4(const char* text) {
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    char tail;
    char canonical[16];

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255 || a == 0 || a == 127 || a >= 224) {
        return false;
    }
    snprintf(canonical, sizeof canonical, "%u.%u.%u.%u", a, b, c, d);
    return strcmp(text, canonical) == 0;
}

bool valid_node_name(const char* text) {
    size_t i;
    size_t n;

    if (text == NULL) {
        return false;
    }
    n = strlen(text);
    if (n == 0 || n > NAME_MAX_LEN) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char ch = text[i];
        bool ok  = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

double percent_to_db(double percent) {
    return percent > 0.0 ? 20.0 * log10(percent / 100.0) : -120.0;
}

void gate_start(struct gate* g, int64_t now) {
    g->streaming    = true;
    g->quiet_since  = 0;
    g->active_since = now;
}

bool gate_stop(struct gate* g) {
    if (!g->streaming) {
        return false;
    }
    g->streaming   = false;
    g->hot_since   = 0;
    g->quiet_since = 0;
    return true;
}

static struct gate_result result(enum gate_action action, const char* reason) {
    struct gate_result r;

    r.action  = action;
    r.reason  = reason;
    r.rearmed = false;
    return r;
}

static struct gate_result stop(struct gate* g, const char* reason) {
    return gate_stop(g) ? result(GATE_STOP, reason) : result(GATE_NONE, NULL);
}

/* Stop for a reason that also abandons a pending activation, so the page
 * doesn't show "pending" while disabled and re-enabling waits the full delay. */
static struct gate_result stop_and_forget(struct gate* g, const char* reason) {
    g->hot_since = 0;
    return stop(g, reason);
}

static struct gate_result start(struct gate* g, int64_t now, const char* reason) {
    if (g->streaming) {
        return result(GATE_NONE, NULL);
    }
    gate_start(g, now);
    return result(GATE_START, reason);
}

struct gate_result gate_step(struct gate* g, const struct gate_input* in, int64_t now) {
    bool hot  = false;
    bool idle = false;

    if (!in->enabled || !in->have_destination) {
        g->waiting_for_clear = false;
        return stop_and_forget(g, in->enabled ? "no destination" : "disabled");
    }
    if (in->test_mode) {
        g->waiting_for_clear = false;
        return start(g, now, "test tone");
    }
    if (!in->format_ready) {
        return stop_and_forget(g, "no capture");
    }
    if (in->force_on) {
        g->waiting_for_clear = false;
        return start(g, now, "force on");
    }
    if (in->trigger_port > 0) {
        if (in->port_active < 0) {
            return stop_and_forget(g, "input state unknown");
        }
        hot  = in->port_active == 1;
        idle = !hot;
    } else {
        /* Threshold and release are dBFS peaks. The release level never goes
         * below the -90 dBFS floor, so a large margin still lets the gate close. */
        double peak_db    = percent_to_db(in->level_percent);
        double release_db = (double)(in->threshold_db - in->margin_db);
        if (release_db < (double)FLOOR_DB) {
            release_db = (double)FLOOR_DB;
        }
        hot  = peak_db >= (double)in->threshold_db;
        idle = peak_db < release_db;
    }
    if (g->waiting_for_clear) {
        struct gate_result r = stop(g, "max on time");
        if (idle) {
            g->waiting_for_clear = false;
            r.rearmed            = true;
        }
        return r;
    }
    if (!g->streaming) {
        if (hot) {
            if (g->hot_since == 0) {
                g->hot_since = now;
            }
            if (now - g->hot_since >= in->activation_delay_ms) {
                return start(g, now, in->trigger_port > 0 ? "input" : "level");
            }
        } else {
            g->hot_since = 0;
        }
        return result(GATE_NONE, NULL);
    }
    if (in->max_on_s > 0 && now - g->active_since >= (int64_t)in->max_on_s * 1000) {
        g->waiting_for_clear = true;
        return stop(g, "max on time");
    }
    if (idle) {
        if (g->quiet_since == 0) {
            g->quiet_since = now;
        }
        if (now - g->quiet_since >= (int64_t)in->deactivation_delay_s * 1000) {
            return stop(g, "quiet");
        }
    } else {
        g->quiet_since = 0;
    }
    return result(GATE_NONE, NULL);
}

const char* gate_state(const struct gate* g, bool test_mode) {
    if (g->waiting_for_clear) {
        return "waiting";
    }
    if (g->streaming && g->quiet_since != 0) {
        return "holding";
    }
    if (g->streaming && test_mode) {
        return "test";
    }
    if (g->streaming) {
        return "active";
    }
    if (g->hot_since != 0) {
        return "pending";
    }
    return "inactive";
}

/* ---------------------------------------------------------------- RTP sender */

void rtp_tx_init(struct rtp_tx* tx, uint16_t seq, uint32_t timestamp, uint32_t ssrc) {
    memset(tx, 0, sizeof *tx);
    tx->seq          = seq;
    tx->timestamp    = timestamp;
    tx->ssrc         = ssrc;
    tx->payload_type = RTP_PT_PCMA;
    tx->clock_hz     = RTP_CLOCK_HZ;
    tx->marker       = true;
}

void rtp_tx_set_format(struct rtp_tx* tx, uint8_t payload_type, unsigned clock_hz, uint32_t ssrc) {
    tx->payload_type = payload_type & 0x7F;
    tx->clock_hz     = clock_hz;
    tx->ssrc         = ssrc;
    tx->marker       = true;
    tx->have_stop    = false;
}

bool rtp_format_make(struct rtp_format* f, enum rtp_codec codec, unsigned rate, int payload_type) {
    f->codec          = CODEC_PCMA;
    f->rate           = RTP_CLOCK_HZ;
    f->packet_ms      = 20;
    f->packet_samples = RTP_CLOCK_HZ / 50;
    f->payload_type   = RTP_PT_PCMA;
    if (codec == CODEC_PCMA) {
        return true;
    }
    if (codec != CODEC_L16 || (rate != 16000 && rate != 48000) || payload_type < 96 || payload_type > 127) {
        return false;
    }
    f->codec          = CODEC_L16;
    f->rate           = rate;
    f->packet_ms      = rate == 48000 ? 10 : 20;
    f->packet_samples = rate / 1000 * f->packet_ms;
    f->payload_type   = (uint8_t)payload_type;
    return true;
}

size_t rtp_encode(const struct rtp_format* f, const int16_t* pcm, unsigned n, uint8_t* out) {
    unsigned i;

    if (f->codec == CODEC_L16) {
        for (i = 0; i < n; i++) {
            uint16_t v     = (uint16_t)pcm[i];
            out[i * 2]     = (uint8_t)(v >> 8); /* network byte order */
            out[i * 2 + 1] = (uint8_t)(v & 0xFF);
        }
        return (size_t)n * 2U;
    }
    for (i = 0; i < n; i++) {
        out[i] = linear_to_alaw(pcm[i]);
    }
    return n;
}

void rtp_tx_talkspurt(struct rtp_tx* tx, int64_t now_ms, unsigned buffered) {
    tx->marker = true;
    if (tx->have_stop && now_ms > tx->stop_ms) {
        /* The pre-roll was sampled before now, so its first sample sits
         * `buffered` samples before the gap's end. */
        int64_t gap = (now_ms - tx->stop_ms) * (int64_t)tx->clock_hz / 1000 - (int64_t)buffered;
        if (gap > 0) {
            tx->timestamp += (uint32_t)gap; /* wraps modulo 2^32, as RTP expects */
        }
    }
    tx->have_stop = false;
}

void rtp_tx_stop(struct rtp_tx* tx, int64_t now_ms) {
    tx->have_stop = true;
    tx->stop_ms   = now_ms;
    tx->marker    = true;
}

void rtp_tx_header(const struct rtp_tx* tx, uint8_t out[RTP_HEADER_LEN]) {
    out[0]  = 0x80; /* version 2, no padding, extension or CSRCs */
    out[1]  = (uint8_t)((tx->marker ? 0x80 : 0x00) | (tx->payload_type & 0x7F));
    out[2]  = (uint8_t)(tx->seq >> 8);
    out[3]  = (uint8_t)(tx->seq & 0xFF);
    out[4]  = (uint8_t)(tx->timestamp >> 24);
    out[5]  = (uint8_t)(tx->timestamp >> 16);
    out[6]  = (uint8_t)(tx->timestamp >> 8);
    out[7]  = (uint8_t)(tx->timestamp & 0xFF);
    out[8]  = (uint8_t)(tx->ssrc >> 24);
    out[9]  = (uint8_t)(tx->ssrc >> 16);
    out[10] = (uint8_t)(tx->ssrc >> 8);
    out[11] = (uint8_t)(tx->ssrc & 0xFF);
}

void rtp_tx_advance(struct rtp_tx* tx, unsigned samples) {
    tx->seq++;
    tx->timestamp += samples;
    tx->marker = false;
}

uint64_t clock_samples_due(int64_t start_ms, int64_t now_ms, unsigned rate, uint64_t produced) {
    uint64_t total;

    if (now_ms <= start_ms) {
        return 0;
    }
    total = (uint64_t)(now_ms - start_ms) * rate / 1000U;
    return total > produced ? total - produced : 0;
}

/* ---------------------------------------------------------------- decimator */

bool decimator_init(struct decimator* d, unsigned in_rate, unsigned out_rate) {
    const double pi = 3.14159265358979323846;
    double fc;
    double sum = 0.0;
    int middle;
    int n;

    memset(d, 0, sizeof *d);
    if (out_rate < 8000 || in_rate < out_rate || in_rate % out_rate != 0 || in_rate / out_rate > DECIM_MAX_FACTOR) {
        return false;
    }
    d->factor = (int)(in_rate / out_rate);
    if (d->factor == 1) {
        d->taps    = 1;
        d->coef[0] = 1.f;
        return true;
    }
    d->taps = d->factor * DECIM_TAPS_PER_FACTOR + 1;
    middle  = (d->taps - 1) / 2;
    fc      = (double)out_rate * DECIM_CUTOFF / (double)in_rate; /* cycles per input sample */
    for (n = 0; n < d->taps; n++) {
        int x       = n - middle;
        double sinc = x == 0 ? 2.0 * fc : sin(2.0 * pi * fc * x) / (pi * x);
        double t    = (double)n / (double)(d->taps - 1);
        double w    = 0.42 - 0.5 * cos(2.0 * pi * t) + 0.08 * cos(4.0 * pi * t);
        d->coef[n]  = (float)(sinc * w);
        sum += sinc * w;
    }
    for (n = 0; n < d->taps; n++) {
        d->coef[n] = (float)(d->coef[n] / sum); /* unity gain at DC */
    }
    return true;
}

void decimator_reset(struct decimator* d) {
    memset(d->hist, 0, sizeof d->hist);
    d->pos   = 0;
    d->phase = 0;
}

bool decimator_push(struct decimator* d, float in, float* out) {
    float acc = 0.f;
    int idx;
    int k;

    if (d->factor <= 0) {
        return false;
    }
    d->hist[d->pos] = in;
    d->pos          = d->pos + 1 == d->taps ? 0 : d->pos + 1;
    if (++d->phase < d->factor) {
        return false;
    }
    d->phase = 0;
    idx      = d->pos; /* oldest sample; the filter is symmetric */
    for (k = 0; k < d->taps; k++) {
        acc += d->coef[k] * d->hist[idx];
        idx = idx + 1 == d->taps ? 0 : idx + 1;
    }
    *out = acc;
    return true;
}
