/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 *
 * The manager thread: looks at the device over local VAPIX to tell whether it
 * belongs to an AXIS Audio Manager Edge site or runs the AXIS Audio Manager
 * Pro client app, and in Edge mode keeps the rule in place that turns the
 * app's event into a Line-in page. All of it is blocking HTTP and D-Bus, so
 * it never runs on the audio loop.
 */
#ifndef MANAGER_H
#define MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "mgrcore.h"

struct mgr_snapshot {
    enum mgr_mode mode;     /* the setting this snapshot was made for */
    enum mgr_active active; /* what the app should be doing */
    struct mgr_detect detect;
    char site_name[64]; /* the Edge site's name, when known */
    char pro_app[48];   /* package name of the Pro client app, if installed */
    bool pro_installed; /* installed, running or not */
    char pro_server[64]; /* the Pro server that app is set to connect to, or "" */
    /* Edge mode only. -1 means not known. */
    int line_in;       /* 1: Edge has the line input as a paging source */
    int source_active; /* 1: Edge reports that source active now */
    int64_t source_checked_ms;
    enum edge_rule_state rule;
    bool rule_created; /* the app added the rule during this run */
    int other_rules;
    char other_rule_names[160];
    char error[160]; /* why the last look at the device failed, or "" */
};

/* Starts the thread. Returns false if it couldn't be started; the app then
 * behaves as in Pro mode. */
bool mgr_start(enum mgr_mode mode);
void mgr_stop(void);

/* A changed setting takes effect at once and triggers a fresh look. */
void mgr_set_mode(enum mgr_mode mode);

/* Asks for a full look at the device now instead of at the next 15 s pass. */
void mgr_look_now(void);

/* Asks for an early check of whether Edge is paging, delay_ms from now. */
void mgr_check_source(int delay_ms);

/* Latest snapshot. Cheap: copies under a mutex. */
void mgr_get(struct mgr_snapshot* out);

#endif
