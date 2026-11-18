#!/usr/bin/env node
/*
 * Checks that the settings page's preview (simulate() in app/html/index.html)
 * reproduces gate_step() from app/rtpcore.c exactly.
 *
 *   make -C tests preview
 *
 * runs tests/gate_dump (C) to get inputs and the real gate states, loads only
 * the page's "BEGIN preview-sim" block into a sandbox, and compares state by
 * state.
 */
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const vectors = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const html = fs.readFileSync(path.join(__dirname, "..", "app", "html", "index.html"), "utf8");
const start = html.indexOf("/* BEGIN preview-sim");
const end = html.indexOf("/* END preview-sim */");
if (start < 0 || end < start) {
  console.error("preview-sim block not found in index.html");
  process.exit(1);
}

const sandbox = {};
vm.createContext(sandbox);
vm.runInContext(html.slice(start, end) + "\nthis.simulate = simulate;", sandbox);

let failed = 0;
let checked = 0;
for (const sc of vectors.scenarios) {
  const got = sandbox.simulate(sc.h, sc.cfg, vectors.intervalMs);
  let bad = 0;
  for (let i = 0; i < sc.states.length; i++) {
    checked++;
    if (got[i] !== sc.states[i]) {
      if (bad < 5) {
        console.error(`  ${sc.name}: entry ${i} (${i * vectors.intervalMs} ms): page ${got[i]}, app ${sc.states[i]}`);
      }
      bad++;
    }
  }
  const counts = {};
  sc.states.forEach((s) => { counts[s] = (counts[s] || 0) + 1; });
  console.log(`${bad ? "FAIL" : "ok  "} ${sc.name}: ${sc.states.length} entries, ` +
    Object.keys(counts).sort().map((k) => k + " " + counts[k]).join(", ") + (bad ? `, ${bad} differ` : ""));
  failed += bad;
}
if (failed) {
  console.error(`FAILED: ${failed} of ${checked} states differ`);
  process.exit(1);
}
console.log(`OK: preview matches the app's gate on ${checked} states`);
