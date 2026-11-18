/**
 * Runs gate_step() from app/rtpcore.c over scripted inputs and prints the
 * inputs and the gate state after each history entry as JSON. The settings
 * page's preview must reproduce these states exactly; tests/test_preview.js
 * checks that. Build and run with `make -C tests preview`.
 * SPDX-License-Identifier: MIT
 */
#include "rtpcore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define INTERVAL_MS 20 /* HIST_INTERVAL_MS in lineinrtp.c */
#define TICK_MS 10     /* the app's timer */
#define ENTRIES 3000   /* 60 s per scenario */

struct scenario {
    const char* name;
    int trigger_port;
    int threshold_db;
    int margin_db;
    int activation_delay_ms;
    int deactivation_delay_s;
    int max_on_s;
    bool port_unknowns;
};

static const struct scenario k_scenarios[] = {
    {"level, defaults", 0, -45, 6, 200, 2, 0, false},
    {"level, no delay or hold", 0, -40, 3, 0, 0, 0, false},
    {"level, delay shorter than an entry", 0, -50, 10, 30, 1, 0, false},
    {"level, max on", 0, -45, 6, 100, 3, 4, false},
    {"level, closing level on the floor", 0, -88, 20, 50, 1, 0, false},
    {"level, quiet threshold and long hold", 0, -70, 15, 500, 5, 20, false},
    {"port 1, with unknown state", 1, -45, 6, 200, 2, 0, true},
    {"port 2, max on", 2, -45, 6, 1000, 5, 10, false},
};

static unsigned g_rng = 12345u;

static double rnd(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    return (double)((g_rng >> 8) & 0xFFFFFF) / 16777216.0;
}

static int rnd_int(int lo, int hi) {
    return lo + (int)(rnd() * (double)(hi - lo + 1));
}

static const char* page_state(const char* c_state) {
    /* The page's names for gate_state(): active -> on, inactive -> idle. */
    if (strcmp(c_state, "active") == 0) {
        return "on";
    }
    if (strcmp(c_state, "inactive") == 0) {
        return "idle";
    }
    return c_state;
}

int main(void) {
    static double level[ENTRIES];
    static int p1[ENTRIES];
    static int p2[ENTRIES];
    static const char* states[ENTRIES];
    size_t s;

    printf("{\"intervalMs\":%d,\"scenarios\":[", INTERVAL_MS);
    for (s = 0; s < sizeof k_scenarios / sizeof k_scenarios[0]; s++) {
        const struct scenario* sc = &k_scenarios[s];
        struct gate g;
        struct gate_input in;
        int i;
        int left    = 0;
        bool burst  = false;
        int pleft   = 0;
        int pstate  = 0;
        int pleft2  = 0;
        int pstate2 = 1;

        /* Speech-like level: bursts with syllable dips, then silence near -90. */
        for (i = 0; i < ENTRIES; i++) {
            double db;
            if (left-- <= 0) {
                burst = !burst;
                left  = burst ? rnd_int(15, 200) : rnd_int(10, 300);
            }
            if (burst) {
                db = -32.0 + 14.0 * sin((double)i / 7.0) + 6.0 * rnd();
                if (rnd() < 0.15) {
                    db -= 30.0 * rnd();
                }
            } else {
                db = -90.0 + 1.5 * rnd();
            }
            if (rnd() < 0.01) {
                db = -150.0; /* digital silence: 0 % */
            }
            level[i] = db < -140.0 ? 0.0 : 100.0 * pow(10.0, db / 20.0);
            if (pleft-- <= 0) {
                pstate = sc->port_unknowns && rnd() < 0.2 ? -1 : (pstate == 1 ? 0 : 1);
                pleft  = rnd_int(5, 400);
            }
            if (pleft2-- <= 0) {
                pstate2 = pstate2 == 1 ? 0 : 1;
                pleft2  = rnd_int(5, 900);
            }
            p1[i] = pstate;
            p2[i] = pstate2;
        }

        memset(&g, 0, sizeof g);
        memset(&in, 0, sizeof in);
        in.enabled              = true;
        in.have_destination     = true;
        in.format_ready         = true;
        in.trigger_port         = sc->trigger_port;
        in.threshold_db         = sc->threshold_db;
        in.margin_db            = sc->margin_db;
        in.activation_delay_ms  = sc->activation_delay_ms;
        in.deactivation_delay_s = sc->deactivation_delay_s;
        in.max_on_s             = sc->max_on_s;
        for (i = 0; i < ENTRIES; i++) {
            int k;
            in.level_percent = level[i];
            in.port_active   = sc->trigger_port == 1 ? p1[i] : sc->trigger_port == 2 ? p2[i] : -1;
            for (k = 0; k < INTERVAL_MS / TICK_MS; k++) {
                int64_t now = (int64_t)(i * (INTERVAL_MS / TICK_MS) + k + 1) * TICK_MS;
                (void)gate_step(&g, &in, now);
            }
            states[i] = page_state(gate_state(&g, false));
        }

        printf("%s{\"name\":\"%s\",\"cfg\":{\"mode\":\"auto\",\"ip\":\"192.168.1.50\",\"trigger\":\"%s\","
               "\"level\":%d,\"margin\":%d,\"delay\":%d,\"hold\":%d,\"maxOn\":%d},\"h\":[",
               s == 0 ? "" : ",", sc->name,
               sc->trigger_port == 1 ? "port1" : sc->trigger_port == 2 ? "port2" : "level", sc->threshold_db,
               sc->margin_db, sc->activation_delay_ms, sc->deactivation_delay_s, sc->max_on_s);
        for (i = 0; i < ENTRIES; i++) {
            printf("%s{\"v\":%.17g,\"p1\":%d,\"p2\":%d}", i == 0 ? "" : ",", level[i], p1[i], p2[i]);
        }
        printf("],\"states\":[");
        for (i = 0; i < ENTRIES; i++) {
            printf("%s\"%s\"", i == 0 ? "" : ",", states[i]);
        }
        printf("]}");
    }
    printf("]}\n");
    return 0;
}
