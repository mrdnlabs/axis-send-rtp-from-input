/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 *
 * Pure logic shared by the ACAP and the host unit tests: the G.711 A-law
 * encoder, input validation and the gate state machine. Nothing here touches
 * PipeWire, AXIS SDK libraries, sockets, clocks or globals, so it builds and
 * runs on any host with a C compiler.
 */
#ifndef RTPCORE_H
#define RTPCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAME_MAX_LEN 63
#define FLOOR_DB -90 /* lowest threshold; silence on the C8110 sits near -90 dBFS */

/* ITU-T G.711 A-law, 16-bit linear PCM to 8-bit. */
uint8_t linear_to_alaw(int16_t pcm);

/* Canonical dotted IPv4, and a unicast target. */
bool valid_ipv4(const char* text);

/* PipeWire node name: 1..NAME_MAX_LEN of [A-Za-z0-9._-]. */
bool valid_node_name(const char* text);

/* Linear peak 0-100 to dBFS. Zero and below map to -120 dBFS. */
double percent_to_db(double percent);

/* The trigger gate. Times are milliseconds on a monotonic clock; 0 means "not set". */
struct gate {
    bool streaming;
    bool waiting_for_clear; /* max-on tripped; stays closed until the trigger goes idle */
    int64_t hot_since;
    int64_t quiet_since;
    int64_t active_since;
};

struct gate_input {
    bool enabled;
    bool have_destination;
    bool test_mode;
    bool force_on;
    bool format_ready;
    int trigger_port;     /* 0 = audio level, 1 or 2 = I/O port */
    int port_active;      /* for a port trigger: 1 active, 0 inactive, -1 unknown */
    double level_percent; /* linear peak 0-100 */
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
};

enum gate_action { GATE_NONE, GATE_START, GATE_STOP };

struct gate_result {
    enum gate_action action;
    const char* reason; /* set for GATE_START and GATE_STOP */
    bool rearmed;       /* the max-on lockout cleared because the trigger went idle */
};

/* Advance the gate one tick. The gate's own fields are already updated when
 * this returns; the caller only carries out the side effects of the action. */
struct gate_result gate_step(struct gate* g, const struct gate_input* in, int64_t now);

/* Open the gate now. */
void gate_start(struct gate* g, int64_t now);

/* Close the gate. Returns true if it was open. */
bool gate_stop(struct gate* g);

/* Name shown in /api/status: waiting, holding, test, active, pending or inactive. */
const char* gate_state(const struct gate* g, bool test_mode);

/* ---------------------------------------------------------------- RTP sender */

#define RTP_HEADER_LEN 12
#define RTP_PT_PCMA 8
#define RTP_CLOCK_HZ 8000 /* G.711 */
#define RTP_MAX_PAYLOAD 1200 /* stays inside a 1500-byte Ethernet MTU with IP and UDP headers */

/* What goes on the wire. G.711 A-law is 8 kHz telephone audio. L16 is
 * uncompressed 16-bit big-endian PCM (RFC 3551 4.5.11), mono, at 16 or
 * 48 kHz; AXIS Audio Manager Pro accepts both as an RTP source. */
enum rtp_codec { CODEC_PCMA, CODEC_L16 };

struct rtp_format {
    enum rtp_codec codec;
    unsigned rate;           /* sample rate and RTP clock, Hz */
    unsigned packet_samples; /* samples per packet */
    unsigned packet_ms;
    uint8_t payload_type;
};

/* PCMA ignores rate and payload_type (always 8 kHz, PT 8, 20 ms). L16 takes
 * rate 16000 or 48000 and a dynamic payload type 96-127; 48 kHz uses 10 ms
 * packets so each stays under RTP_MAX_PAYLOAD. Returns false for anything
 * else, leaving *f as PCMA. */
bool rtp_format_make(struct rtp_format* f, enum rtp_codec codec, unsigned rate, int payload_type);

/* Encodes n samples for the format into out (at least n * 2 bytes). Returns
 * the payload length in bytes. */
size_t rtp_encode(const struct rtp_format* f, const int16_t* pcm, unsigned n, uint8_t* out);

/* RTP header state for one SSRC. The timestamp follows the 8 kHz sampling
 * clock through silence (RFC 3550 5.1, RFC 3551 4.1), and sequence numbers and
 * timestamps advance for every packet, sent or not, so a failed send reads as
 * packet loss at the receiver. */
struct rtp_tx {
    uint16_t seq;
    uint32_t timestamp; /* timestamp of the next packet */
    uint32_t ssrc;
    uint8_t payload_type;
    unsigned clock_hz;
    bool marker;
    bool have_stop;
    int64_t stop_ms;
};

/* Starts as G.711: payload type 8, 8 kHz clock. */
void rtp_tx_init(struct rtp_tx* tx, uint16_t seq, uint32_t timestamp, uint32_t ssrc);

/* Switches payload type and clock. A new format is a new stream to the
 * receiver, so it also takes a new SSRC and starts a talkspurt. */
void rtp_tx_set_format(struct rtp_tx* tx, uint8_t payload_type, unsigned clock_hz, uint32_t ssrc);

/* A talkspurt starts at now_ms with `buffered` samples of pre-roll already
 * captured. Sets the marker and moves the timestamp over the silent gap. */
void rtp_tx_talkspurt(struct rtp_tx* tx, int64_t now_ms, unsigned buffered);

/* The talkspurt ended at now_ms. */
void rtp_tx_stop(struct rtp_tx* tx, int64_t now_ms);

/* Writes the 12-byte header for the next packet. */
void rtp_tx_header(const struct rtp_tx* tx, uint8_t out[RTP_HEADER_LEN]);

/* Call once per packet after the send attempt, whatever its result. */
void rtp_tx_advance(struct rtp_tx* tx, unsigned samples);

/* Samples a clock of `rate` Hz has produced between start_ms and now_ms,
 * less the `produced` already generated. Never negative. */
uint64_t clock_samples_due(int64_t start_ms, int64_t now_ms, unsigned rate, uint64_t produced);

/* ---------------------------------------------------------------- decimator */

#define DECIM_MAX_FACTOR 24 /* e.g. 192 kHz to 8 kHz */
#define DECIM_CUTOFF 0.4625 /* fraction of the output rate */
#define DECIM_TAPS_PER_FACTOR 48
#define DECIM_MAX_TAPS (DECIM_MAX_FACTOR * DECIM_TAPS_PER_FACTOR + 1)

/* Windowed-sinc low-pass FIR (Blackman) followed by integer decimation. The
 * cutoff sits at 46.25 % of the output rate, below its Nyquist frequency, so
 * the transition band ends near Nyquist instead of straddling it. At 8 kHz
 * out: flat to 3.4 kHz (-0.2 dB), -33 dB at 4 kHz, -78 dB from 4.2 kHz. It
 * replaced a 6-sample average, which barely attenuated 4-8 kHz and folded it
 * back into the speech band. */
struct decimator {
    int factor; /* 0 = not configured */
    int taps;
    int pos;
    int phase;
    float coef[DECIM_MAX_TAPS];
    float hist[DECIM_MAX_TAPS];
};

/* Returns false (and leaves the decimator unconfigured) unless in_rate is
 * out_rate times 1 to DECIM_MAX_FACTOR, and out_rate is at least 8 kHz. Equal
 * rates pass samples straight through. */
bool decimator_init(struct decimator* d, unsigned in_rate, unsigned out_rate);

/* Clears the filter history, keeping the coefficients. */
void decimator_reset(struct decimator* d);

/* Feeds one input sample. Returns true and sets *out when an output sample
 * is ready. */
bool decimator_push(struct decimator* d, float in, float* out);

#endif
