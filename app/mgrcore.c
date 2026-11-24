/**
 * Copyright (c) 2026 mrdnlabs.pro
 * SPDX-License-Identifier: MIT
 */
#include "mgrcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------ mode choice */

const char* mgr_mode_name(enum mgr_mode mode) {
    return mode == MGR_PRO ? "pro" : mode == MGR_EDGE ? "edge" : "auto";
}

bool mgr_mode_parse(const char* text, enum mgr_mode* out) {
    if (text == NULL) {
        return false;
    }
    if (strcmp(text, "auto") == 0) {
        *out = MGR_AUTO;
    } else if (strcmp(text, "pro") == 0) {
        *out = MGR_PRO;
    } else if (strcmp(text, "edge") == 0) {
        *out = MGR_EDGE;
    } else {
        return false;
    }
    return true;
}

const char* mgr_active_name(enum mgr_active active) {
    return active == ACTIVE_PRO ? "pro" : active == ACTIVE_EDGE ? "edge" : "unknown";
}

enum mgr_active mgr_resolve(enum mgr_mode mode, const struct mgr_detect* detect) {
    if (mode == MGR_PRO) {
        return ACTIVE_PRO;
    }
    if (mode == MGR_EDGE) {
        return ACTIVE_EDGE;
    }
    if (detect == NULL || !detect->known) {
        return ACTIVE_UNKNOWN;
    }
    if (detect->pro_app) {
        return ACTIVE_PRO;
    }
    return detect->edge_site ? ACTIVE_EDGE : ACTIVE_PRO;
}

/* ------------------------------------------------------------ XML reading */

static bool name_char_ends(char ch) {
    return ch == '>' || ch == '/' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* At p just after '<' (or "</"): returns the end of the element name if its
 * local part equals `local`, else NULL. */
static const char* match_name(const char* p, const char* end, const char* local) {
    const char* name = p;
    const char* q    = p;
    size_t n         = strlen(local);

    while (q < end && !name_char_ends(*q)) {
        if (*q == ':') {
            name = q + 1;
        }
        q++;
    }
    if (q >= end || (size_t)(q - name) != n || memcmp(name, local, n) != 0) {
        return NULL;
    }
    return q;
}

bool xml_next(const char** cursor, const char* end, const char* local, struct xml_el* el) {
    const char* p = *cursor;

    while (p < end) {
        const char* lt = memchr(p, '<', (size_t)(end - p));
        const char* name_end;
        const char* gt;

        if (lt == NULL || lt + 1 >= end) {
            break;
        }
        p = lt + 1;
        if (*p == '/' || *p == '?' || *p == '!') {
            continue;
        }
        name_end = match_name(p, end, local);
        if (name_end == NULL) {
            continue;
        }
        gt = memchr(name_end, '>', (size_t)(end - name_end));
        if (gt == NULL) {
            break;
        }
        el->tag     = name_end;
        el->tag_len = (size_t)(gt - name_end);
        if (gt > name_end && gt[-1] == '/') {
            el->tag_len--;
            el->body     = gt + 1;
            el->body_len = 0;
            *cursor      = gt + 1;
            return true;
        }
        /* The first closing tag with the same local name ends it. */
        {
            const char* q = gt + 1;
            while (q < end) {
                const char* close = memchr(q, '<', (size_t)(end - q));
                const char* close_end;

                if (close == NULL || close + 2 >= end) {
                    break;
                }
                q = close + 1;
                if (*q != '/') {
                    continue;
                }
                close_end = match_name(q + 1, end, local);
                if (close_end == NULL || *close_end != '>') {
                    continue;
                }
                el->body     = gt + 1;
                el->body_len = (size_t)(close - (gt + 1));
                *cursor      = close_end + 1;
                return true;
            }
        }
        break; /* unterminated */
    }
    *cursor = end;
    return false;
}

/* Copies [src, src+len) to out with the five XML entities and numeric
 * references below 128 decoded, and surrounding whitespace removed. */
static void copy_text(const char* src, size_t len, char* out, size_t out_len) {
    static const struct {
        const char* name;
        char ch;
    } entities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    size_t used = 0;
    size_t i    = 0;

    if (out_len == 0) {
        return;
    }
    while (len > 0 && (src[0] == ' ' || src[0] == '\t' || src[0] == '\r' || src[0] == '\n')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }
    while (i < len && used + 1 < out_len) {
        char ch = src[i];

        if (ch == '&') {
            size_t e;
            bool done = false;

            for (e = 0; e < sizeof entities / sizeof entities[0]; e++) {
                size_t n = strlen(entities[e].name);
                if (len - i >= n && memcmp(src + i, entities[e].name, n) == 0) {
                    out[used++] = entities[e].ch;
                    i += n;
                    done = true;
                    break;
                }
            }
            if (done) {
                continue;
            }
            if (len - i >= 4 && src[i + 1] == '#') {
                const char* semi = memchr(src + i, ';', len - i);
                if (semi != NULL && semi - (src + i) <= 8) {
                    bool hex    = src[i + 2] == 'x' || src[i + 2] == 'X';
                    long code   = strtol(src + i + (hex ? 3 : 2), NULL, hex ? 16 : 10);
                    out[used++] = code >= 32 && code < 127 ? (char)code : '?';
                    i           = (size_t)(semi - src) + 1;
                    continue;
                }
            }
        }
        out[used++] = ch;
        i++;
    }
    out[used] = '\0';
}

bool xml_child_text(const struct xml_el* parent, const char* local, char* out, size_t out_len) {
    const char* cursor = parent->body;
    struct xml_el el;

    if (out_len > 0) {
        out[0] = '\0';
    }
    if (!xml_next(&cursor, parent->body + parent->body_len, local, &el)) {
        return false;
    }
    copy_text(el.body, el.body_len, out, out_len);
    return true;
}

bool xml_attr(const struct xml_el* el, const char* name, char* out, size_t out_len) {
    const char* p   = el->tag;
    const char* end = el->tag + el->tag_len;
    size_t n        = strlen(name);

    if (out_len > 0) {
        out[0] = '\0';
    }
    while (p < end) {
        const char* key;
        const char* key_end;
        const char* value;
        const char* value_end;
        char quote;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }
        key = p;
        while (p < end && *p != '=' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            p++;
        }
        key_end = p;
        while (p < end && *p != '=') {
            p++;
        }
        if (p >= end) {
            break;
        }
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p >= end || (*p != '"' && *p != '\'')) {
            break;
        }
        quote     = *p++;
        value     = p;
        value_end = memchr(p, quote, (size_t)(end - p));
        if (value_end == NULL) {
            break;
        }
        p = value_end + 1;
        if ((size_t)(key_end - key) == n && memcmp(key, name, n) == 0) {
            copy_text(value, (size_t)(value_end - value), out, out_len);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------- installed apps list */

static bool contains_nocase(const char* text, const char* needle) {
    size_t n = strlen(needle);

    for (; *text != '\0'; text++) {
        if (strncasecmp(text, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

bool app_list_find_pro(const char* xml, char* name, size_t name_len, bool* running) {
    const char* cursor = xml;
    const char* end;
    struct xml_el el;

    if (name_len > 0) {
        name[0] = '\0';
    }
    if (running != NULL) {
        *running = false;
    }
    if (xml == NULL) {
        return false;
    }
    end = xml + strlen(xml);
    while (xml_next(&cursor, end, "application", &el)) {
        char app[64];
        char status[32];

        if (!xml_attr(&el, "Name", app, sizeof app)) {
            continue;
        }
        if (!contains_nocase(app, "audiomanagerpro") && strcasecmp(app, "netspeaker") != 0) {
            continue;
        }
        snprintf(name, name_len, "%s", app);
        if (running != NULL) {
            *running = xml_attr(&el, "Status", status, sizeof status) && strcasecmp(status, "Running") == 0;
        }
        return true;
    }
    return false;
}

bool param_value(const char* reply, char* out, size_t out_len) {
    const char* eq;
    size_t n;

    if (out_len > 0) {
        out[0] = '\0';
    }
    if (reply == NULL || reply[0] == '#') {
        return false;
    }
    n  = strcspn(reply, "\r\n");
    eq = memchr(reply, '=', n);
    if (eq == NULL || out_len == 0) {
        return false;
    }
    n -= (size_t)(eq + 1 - reply);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, eq + 1, n);
    out[n] = '\0';
    return true;
}

/* ------------------------------------------------------------- Edge rules */

#define MAX_ACTIONS 64

struct action {
    int id;
    bool activates_source; /* any audiosite.activate_source template, for EDGE_SOURCE */
    bool is_ours;          /* the unlimited template, activate=1, for EDGE_SOURCE */
    bool named_ours;
};

const char* edge_rule_state_name(enum edge_rule_state state) {
    switch (state) {
    case RULE_MISSING:
        return "missing";
    case RULE_OK:
        return "ok";
    case RULE_CHANGED:
        return "changed";
    default:
        return "unknown";
    }
}

static unsigned read_actions(const char* xml, struct action* actions, unsigned max) {
    const char* cursor = xml;
    const char* end    = xml + strlen(xml);
    struct xml_el el;
    unsigned count = 0;

    while (count < max && xml_next(&cursor, end, "ActionConfiguration", &el)) {
        char id[16];
        char name[96];
        char token[96];
        char source[64]   = "";
        char activate[16] = "";
        const char* pc    = el.body;
        struct xml_el param;
        struct action* a = &actions[count];

        if (!xml_child_text(&el, "ConfigurationID", id, sizeof id)) {
            continue;
        }
        xml_child_text(&el, "Name", name, sizeof name);
        xml_child_text(&el, "TemplateToken", token, sizeof token);
        while (xml_next(&pc, el.body + el.body_len, "Parameter", &param)) {
            char key[32];
            if (!xml_attr(&param, "Name", key, sizeof key)) {
                continue;
            }
            if (strcmp(key, "source") == 0) {
                xml_attr(&param, "Value", source, sizeof source);
            } else if (strcmp(key, "activate") == 0) {
                xml_attr(&param, "Value", activate, sizeof activate);
            }
        }
        a->id               = atoi(id);
        a->activates_source = strstr(token, "audiosite.activate_source") != NULL && strcmp(source, EDGE_SOURCE) == 0;
        a->is_ours    = a->activates_source && strcmp(token, EDGE_ACTION_TEMPLATE) == 0 && strcmp(activate, "1") == 0;
        a->named_ours = strcmp(name, EDGE_ACTION_NAME) == 0;
        count++;
    }
    return count;
}

static const struct action* find_action(const struct action* actions, unsigned count, int id) {
    unsigned i;

    for (i = 0; i < count; i++) {
        if (actions[i].id == id) {
            return &actions[i];
        }
    }
    return NULL;
}

static void add_name(struct edge_rules* out, const char* name) {
    size_t used = strlen(out->other_names);
    size_t cap  = sizeof out->other_names;
    size_t need = strlen(name) + (used > 0 ? 2 : 0);

    out->others++;
    /* Keep room for ", ..." so a cut list still reads as cut. */
    if (used + need + 6 > cap) {
        if (used + 6 <= cap && (used < 3 || strcmp(out->other_names + used - 3, "...") != 0)) {
            snprintf(out->other_names + used, cap - used, "%s...", used > 0 ? ", " : "");
        }
        return;
    }
    snprintf(out->other_names + used, cap - used, "%s%s", used > 0 ? ", " : "", name);
}

void edge_rules_scan(const char* rules_xml, const char* actions_xml, struct edge_rules* out) {
    static const struct action none;
    struct action actions[MAX_ACTIONS];
    unsigned action_count;
    const char* cursor;
    const char* end;
    struct xml_el el;
    unsigned i;

    memset(out, 0, sizeof *out);
    out->state     = RULE_MISSING;
    out->action_id = -1;
    if (rules_xml == NULL || actions_xml == NULL) {
        out->state = RULE_UNKNOWN;
        return;
    }
    action_count = read_actions(actions_xml, actions, MAX_ACTIONS);
    for (i = 0; i < action_count; i++) {
        if (actions[i].is_ours && actions[i].named_ours) {
            out->action_id = actions[i].id;
            break;
        }
    }
    cursor = rules_xml;
    end    = rules_xml + strlen(rules_xml);
    while (xml_next(&cursor, end, "ActionRule", &el)) {
        char name[96];
        char enabled[8];
        char primary[16];
        char topic[160];
        const struct action* action;
        bool on;

        xml_child_text(&el, "Name", name, sizeof name);
        xml_child_text(&el, "Enabled", enabled, sizeof enabled);
        xml_child_text(&el, "PrimaryAction", primary, sizeof primary);
        xml_child_text(&el, "TopicExpression", topic, sizeof topic);
        action = find_action(actions, action_count, atoi(primary));
        if (action == NULL) {
            action = &none;
        }
        on = strcmp(enabled, "true") == 0;
        if (strcmp(name, EDGE_RULE_NAME) == 0) {
            bool wired = on && action->is_ours && strcmp(topic, EDGE_EVENT_TOPIC) == 0;
            /* If there are several, one good copy is enough. */
            if (wired) {
                out->state = RULE_OK;
            } else if (out->state != RULE_OK) {
                out->state = RULE_CHANGED;
            }
        } else if (on && action->activates_source) {
            add_name(out, name[0] != '\0' ? name : "(unnamed rule)");
        }
    }
}
