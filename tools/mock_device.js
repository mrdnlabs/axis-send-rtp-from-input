#!/usr/bin/env node
/*
 * Mock AXIS device for working on the settings page without a camera.
 *
 *   node tools/mock_device.js [--port 8099] [--latency 40] [--save-ms 300] [--fail 0]
 *
 * Open http://localhost:8099/local/lineinrtp/ . It serves app/html/index.html
 * and fakes the app's API behind the reverse proxy:
 *   GET  /local/lineinrtp/api/status   level, gate, counters and 100 ms history
 *   POST /local/lineinrtp/api/config   saves, confirmed via configRev
 *   GET  /axis-cgi/param.cgi           AudioSource.A0 input type and gain
 * The input alternates speech-like bursts and silence near -90 dBFS, and the
 * gate follows the same rules as app/rtpcore.c.
 *
 *   --latency  ms added to every API reply (slow device or network)
 *   --save-ms  ms per changed parameter before a save is confirmed
 *              (each ax_parameter_set with sync is a D-Bus round trip)
 *   --fail     fraction 0-1 of status requests that fail with 502
 *   --hang     fraction 0-1 of status requests that never get a reply
 *
 * While it runs, POST /mock/control with JSON such as {"latency":2000} or
 * {"down":true} changes the behaviour without a restart. The audio manager
 * side is set the same way: {"edgeSite":true,"siteName":"Lobby"} puts the
 * mock in an Audio Manager Edge site, {"proApp":true} runs the Pro app, and
 * edgeLineIn (1, 0, -1), edgeRule ("ok", "changed", "missing"),
 * edgeOtherRules (names, comma-separated) and edgeConfirms (false: Edge
 * never starts the page) shape what the Edge card shows.
 */
"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf("--" + name);
  return i >= 0 ? Number(args[i + 1]) : def;
};
const PORT = opt("port", 8099);
const knobs = {
  latency: opt("latency", 40),
  saveMs: opt("save-ms", 300),
  fail: opt("fail", 0),
  hang: opt("hang", 0),
  down: false,
  edgeSite: false, siteName: "Lobby", proApp: false, proServer: "192.168.1.20",
  edgeLineIn: 1, edgeRule: "ok", edgeOtherRules: "", edgeConfirms: true, detectError: "",
  errors: []   /* e.g. [{"area":"capture","message":"No audio from the input, reconnecting"}] */
};
const PAGE = path.join(__dirname, "..", "app", "html", "index.html");
const FLOOR_DB = -90;
const HIST_LEN = 2048;   /* 41 s of 20 ms entries, as in the app */
const IV = 20;

const cfg = {
  enabled: true, forceOn: false, testMode: false, trigger: "level",
  destinationIp: "192.168.1.50", destinationPort: 49000,
  thresholdDb: -45, marginDb: 6, activationDelayMs: 200, deactivationDelayS: 2, maxOnS: 0,
  inputNode: "AudioDevice0Input0.Unprocessed",
  codec: "pcma", sampleRate: 48000, payloadType: 96, manager: "auto"
};
/* The same rule as mgr_resolve() in app/mgrcore.c. */
const activeMode = () => cfg.manager !== "auto" ? cfg.manager : knobs.proApp ? "pro" : knobs.edgeSite ? "edge" : "pro";
const toneMode = () => cfg.testMode && activeMode() !== "edge";
let configRev = 0;
let requestedRev = 0;
let configError = "";

const start = Date.now();
const gate = { streaming: false, waiting: false, hotSince: 0, quietSince: 0, activeSince: 0 };
const hist = { seq: 0, level: [], gate: [], port1: [], port2: [] };
let levelPercent = 0;
let packetsSent = 0;
let port1 = 0;

/* Speech-like input: 2-5 s bursts with syllable dips, then 3-8 s of silence. */
let burstUntil = 0, silentUntil = 2000;
function inputPercent(t) {
  if (t >= silentUntil && t >= burstUntil) {
    burstUntil = t + 2000 + Math.random() * 3000;
    silentUntil = burstUntil + 3000 + Math.random() * 5000;
  }
  if (toneMode()) return 25;
  const db = t < burstUntil
    ? -28 + 10 * Math.sin(t / 90) - (Math.random() < 0.12 ? 25 : 0) + Math.random() * 4
    : FLOOR_DB + Math.random() * 2;
  return 100 * Math.pow(10, db / 20);
}

function step(t) {
  levelPercent = inputPercent(t);
  port1 = Math.floor(t / 7000) % 2;
  const off = (g, reason) => { if (g.streaming) { g.streaming = false; g.quietSince = 0; } g.hotSince = 0; return reason; };
  if (!cfg.enabled || (activeMode() !== "edge" && !cfg.destinationIp)) { gate.waiting = false; off(gate); return; }
  if (toneMode() || cfg.forceOn) { gate.waiting = false; if (!gate.streaming) { gate.streaming = true; gate.activeSince = t; } return; }
  let hot, idle;
  if (cfg.trigger === "level") {
    const db = levelPercent > 0 ? 20 * Math.log10(levelPercent / 100) : -120;
    hot = db >= cfg.thresholdDb;
    idle = db < Math.max(FLOOR_DB, cfg.thresholdDb - cfg.marginDb);
  } else {
    hot = (cfg.trigger === "port1" ? port1 : 1 - port1) === 1;
    idle = !hot;
  }
  if (gate.waiting) { off(gate); if (idle) gate.waiting = false; return; }
  if (!gate.streaming) {
    if (hot) {
      if (!gate.hotSince) gate.hotSince = t;
      if (t - gate.hotSince >= cfg.activationDelayMs) { gate.streaming = true; gate.quietSince = 0; gate.activeSince = t; }
    } else gate.hotSince = 0;
    return;
  }
  if (cfg.maxOnS > 0 && t - gate.activeSince >= cfg.maxOnS * 1000) { gate.waiting = true; off(gate); return; }
  if (idle) {
    if (!gate.quietSince) gate.quietSince = t;
    if (t - gate.quietSince >= cfg.deactivationDelayS * 1000) off(gate);
  } else gate.quietSince = 0;
}

function gateName() {
  if (gate.waiting) return "waiting";
  if (gate.streaming && gate.quietSince) return "holding";
  if (gate.streaming && toneMode()) return "test";
  if (gate.streaming) return "active";
  if (gate.hotSince) return "pending";
  return "inactive";
}
const GATE_CODE = { inactive: 0, pending: 1, active: 2, holding: 3, waiting: 4, test: 5 };

/* 10 ms ticks like the app, run in batches every 100 ms; one history entry
   per 20 ms holding that window's peak. */
let peak = 0;
let tick = 0;
setInterval(() => {
  const t = Date.now() - start;
  const push = (arr, v) => { arr.push(v); if (arr.length > HIST_LEN) arr.shift(); };
  for (let i = 0; i < 10; i++) {
    step(t + i * 10);
    peak = Math.max(peak, levelPercent);
    if (gate.streaming && activeMode() !== "edge" && i % 2 === 0) packetsSent++;
    if (++tick % (IV / 10) === 0) {
      push(hist.level, Number(peak.toPrecision(4)));
      push(hist.gate, GATE_CODE[gateName()]);
      push(hist.port1, port1);
      push(hist.port2, 1 - port1);
      hist.seq++;
      peak = 0;
    }
  }
}, 100);

function status(since) {
  const first = hist.seq - hist.level.length;
  const skip = since >= 0 ? Math.max(0, Math.min(hist.level.length, since + 1 - first)) : 0;
  return {
    configRev, configError,
    history: { seq: hist.seq, intervalMs: IV, level: hist.level.slice(skip), gate: hist.gate.slice(skip),
      port1: hist.port1.slice(skip), port2: hist.port2.slice(skip) },
    levelDb: levelPercent > 0 ? 20 * Math.log10(levelPercent / 100) - 3 : -120,
    levelPercent, gate: gateName(), streaming: gate.streaming,
    enabled: cfg.enabled, forceOn: cfg.forceOn, testMode: cfg.testMode, trigger: cfg.trigger,
    port1Active: port1, port2Active: 1 - port1,
    streamUrl: cfg.destinationIp ? "rtp://" + cfg.destinationIp + ":" + cfg.destinationPort : "",
    nodeConnected: true,
    thresholdDb: cfg.thresholdDb, marginDb: cfg.marginDb, activationDelayMs: cfg.activationDelayMs,
    deactivationDelayS: cfg.deactivationDelayS, maxOnS: cfg.maxOnS,
    destinationIp: cfg.destinationIp, destinationPort: cfg.destinationPort,
    inputNode: cfg.inputNode, connectedNode: cfg.inputNode, captureRate: 48000, captureChannels: 2,
    codec: cfg.codec, sampleRate: cfg.sampleRate, payloadType: cfg.payloadType,
    rtpCodec: cfg.codec === "l16" ? "L16" : "PCMA", rtpPayloadType: cfg.codec === "l16" ? cfg.payloadType : 8,
    rtpRate: cfg.codec === "l16" ? cfg.sampleRate : 8000, packetMs: cfg.codec === "l16" && cfg.sampleRate === 48000 ? 10 : 20,
    packetsSent, sendErrors: 0, samplesDropped: 0, bufferedSamples: gate.streaming ? 80 : 0,
    lastError: knobs.errors.length ? knobs.errors[0].message : "", errors: knobs.errors, lastRtpHeader: gate.streaming ? "8008a1b2c3d4e5f6deadbeef" : "",
    manager: cfg.manager, activeMode: activeMode(),
    detect: { known: true, edgeSite: knobs.edgeSite, siteName: knobs.edgeSite ? knobs.siteName : "", proApp: knobs.proApp,
      proInstalled: knobs.proApp, proAppName: knobs.proApp ? "AudioManagerPro" : "", proServer: knobs.proApp ? knobs.proServer : "", error: knobs.detectError },
    edge: edgeStatus()
  };
}

function edgeStatus() {
  const live = activeMode() === "edge" && knobs.edgeSite;
  const paging = activeMode() === "edge" && gate.streaming;
  const others = knobs.edgeOtherRules ? knobs.edgeOtherRules.split(",").map((x) => x.trim()).filter(Boolean) : [];
  return {
    paging, confirmed: paging ? (knobs.edgeConfirms ? 1 : 0) : -1,
    lineIn: live ? knobs.edgeLineIn : -1, sourceActive: live ? (paging && knobs.edgeConfirms ? 1 : 0) : -1,
    rule: live ? knobs.edgeRule : "unknown", ruleCreated: false, ruleName: "Line Input RTP: Edge paging",
    otherRules: live ? others.length : 0, otherRuleNames: live ? others.join(", ") : ""
  };
}

function validIp(ip) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(ip);
  if (!m) return false;
  const p = m.slice(1).map(Number);
  return p.every((x, i) => x <= 255 && String(x) === m[i + 1]) && p[0] !== 0 && p[0] !== 127 && p[0] < 224;
}

function applyConfig(body) {
  const next = Object.assign({}, cfg);
  const ints = { destinationPort: [1, 65535], thresholdDb: [-90, 0], marginDb: [0, 90],
    activationDelayMs: [0, 10000], deactivationDelayS: [0, 60], maxOnS: [0, 3600], payloadType: [96, 127],
    sampleRate: [16000, 48000] };
  for (const k of ["enabled", "forceOn", "testMode"]) {
    if (k in body) { if (typeof body[k] !== "boolean") return k + " must be true or false"; next[k] = body[k]; }
  }
  for (const k of Object.keys(ints)) {
    if (k in body) {
      const v = body[k];
      if (!Number.isInteger(v) || v < ints[k][0] || v > ints[k][1]) return k + " is out of range";
      next[k] = v;
    }
  }
  if ("codec" in body) {
    if (!["pcma", "l16"].includes(body.codec)) return "codec must be pcma or l16";
    next.codec = body.codec;
  }
  if ("manager" in body) {
    if (!["auto", "pro", "edge"].includes(body.manager)) return "manager must be auto, pro or edge";
    next.manager = body.manager;
  }
  if (next.sampleRate !== 16000 && next.sampleRate !== 48000) return "sampleRate must be 16000 or 48000";
  if ("trigger" in body) {
    if (!["level", "port1", "port2"].includes(body.trigger)) return "trigger must be level, port1 or port2";
    next.trigger = body.trigger;
  }
  if ("destinationIp" in body) {
    if (body.destinationIp !== "" && !validIp(body.destinationIp)) return "Destination IP is not a unicast IPv4 address";
    next.destinationIp = body.destinationIp;
  }
  if ("inputNode" in body) {
    if (!/^[A-Za-z0-9._-]{1,63}$/.test(body.inputNode)) return "inputNode is not a valid node name";
    next.inputNode = body.inputNode;
  }
  const changed = Object.keys(next).filter((k) => next[k] !== cfg[k]).length;
  const rev = ++requestedRev;
  setTimeout(() => { Object.assign(cfg, next); configRev = rev; configError = ""; }, changed * knobs.saveMs);
  return { rev };
}

function send(res, code, type, body) {
  res.writeHead(code, { "Content-Type": type, "Cache-Control": "no-store" });
  res.end(body);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  const p = url.pathname;
  let body = "";
  req.on("data", (d) => { body += d; });
  req.on("end", () => {
    if (p === "/mock/control" && req.method === "POST") {
      try { Object.assign(knobs, JSON.parse(body || "{}")); } catch (e) { return send(res, 400, "text/plain", "bad json"); }
      return send(res, 200, "application/json", JSON.stringify(knobs));
    }
    if (p === "/" || p === "/local/lineinrtp") { res.writeHead(302, { Location: "/local/lineinrtp/" }); return res.end(); }
    if (p === "/local/lineinrtp/" || p === "/local/lineinrtp/index.html") {
      return send(res, 200, "text/html; charset=utf-8", fs.readFileSync(PAGE));
    }
    const api = () => {
      if (knobs.down) return send(res, 503, "text/plain", "Service Unavailable");
      if (p === "/local/lineinrtp/api/status" && req.method === "GET") {
        if (Math.random() < knobs.hang) return; /* never answer */
        if (Math.random() < knobs.fail) return send(res, 502, "text/plain", "Bad Gateway");
        const since = url.searchParams.has("since") ? Number(url.searchParams.get("since")) : -1;
        return send(res, 200, "application/json", JSON.stringify(status(since)));
      }
      if (p === "/local/lineinrtp/api/config" && req.method === "POST") {
        let parsed;
        try { parsed = JSON.parse(body); } catch (e) { return send(res, 400, "application/json", '{"ok":false,"error":"Body is not valid JSON"}'); }
        const r = applyConfig(parsed || {});
        if (typeof r === "string") return send(res, 400, "application/json", JSON.stringify({ ok: false, error: r }));
        return send(res, 200, "application/json", JSON.stringify({ ok: true, rev: r.rev }));
      }
      if (p === "/axis-cgi/param.cgi") {
        return send(res, 200, "text/plain", "root.AudioSource.A0.InputType=line\nroot.AudioSource.A0.InputGain=0\n"
          .replace(/root\./g, ""));
      }
      send(res, 404, "application/json", '{"ok":false,"error":"Not found"}');
    };
    setTimeout(api, knobs.latency);
  });
});

server.listen(PORT, () => console.log("Mock device on http://localhost:" + PORT + "/local/lineinrtp/"));
