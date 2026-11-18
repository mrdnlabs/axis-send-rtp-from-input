# Line Input RTP: quality and reliability audit

Scope: `app/lineinrtp.c` (1,769 lines), `app/html/index.html`, `app/manifest.json`, `Makefile`, `Dockerfile`, `tools/listen_rtp.py` at commit `a4fa002` (v1.4.1).
Method: I read every file. Nothing was built or run on a device, so the findings marked *verify* are inferred from the code and the PipeWire, ACAP and RTP specs.

## Status

- **1.5.0:** H1, H2, H3 and M9 fixed. M10 is fixed for the settings page: saves are confirmed by revision.
- **1.6.0:** H1 and M5's scale concern are gone. The app now owns its threshold in dBFS (`ThresholdDb`) and no longer depends on the device's Audio detection level, so the VAPIX reader was removed.
- **1.7.0:** M1, M2, M3, M4, M7, M8, L1, L2, L4, L5, L7 and L8 fixed, with host unit tests for each where the logic allows (`tests/`). The gate state machine, RTP header state, A-law encoder, validation and the decimation filter now live in `app/rtpcore.c`. Also fixed: disabling the app or losing capture during the activation delay left the gate "pending", and re-enabling skipped the delay.
- **1.7.0 page:** polling halved to 2 per second and paused while the tab is hidden, each poll asks only for new history (L6), every request has a timeout so one unanswered request can't freeze the page, and a slow save is waited out for 30 s instead of being reported as failed after 8 s. The app keeps 41 s of history so the chart is full on first load, and skips re-writing unchanged string settings on save. History entries are 20 ms instead of 100 ms, and the page's preview is a port of gate_step() run at the app's 10 ms tick, checked against the C code in CI. The page also has a dark theme, sliders that reach each setting's full range, port validation, and one notice per error with its source.
- **1.7.0 codec:** a Codec setting adds L16 (uncompressed 16-bit PCM, mono) at 16 or 48 kHz alongside G.711, for music. AXIS Audio Manager Pro accepts both L16 rates as an RTP source. 48 kHz uses 10 ms packets so each stays in one Ethernet frame. The resampling filter takes any output rate, the RTP header carries the configured payload type and clock, and a codec change starts a new SSRC. `tools/listen_rtp.py` decodes L16 and can record the stream to a WAV file (`--wav`) to compare it with the source.
- **Not yet verified on a device:** PipeWire recovery (1.5.0), and in 1.7.0 the CPU cost of the 289-tap filter on the C8110 (about 2.3 million multiply-adds per second) and the new error clearing.
- **Open:** M5 (single-buffer peak; the dBFS threshold fixes its scale, not its dips), M6 (port numbering), L3 (version in the README is updated by hand), L9 (max-on doesn't apply to Always on or Test tone, which is probably intended) and L10.

## Summary

The core is solid: the A-law encoder matches the ITU/Sun reference, input validation on the config API is strict, the HTTP server is loopback-only behind the AXIS reverse proxy with admin-only config, and everything is single-threaded on the PipeWire main loop except two small, mutex-guarded handoffs. The main reliability risks are:

1. The level trigger depends on a browser having opened the settings page.
2. The app has no recovery path after a PipeWire error.
3. Blocking parameter I/O runs on the audio thread.
4. The RTP timing details can confuse strict receivers.

---

## High

### H1. Level trigger depends on the settings page being open
`g_alarm_level` (the activation threshold) only gets a value in two ways: from `CachedAlarmLevel`, or when the **browser** reads `AudioSource.A0.AlarmLevel` through `param.cgi` and POSTs it back (`index.html:366-389`, `lineinrtp.c:397`).
- On a fresh install, nobody has opened the page, so `g_alarm_level = -1` and `update_gate()` returns `"audio detection level unavailable"` (`:640`). The gate **never opens**.
- If someone moves the device's Audio detection slider and never reopens this page, the app keeps using the old value without any warning.

**Fix:** remove the browser from the loop. Use either:
- (a) VAPIX from the ACAP (service-account credentials over D-Bus `com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials`, AXIS OS 11.6+), polling `AudioSource.A0.AlarmLevel` every few seconds off the audio thread, or
- (b) better: subscribe to the device's own audio-detection event (`tns1:AudioSource/tnsaxis:TriggerLevel`, *verify topic on the C8110*) with the existing `axevent` thread, and use it as the "hot" signal. The gate then matches the device's own detector exactly, instead of re-implementing it with a peak-% guess (see M5).

### H2. No recovery from PipeWire errors or restarts
- `on_state_changed` with `PW_STREAM_STATE_ERROR` sets `format_ready = false` (`:909-913`), but the stream object stays alive. `ensure_stream()` sees a stream with the right name and returns early (`:1151`), so the app **never reconnects**. Capture stays dead until the node disappears or the config changes.
- No `pw_core` listener exists. If the PipeWire daemon restarts, the core proxy dies silently. The process stays up, doing nothing, so `runMode: respawn` never kicks in.

**Fix:**
- On a stream error, destroy the stream and retry with backoff from `on_timeout`.
- Add `pw_core_add_listener` with an `.error` handler. When `id == PW_ID_CORE && res == -EPIPE`, call `pw_main_loop_quit()` and exit non-zero so the device respawns the app.
- Add a watchdog: if streaming and no `on_process` call has arrived for more than 2 s, rebuild the stream.

### H3. Blocking parameter I/O runs on the audio thread (*verify*)
`on_timeout` runs on the PipeWire main loop, which also delivers `on_process` (the stream has no `PW_STREAM_FLAG_RT_PROCESS`).
- Every second, `load_cfg` makes ~13 `ax_parameter_get` calls, and each one is a D-Bus round trip (`:858`).
- On every save, `apply_pending` makes 13 `ax_parameter_set(..., do_sync=TRUE)` calls (`:410-429`).

A stall of tens of milliseconds every second is enough to make the capture stream drop buffers, which you'd hear as periodic clicks while streaming.

**Fix:** use `ax_parameter_register_callback` on the existing GLib event thread, and hand the new `struct cfg` to the main loop under `g_mu`. That's the same pattern already used for `g_pending`. Write only the fields that changed, with `do_sync=FALSE`, then call `ax_parameter_sync` once.

---

## Medium

### M1. RTP timestamp doesn't advance across silence
`g_timestamp` only increments per packet sent (`:767`). After a 30 s quiet gap, the next talkspurt carries a timestamp only 20 ms after the last one. RFC 3550/3551 expect the timestamp to keep tracking the sampling clock through silence suppression; the marker bit alone isn't enough for every jitter buffer.

**Fix:** in `start_streaming()`, advance `g_timestamp` by `(now - last_stop) * 8` samples.

### M2. Failed sends freeze seq/timestamp and drop audio
On a send error (`:757-764`), the 160 samples are already consumed from the ring, but `g_seq` and `g_timestamp` don't advance. The receiver then sees contiguous sequence numbers across missing audio.

**Fix:** always advance seq and timestamp. A failed send is just packet loss.

### M3. `lastError` is sticky and the send-error clear never fires
- `set_error(strerror(errno))` stores text like "Connection refused". The clear condition searches for `"send"` (`:769`), which never appears in that text, so the error **never clears**.
- "Input node disappeared", "PipeWire stream error" and "Destination IP is not…" also persist after recovery.
- `errno` is read after `syslog()`, which may already have overwritten it.

**Fix:** keep one error slot per subsystem (capture / network / config), each with a timestamp, and clear each slot when that subsystem recovers.

### M4. Decimation aliases
48 kHz is reduced to 8 kHz by averaging 6 samples (`:935-948`, duplicated at `:1035-1048`). A 6-tap box filter barely attenuates 4–8 kHz, so that content folds back into the speech band.

**Fix (simplest and best):** ask PipeWire for `.rate = 8000, .channels = 1` in the `EnumFormat` and let its resampler do the filtering. That also removes `g_decim`, the "rate not a multiple of 8000" failure, and both copies of the mixing loop.

### M5. Gate decisions use a single-buffer peak
`hot = g_level_percent >= alarm` is evaluated against one PipeWire quantum (~21 ms). One quiet quantum inside the activation delay resets `g_hot_since`, so speech with natural dips can take far longer than `ActivationDelayMs` to open the gate.
- The 0–100 "percent" is linear peak × 100, which probably isn't the scale behind the device's Audio detection slider (*verify*). That makes the threshold borrowed from that slider approximate.

**Fix:** use a peak-hold/decay envelope (e.g. a 50 ms attack window). Or adopt H1(b) and let the device decide.

### M6. Port numbering heuristic can flip mid-run
`remember_port` guesses 0- vs 1-based numbering from the first event (`:526-537`). If port 1 fires first on a 0-based device, "Port 1" maps to index 1. Once port 0 fires later, the mapping silently switches.
- Also *verify* that the subscription delivers the initial state. If it doesn't, the port trigger reports "input state unknown" until the input toggles once.

**Fix:** read the I/O port configuration once at startup and fix the mapping then.

### M7. Pre-roll is half-discarded and counted as "dropped"
`start_streaming` keeps `activation_delay_ms × 8` samples (1,600 at the default). On the same tick, `drain_audio` trims to 800 and adds the trimmed samples to `samples_dropped` (`:799-803`). Every gate open therefore inflates "dropped", and pre-roll is capped at 100 ms. Ring overflow while pending also counts as dropped.

**Fix:** size the ring for the pre-roll you actually want. Count only real overruns.

### M8. Test tone isn't clock-accurate
`push_test_tone(80)` runs once per timer callback and ignores `expirations` (`:856,865`). Any loop stall makes the tone stream slower than 8 kHz, so the receiver underruns and the test reports false problems.

**Fix:** generate `elapsed_ms × 8` samples based on `now_ms()`.

### M9. Settings page overwrites unsaved edits
- The 500 ms poll repopulates the form whenever focus isn't inside it (`index.html:350-362`). Change a field, click on empty space, and the edit reverts. In Safari, clicking a checkbox doesn't focus it, so checkbox changes revert almost immediately.
- Because `setInterval` fires async refreshes, overlapping requests are possible.
- A failed fetch is swallowed silently. The page keeps showing stale "active" status after the app dies.

**Fix:** track dirty fields, chain `setTimeout` instead of `setInterval`, and show a "connection lost" state.

### M10. "Saved" is optimistic
`POST /api/config` returns `ok:true` before anything is persisted (`:1452-1465`). `set_param` failures only appear later in `lastError`. Two POSTs inside one 10 ms tick also lose the first one, because each is built from the same `g_status` snapshot.

**Fix:** either apply synchronously in a worker and return the real result, or return a config revision number that the UI waits to see reflected in `/api/status`.

---

## Low / hygiene

- **L1. Dead `ThresholdDb`.** It's declared in the manifest, stored, reported and logged as "threshold %d dBFS" at startup, but never used. The README table says "unused". Remove it or make it the dBFS trigger.
- **L2. Duplicated sample loop.** `consume_samples()` and the interleaved branch of `on_process` are copy-pastes. M4 removes both.
- **L3. Version is hard-coded in the README** (`1_4_1`, tag `lineinrtp:1.4.1`) and the manifest. README says "through 12.x" while the manifest has `max: "13"`.
- **L4. Address validation gaps.** `valid_ipv4` accepts loopback `127.x` and subnet-directed broadcast (`x.x.x.255`). README says broadcast is rejected, but only `255.255.255.255` actually is.
- **L5. Single-threaded HTTP server.** It has a 2 s recv timeout, so one slow client blocks status for everyone, including the 500 ms poll. Paths are matched with `strstr`. Fine behind the proxy, but drop the timeout to ~250 ms.
- **L6. Polling load.** The UI makes 2 requests every 500 ms, one of them `param.cgi`. With H1 fixed, the `param.cgi` poll goes away.
- **L7. Wrong decode in `tools/listen_rtp.py`.** `alaw_to_linear` is wrong for segments ≥ 1 (it uses `+8 << seg` instead of `+0x108 << (seg-1)`) and has the sign inverted, so "peak" is approximate. It also doesn't check PT=8 or timestamp continuity. A `--wav out.wav` option would make field checks much easier.
- **L8. No tests or CI.**
  - Host unit tests: `linear_to_alaw` (against a reference table), `valid_ipv4`, the JSON config parser, and the gate state machine. That last one means moving the gate globals into a `struct gate` with a pure `gate_step(&g, now, hot, idle, cfg)`.
  - A GitHub Action that runs the Docker build.
- **L9. Max-on doesn't apply to Force-on or Test mode.** That's probably intended, but the UI diagram implies it always applies.
- **L10. Unsynchronized globals across threads.** `g_event_loop` and `impl->http_listen_fd` are read across threads without atomics. Benign in practice; use `g_atomic_pointer`.

## Suggested order

1. H2 (recovery) and H3 (blocking I/O). These are small, contained changes with the biggest field-reliability payoff.
2. H1(b): device audio-detection event as the trigger. This also fixes M5 and L6.
3. M1, M2, M3: RTP correctness and honest errors.
4. M4: let PipeWire resample to 8 kHz mono.
5. UI rewrite (M9, M10) together with the new design.
6. L8 tests, then the rest.
