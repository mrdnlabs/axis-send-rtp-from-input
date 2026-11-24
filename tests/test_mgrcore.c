/**
 * Host unit tests for app/mgrcore.c. Build and run with `make -C tests`.
 * The XML fixtures are replies captured from an AXIS C8110 on AXIS OS 12.11,
 * cut down to the parts the code reads.
 * SPDX-License-Identifier: MIT
 */
#include "mgrcore.h"

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

/* ------------------------------------------------------------ mode choice */

static void test_mode_names(void) {
    enum mgr_mode mode = MGR_PRO;

    CHECK_STR(mgr_mode_name(MGR_AUTO), "auto");
    CHECK_STR(mgr_mode_name(MGR_PRO), "pro");
    CHECK_STR(mgr_mode_name(MGR_EDGE), "edge");
    CHECK(mgr_mode_parse("auto", &mode) && mode == MGR_AUTO);
    CHECK(mgr_mode_parse("pro", &mode) && mode == MGR_PRO);
    CHECK(mgr_mode_parse("edge", &mode) && mode == MGR_EDGE);
    mode = MGR_PRO;
    CHECK(!mgr_mode_parse("Edge", &mode) && mode == MGR_PRO); /* unchanged on failure */
    CHECK(!mgr_mode_parse("", &mode));
    CHECK(!mgr_mode_parse(NULL, &mode));
    CHECK_STR(mgr_active_name(ACTIVE_UNKNOWN), "unknown");
    CHECK_STR(mgr_active_name(ACTIVE_PRO), "pro");
    CHECK_STR(mgr_active_name(ACTIVE_EDGE), "edge");
}

static enum mgr_active resolve(enum mgr_mode mode, bool known, bool site, bool pro) {
    struct mgr_detect d = {known, site, pro};

    return mgr_resolve(mode, &d);
}

static void test_resolve(void) {
    /* A forced mode never depends on the device. */
    CHECK(resolve(MGR_PRO, false, false, false) == ACTIVE_PRO);
    CHECK(resolve(MGR_PRO, true, true, false) == ACTIVE_PRO);
    CHECK(resolve(MGR_EDGE, false, false, false) == ACTIVE_EDGE);
    CHECK(resolve(MGR_EDGE, true, false, true) == ACTIVE_EDGE);
    CHECK(mgr_resolve(MGR_EDGE, NULL) == ACTIVE_EDGE);
    /* Auto waits for the first look. */
    CHECK(resolve(MGR_AUTO, false, true, true) == ACTIVE_UNKNOWN);
    CHECK(mgr_resolve(MGR_AUTO, NULL) == ACTIVE_UNKNOWN);
    /* Then: Pro app wins, else an Edge site, else RTP. */
    CHECK(resolve(MGR_AUTO, true, false, false) == ACTIVE_PRO);
    CHECK(resolve(MGR_AUTO, true, true, false) == ACTIVE_EDGE);
    CHECK(resolve(MGR_AUTO, true, false, true) == ACTIVE_PRO);
    CHECK(resolve(MGR_AUTO, true, true, true) == ACTIVE_PRO);
}

/* ------------------------------------------------------------ XML reading */

static void test_xml(void) {
    const char* xml = "<?xml version=\"1.0\"?><a:Rules x=\"1\"><a:Rule id='7' note=\"a &amp; b\"><a:Name> Tom &amp; "
                      "Jerry &lt;1&gt; &#65;&#x42; </a:Name><b:Empty/><a:Name>second</a:Name></a:Rule>"
                      "<Rule><Name>plain</Name></Rule><a:Rule>unterminated";
    const char* end    = xml + strlen(xml);
    const char* cursor = xml;
    struct xml_el el;
    struct xml_el inner;
    const char* c2;
    char text[64];

    /* "Rule" must not match "Rules", and the prefix doesn't matter. */
    CHECK(xml_next(&cursor, end, "Rule", &el));
    CHECK(xml_attr(&el, "id", text, sizeof text));
    CHECK_STR(text, "7");
    CHECK(xml_attr(&el, "note", text, sizeof text));
    CHECK_STR(text, "a & b");
    CHECK(!xml_attr(&el, "missing", text, sizeof text));
    CHECK_STR(text, "");
    CHECK(xml_child_text(&el, "Name", text, sizeof text));
    CHECK_STR(text, "Tom & Jerry <1> AB");
    CHECK(!xml_child_text(&el, "Nope", text, sizeof text));
    CHECK_STR(text, "");
    /* A self-closing element has an empty body. */
    c2 = el.body;
    CHECK(xml_next(&c2, el.body + el.body_len, "Empty", &inner));
    CHECK(inner.body_len == 0);
    /* Second Rule, without a prefix. */
    CHECK(xml_next(&cursor, end, "Rule", &el));
    CHECK(xml_child_text(&el, "Name", text, sizeof text));
    CHECK_STR(text, "plain");
    /* The third never closes: not returned, and the cursor reaches the end. */
    CHECK(!xml_next(&cursor, end, "Rule", &el));
    CHECK(cursor == end);
    CHECK(!xml_next(&cursor, end, "Rule", &el));

    /* Text that doesn't fit is cut, never overrun. */
    {
        char small[5];
        cursor = xml;
        CHECK(xml_next(&cursor, end, "Rule", &el));
        CHECK(xml_child_text(&el, "Name", small, sizeof small));
        CHECK_STR(small, "Tom ");
    }
    /* Odd input doesn't crash or match. */
    cursor = "<";
    CHECK(!xml_next(&cursor, cursor + 1, "a", &el));
    cursor = "<a";
    CHECK(!xml_next(&cursor, cursor + 2, "a", &el));
    cursor = "<a b=\"1";
    CHECK(!xml_next(&cursor, cursor + 7, "a", &el));
    cursor = "";
    CHECK(!xml_next(&cursor, cursor, "a", &el));
    {
        const char* odd = "<a b=1 c='2' d = \"3\" e>x</a>";
        cursor          = odd;
        CHECK(xml_next(&cursor, odd + strlen(odd), "a", &el));
        CHECK(!xml_attr(&el, "c", text, sizeof text)); /* unquoted value stops the scan */
        CHECK(el.body_len == 1 && el.body[0] == 'x');
    }
}

/* ---------------------------------------------------- installed apps list */

#define APP(name, status)                                                                                             \
    " <application Name=\"" name "\" NiceName=\"x\" Vendor=\"v\" Version=\"1.0\" Status=\"" status                    \
    "\" License=\"None\"><Resources><Resource name=\"DeepLearningProcessor\" used=\"No\" /></Resources></application>\n"

static void test_app_list(void) {
    char name[32];
    bool running = true;

    /* As captured: two unrelated apps. */
    CHECK(!app_list_find_pro("<reply result=\"ok\">\n" APP("c8110var", "Running") APP("lineinrtp", "Running") "</reply>",
                             name, sizeof name, &running));
    CHECK_STR(name, "");
    CHECK(!running);
    /* The names in AXIS Audio Manager Pro 5.1.44's own device packages. */
    CHECK(app_list_find_pro("<reply result=\"ok\">\n" APP("lineinrtp", "Running") APP("AudioManagerPro", "Running")
                            "</reply>",
                            name, sizeof name, &running));
    CHECK_STR(name, "AudioManagerPro");
    CHECK(running);
    CHECK(app_list_find_pro("<reply result=\"ok\">\n" APP("AudioManagerProB", "Stopped") "</reply>", name,
                            sizeof name, &running));
    CHECK_STR(name, "AudioManagerProB");
    CHECK(!running);
    CHECK(app_list_find_pro("<reply result=\"ok\">\n" APP("audiomanagerpro", "Running") "</reply>", name, sizeof name,
                            &running));
    /* Audio Manager Pro 3's client. */
    CHECK(app_list_find_pro("<reply result=\"ok\">\n" APP("netspeaker", "Running") "</reply>", name, sizeof name,
                            &running));
    CHECK(running);
    CHECK(!app_list_find_pro("<reply result=\"ok\">\n" APP("netspeakerx", "Running") "</reply>", name, sizeof name,
                             &running));
    CHECK(!app_list_find_pro("<reply result=\"ok\"></reply>", name, sizeof name, NULL));
    CHECK(!app_list_find_pro("", name, sizeof name, &running));
    CHECK(!app_list_find_pro(NULL, name, sizeof name, &running));
}

static void test_param_value(void) {
    char v[16];

    CHECK(param_value("root.AudioManagerPro.PrimaryServerIpAddress=192.168.1.195\n", v, sizeof v));
    CHECK_STR(v, "192.168.1.195");
    CHECK(param_value("root.X.Y=10.0.0.1\r\nroot.X.Z=2\r\n", v, sizeof v));
    CHECK_STR(v, "10.0.0.1");
    CHECK(param_value("root.X.Y=\n", v, sizeof v)); /* set, but empty */
    CHECK_STR(v, "");
    CHECK(param_value("a=b=c", v, sizeof v));
    CHECK_STR(v, "b=c");
    CHECK(param_value("k=a-value-longer-than-the-buffer\n", v, sizeof v));
    CHECK_STR(v, "a-value-longer-");
    CHECK(!param_value("# Error: Error -1 getting param in group 'root.Nope'\n", v, sizeof v));
    CHECK_STR(v, "");
    CHECK(!param_value("no equals sign\nnext=1\n", v, sizeof v));
    CHECK(!param_value("", v, sizeof v));
    CHECK(!param_value(NULL, v, sizeof v));
}

/* ------------------------------------------------------------- Edge rules */

#define TOPIC_DIALECT "http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete"
#define RULE(id, name, enabled, topic, action)                                                                \
    "<aa:ActionRule><aa:RuleID>" id "</aa:RuleID><aa:Name>" name "</aa:Name><aa:Enabled>" enabled             \
    "</aa:Enabled><aa:Conditions><aa:Condition><wsnt:TopicExpression Dialect=\"" TOPIC_DIALECT "\">" topic    \
    "</wsnt:TopicExpression><wsnt:MessageContent Dialect=\"x\">boolean(//SimpleItem[@Name=\"active\" and "    \
    "@Value=\"1\"])</wsnt:MessageContent></aa:Condition></aa:Conditions><aa:PrimaryAction>" action            \
    "</aa:PrimaryAction></aa:ActionRule>"
#define RULES(body)                                                                                       \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SOAP-ENV:Envelope xmlns:aa=\"http://www.axis.com/vapix/ws/" \
    "action1\"><SOAP-ENV:Body><aa:GetActionRulesResponse><aa:ActionRules>" body                            \
    "</aa:ActionRules></aa:GetActionRulesResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>"
#define ACTION(id, name, token, activate, source)                                                             \
    "<aa:ActionConfiguration><aa:ConfigurationID>" id "</aa:ConfigurationID><aa:Name>" name                   \
    "</aa:Name><aa:TemplateToken>" token "</aa:TemplateToken><aa:Parameters><aa:Parameter Value=\"" activate   \
    "\" Name=\"activate\"></aa:Parameter><aa:Parameter Value=\"" source                                       \
    "\" Name=\"source\"></aa:Parameter></aa:Parameters></aa:ActionConfiguration>"
#define ACTIONS(body)                                                                                      \
    "<SOAP-ENV:Envelope><SOAP-ENV:Body><aa:GetActionConfigurationsResponse><aa:ActionConfigurations>" body \
    "</aa:ActionConfigurations></aa:GetActionConfigurationsResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>"

#define FIXED "com.axis.action.fixed.audiosite.activate_source"
#define CLIP                                                                                                  \
    "<aa:ActionConfiguration><aa:ConfigurationID>1</aa:ConfigurationID><aa:Name>loudringer_ringtone</aa:Name>" \
    "<aa:TemplateToken>com.axis.action.unlimited.play.audioclip</aa:TemplateToken><aa:Parameters>"            \
    "<aa:Parameter Value=\"%2Fetc%2Faudioclips%2Falarm.mp3\" Name=\"location\"></aa:Parameter>"                \
    "</aa:Parameters></aa:ActionConfiguration>"
#define LEVEL_TOPIC "tns1:AudioSource/tnsaxis:TriggerLevel"
#define RINGER RULE("1", "Loud ringer", "false", "tnsaxis:Call/State", "1")
#define OURS(enabled, action) RULE("5", EDGE_RULE_NAME, enabled, EDGE_EVENT_TOPIC, action)
#define OUR_ACTION(id) ACTION(id, EDGE_ACTION_NAME, EDGE_ACTION_TEMPLATE, "1", EDGE_SOURCE)

static void test_edge_rules(void) {
    struct edge_rules r;

    /* The lab C8110 as found: two Audio detection rules and the app's own. */
    edge_rules_scan(RULES(RINGER RULE("3", "deactivate paging", "true", LEVEL_TOPIC, "3") OURS("true", "5")
                              RULE("2", "activate paging", "true", LEVEL_TOPIC, "2")),
                    ACTIONS(CLIP ACTION("3", "Source activation", FIXED, "0", EDGE_SOURCE) OUR_ACTION("5")
                                ACTION("2", "Source activation", FIXED, "1", EDGE_SOURCE)),
                    &r);
    CHECK(r.state == RULE_OK);
    CHECK(r.action_id == 5);
    CHECK(r.others == 2);
    CHECK_STR(r.other_names, "deactivate paging, activate paging");

    /* Before the app has added anything. */
    edge_rules_scan(RULES(RINGER), ACTIONS(CLIP), &r);
    CHECK(r.state == RULE_MISSING);
    CHECK(r.action_id == -1);
    CHECK(r.others == 0);
    CHECK_STR(r.other_names, "");
    edge_rules_scan(RULES(""), ACTIONS(""), &r);
    CHECK(r.state == RULE_MISSING);

    /* The rule was deleted but its action is still there: reuse it. */
    edge_rules_scan(RULES(RINGER), ACTIONS(CLIP OUR_ACTION("9")), &r);
    CHECK(r.state == RULE_MISSING);
    CHECK(r.action_id == 9);

    /* Turned off by hand: the app leaves it alone. */
    edge_rules_scan(RULES(OURS("false", "5")), ACTIONS(OUR_ACTION("5")), &r);
    CHECK(r.state == RULE_CHANGED);
    CHECK(r.others == 0);
    /* Pointed at another action, another source, or another event. */
    edge_rules_scan(RULES(OURS("true", "1")), ACTIONS(CLIP OUR_ACTION("5")), &r);
    CHECK(r.state == RULE_CHANGED);
    edge_rules_scan(RULES(OURS("true", "5")), ACTIONS(ACTION("5", EDGE_ACTION_NAME, EDGE_ACTION_TEMPLATE, "1", "Other")),
                    &r);
    CHECK(r.state == RULE_CHANGED);
    edge_rules_scan(RULES(OURS("true", "5")), ACTIONS(ACTION("5", EDGE_ACTION_NAME, FIXED, "1", EDGE_SOURCE)), &r);
    CHECK(r.state == RULE_CHANGED);
    edge_rules_scan(RULES(RULE("5", EDGE_RULE_NAME, "true", LEVEL_TOPIC, "5")), ACTIONS(OUR_ACTION("5")), &r);
    CHECK(r.state == RULE_CHANGED);
    edge_rules_scan(RULES(OURS("true", "77")), ACTIONS(OUR_ACTION("5")), &r); /* action gone */
    CHECK(r.state == RULE_CHANGED);
    /* A broken copy beside a good one doesn't matter. */
    edge_rules_scan(RULES(OURS("false", "5") OURS("true", "5")), ACTIONS(OUR_ACTION("5")), &r);
    CHECK(r.state == RULE_OK);
    edge_rules_scan(RULES(OURS("true", "5") OURS("false", "5")), ACTIONS(OUR_ACTION("5")), &r);
    CHECK(r.state == RULE_OK);

    /* Other rules count only when enabled and aimed at the same source. */
    edge_rules_scan(RULES(RULE("2", "off rule", "false", LEVEL_TOPIC, "2") RULE("3", "mic &amp; hum", "true",
                                                                                 LEVEL_TOPIC, "2")
                              RULE("4", "other input", "true", LEVEL_TOPIC, "4") RULE("6", "clip", "true",
                                                                                      LEVEL_TOPIC, "1")),
                    ACTIONS(CLIP ACTION("2", "a", FIXED, "1", EDGE_SOURCE)
                                ACTION("4", "b", FIXED, "1", "HardwareAudioInput1")),
                    &r);
    CHECK(r.state == RULE_MISSING);
    CHECK(r.others == 1);
    CHECK_STR(r.other_names, "mic & hum");

    /* Many long names: the count is exact, the list is cut and says so. */
    {
        char rules[8192];
        size_t used = 0;
        int i;

        used += (size_t)snprintf(rules + used, sizeof rules - used, "<aa:ActionRules>");
        for (i = 0; i < 12; i++) {
            used += (size_t)snprintf(rules + used, sizeof rules - used,
                                     "<aa:ActionRule><aa:RuleID>%d</aa:RuleID><aa:Name>A rather long rule name %02d"
                                     "</aa:Name><aa:Enabled>true</aa:Enabled><aa:PrimaryAction>2</aa:PrimaryAction>"
                                     "</aa:ActionRule>",
                                     10 + i, i);
        }
        snprintf(rules + used, sizeof rules - used, "</aa:ActionRules>");
        edge_rules_scan(rules, ACTIONS(ACTION("2", "a", FIXED, "1", EDGE_SOURCE)), &r);
        CHECK(r.others == 12);
        CHECK(strlen(r.other_names) < sizeof r.other_names);
        CHECK(strstr(r.other_names, "A rather long rule name 00") == r.other_names);
        CHECK(strcmp(r.other_names + strlen(r.other_names) - 5, ", ...") == 0);
        CHECK(strstr(r.other_names, "name 11") == NULL);
    }

    /* A reply that never arrived. */
    edge_rules_scan(NULL, ACTIONS(""), &r);
    CHECK(r.state == RULE_UNKNOWN);
    edge_rules_scan(RULES(""), NULL, &r);
    CHECK(r.state == RULE_UNKNOWN);
    /* Garbage in, nothing found. */
    edge_rules_scan("not xml at all <<<", "<aa:ActionConfiguration><aa:ConfigurationID>", &r);
    CHECK(r.state == RULE_MISSING);
    CHECK(r.others == 0);

    CHECK_STR(edge_rule_state_name(RULE_UNKNOWN), "unknown");
    CHECK_STR(edge_rule_state_name(RULE_MISSING), "missing");
    CHECK_STR(edge_rule_state_name(RULE_OK), "ok");
    CHECK_STR(edge_rule_state_name(RULE_CHANGED), "changed");
}

int main(void) {
    test_mode_names();
    test_resolve();
    test_xml();
    test_app_list();
    test_param_value();
    test_edge_rules();
    if (g_failures != 0) {
        fprintf(stderr, "FAILED: %d of %d checks\n", g_failures, g_checks);
        return EXIT_FAILURE;
    }
    printf("OK: %d checks\n", g_checks);
    return EXIT_SUCCESS;
}
