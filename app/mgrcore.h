/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 *
 * Pure logic for choosing between the two ways the app can deliver a page:
 * RTP to AXIS Audio Manager Pro, or switching the Line-in paging recipient of
 * an AXIS Audio Manager Edge site. Also the small XML readers for the VAPIX
 * replies that decision needs. Nothing here touches the network, D-Bus or
 * globals, so it builds and runs on any host with a C compiler.
 */
#ifndef MGRCORE_H
#define MGRCORE_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------ mode choice */

/* The "Audio manager" setting. */
enum mgr_mode { MGR_AUTO, MGR_PRO, MGR_EDGE };

/* What the app is doing now. UNKNOWN only while Auto waits for the first
 * look at the device; the gate stays closed until it is settled. */
enum mgr_active { ACTIVE_UNKNOWN, ACTIVE_PRO, ACTIVE_EDGE };

struct mgr_detect {
    bool known;     /* a detection pass has finished (or given up) */
    bool edge_site; /* this device is a member of an Audio Manager Edge site */
    bool pro_app;   /* the Audio Manager Pro client app is running on this device */
};

/* "auto", "pro" or "edge". */
const char* mgr_mode_name(enum mgr_mode mode);
bool mgr_mode_parse(const char* text, enum mgr_mode* out);

/* "unknown", "pro" or "edge". */
const char* mgr_active_name(enum mgr_active active);

/* Pro and Edge are forced by the setting. Auto follows the device: a running
 * Pro client app means Pro, even inside an Edge site, because Pro then owns
 * the device's audio. Otherwise membership of an Edge site means Edge.
 * With neither, RTP (Pro) is the fallback, as before 1.8.0. */
enum mgr_active mgr_resolve(enum mgr_mode mode, const struct mgr_detect* detect);

/* ------------------------------------------------------------ XML reading */

/* One element found by xml_next(). The spans point into the caller's text. */
struct xml_el {
    const char* tag; /* inside the opening tag, after the name: attributes */
    size_t tag_len;
    const char* body; /* between the opening and closing tags; empty for <x/> */
    size_t body_len;
};

/* Finds the next element whose local name is `local`, whatever its namespace
 * prefix, at or after *cursor and before end. On success fills *el and moves
 * *cursor past the element. Elements of the same name are assumed not to
 * nest, which holds for the VAPIX replies read here. */
bool xml_next(const char** cursor, const char* end, const char* local, struct xml_el* el);

/* Text of the first `local` element inside parent, entities decoded and
 * surrounding whitespace removed. Returns false and an empty string if there
 * is none. Text that doesn't fit is cut. */
bool xml_child_text(const struct xml_el* parent, const char* local, char* out, size_t out_len);

/* Value of attribute `name` in the element's opening tag, entities decoded. */
bool xml_attr(const struct xml_el* el, const char* name, char* out, size_t out_len);

/* ---------------------------------------------------- installed apps list */

/* Looks in the reply of /axis-cgi/applications/list.cgi for the client app
 * that AXIS Audio Manager Pro installs on the devices it manages. In Pro
 * 5.1.44 its package name is "AudioManagerPro", or "AudioManagerProB" for the
 * second variant the server ships; Pro 3's was "netspeaker". Any package
 * whose name contains "audiomanagerpro", or is "netspeaker", counts, ignoring
 * case. Fills in the package name found and whether it is running. */
bool app_list_find_pro(const char* xml, char* name, size_t name_len, bool* running);

/* Reads one value out of a param.cgi "list" reply: the text after the first
 * "=" on the first line, without the line ending. False if there is none,
 * as in "# Error: ..." replies. */
bool param_value(const char* reply, char* out, size_t out_len);

/* ------------------------------------------------------------- Edge rules */

/* The event the app raises while its gate is open in Edge mode, and the rule
 * that turns it into an Edge Line-in page. */
#define EDGE_EVENT_TOPIC1 "LineInputRTP"
#define EDGE_EVENT_TOPIC2 "EdgePaging"
#define EDGE_EVENT_TOPIC "tnsaxis:CameraApplicationPlatform/" EDGE_EVENT_TOPIC1 "/" EDGE_EVENT_TOPIC2
#define EDGE_EVENT_KEY "active"
#define EDGE_RULE_NAME "Line Input RTP: Edge paging"
#define EDGE_ACTION_NAME "Line Input RTP: activate Line-in"
#define EDGE_ACTION_TEMPLATE "com.axis.action.unlimited.audiosite.activate_source"
/* The C8110's line input as Audio Manager Edge names it. */
#define EDGE_SOURCE "HardwareAudioInput0"

enum edge_rule_state {
    RULE_UNKNOWN, /* not looked at yet */
    RULE_MISSING, /* no rule with the app's name */
    RULE_OK,      /* present, enabled and wired as the app made it */
    RULE_CHANGED  /* present, but disabled or edited: the app leaves it alone */
};

struct edge_rules {
    enum edge_rule_state state;
    int action_id;         /* an existing action the app's rule can use, or -1 */
    int others;            /* other enabled rules that switch the same source */
    char other_names[160]; /* their names, comma-separated, cut with "..." if long */
};

/* Reads the replies of action1 GetActionRules and GetActionConfigurations. */
void edge_rules_scan(const char* rules_xml, const char* actions_xml, struct edge_rules* out);

/* "unknown", "missing", "ok" or "changed". */
const char* edge_rule_state_name(enum edge_rule_state state);

#endif
