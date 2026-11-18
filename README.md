# Line Input RTP

This ACAP sends the C8110 line input as unicast RTP, either G.711 A-law or uncompressed L16, while the input stays above a level threshold.

## Prerequisites

- AXIS C8110, or another AXIS OS audio device with PipeWire
- AXIS OS 12.11.72 through 13. The C8110 is `armv7hf` (1.4.1 was tested on one running 12.11.77). An `aarch64` build is published for ARTPEC-8 and ARTPEC-9 devices.
- Docker, for the ACAP Native SDK images `axisecp/acap-native-sdk:12.11.0-<arch>-ubuntu24.04`

## How it works

PipeWire captures `AudioDevice0Input0.Unprocessed`. The app measures the input peak in dBFS. When it reaches the activation level, the app opens a gate after a short activation delay and holds it through brief gaps. While the gate is open it sends RTP packets in the chosen codec to the configured IPv4 address and UDP port. When the gate closes, packets stop.

The capture is mixed to mono and passed through a 4 kHz low-pass filter before it is reduced to 8 kHz, so content above the telephone band doesn't fold back into it. The first packets after the gate opens carry up to 200 ms of audio from before it opened (the activation delay), so the start of the sound that triggered it isn't lost. The RTP timestamp keeps counting through quiet gaps, as RFC 3551 expects for silence suppression, and a failed send counts as a lost packet.

The activation level is this app's own setting, `ThresholdDb`, from −90 to 0 dBFS in 1 dB steps. The gate closes once the peak drops below the activation level minus `MarginDb`. That closing level never goes below −90 dBFS, which is where silence on the C8110 sits. Before 1.6.0 the activation level followed the device's Audio detection setting, a whole number from 0 to 100 on a linear scale. That setting couldn't go below about −40 dBFS, so quiet microphone sources never triggered.

Parameter reads and writes run on a separate control thread, so a save never stalls audio capture. If the PipeWire stream fails, or delivers no audio for 5 seconds, the app rebuilds it with a backoff of 1 to 30 seconds. If the PipeWire daemon goes away, the app exits so AXIS OS respawns it.

## Settings page

The page at `/local/lineinrtp/` has three parts:

- **Live chart.** The last 24 seconds of input peak level in dBFS, from −90 to 0 dB, so quiet and loud sources are both readable. Lines mark where the gate opens and closes, and a timeline below shows what was actually sent. Dark green is opening, with a caret where the trigger started. Light green is sending. Amber is holding: still sending while the release hold counts down. Purple is the max-on limit re-arming. The bars use the same time scale and width as the graph at any page size. In I/O-port mode the chart shows the port state instead.
- **Input settings and hints.** The page reads the device's audio input type and gain (`AudioSource.A0.InputType` and `InputGain`) and shows them beside the chart. It shows a hint for a faint signal (loudest peak between −75 and −45 dBFS over 10 s) or a clipping one, worded for the current input type. Silence on the C8110 sits near −90 dBFS, so a quiet room doesn't trigger the hint.
- **Preview.** Moving any setting replays the recorded input with the unsaved values and shows the result as a second timeline, before you save.
- **One Mode control.** Off, Auto, Always on, and Test tone replace the separate Enabled, Force on, and Test mode switches. The trigger choice (audio level, I/O port 1, I/O port 2) shows only in Auto.

In a window 900 px or wider, the chart stays in view on the left while the settings scroll on the right, so you can watch the preview as you move a slider. The header with Save stays at the top at any width.

The Destination card has a Codec choice. For music, pick L16 48 kHz and tick **L16 - mono (48 kHz)** with the same payload type in the Audio Manager Pro source. The card names the exact row to tick.

The preview replays the app's own gate logic: the app records the input every 20 ms (about one PipeWire buffer), and the page runs each entry as two 10 ms ticks, the app's timer. CI checks that the page's replay matches the app's gate state for state (`make -C tests preview`).

The Activation delay, Release hold and Maximum on time sliders take fine steps at the low end and coarse ones higher up, so each reaches its full range (10 s, 60 s and 1 hour). The box beside each slider takes any whole value.

The page follows the system's light or dark theme. The button at the right of the header switches between system, dark and light, and the choice is kept in that browser.

You can drag the Opens and Closes lines on the chart directly. Dragging one line leaves the other in place unless it has to move: Closes can't go above Opens or below −90 dBFS. Hold Shift to move both lines together, and press Esc to cancel a drag. Each line can also take keyboard focus: the arrow keys move it 1 dB, Page Up and Page Down move it 6 dB.

The activation level slider runs from −90 to 0 dBFS. The release margin slider runs from 0 to 90 dB, but its top end shrinks as the activation level nears −90, so the closing level stays at or above the floor. Lowering the activation level pulls the margin down with it when needed. If the closing level is at or under the input's noise floor (the quietest tenth of the last 10 s), the page warns that the gate may never close. "Saved" only appears after the app has written the values and read them back.

## Build

```bash
for ARCH in armv7hf aarch64; do
  docker build --platform linux/amd64 --build-arg ARCH=$ARCH -t lineinrtp:1.7.0-$ARCH .
  id=$(docker create lineinrtp:1.7.0-$ARCH)
  docker cp "$id":/opt/app/Line_Input_RTP_1_7_0_$ARCH.eap .
  docker rm "$id"
done
```

`acap-build` names each package from the friendly name, for example `Line_Input_RTP_1_7_0_armv7hf.eap`.

## Working on the settings page

`tools/mock_device.js` serves the page with a fake device behind it, so you can work on the UI without a camera. It needs Node.js only:

```bash
node tools/mock_device.js
```

Then open http://localhost:8099/local/lineinrtp/. Options simulate a slow or unreliable device: `--latency` (ms per reply), `--save-ms` (ms per changed setting before a save is confirmed), `--fail` and `--hang` (fraction of status requests that fail or never answer). The same knobs can be changed while it runs with `curl -X POST localhost:8099/mock/control -d '{"hang":1}'`.

## Tests

The pure logic (A-law and L16 encoding, address and node-name validation, the gate state machine, RTP headers and the resampling filter) lives in `app/rtpcore.c`, with no PipeWire or AXIS SDK dependencies, so it builds and tests on any Linux or macOS host:

```bash
make -C tests            # build and run
make -C tests SAN=1      # with AddressSanitizer and UBSan
make -C tests preview    # the page's preview against the app's gate (needs Node.js)
python3 -m unittest discover -s tests
```

GitHub Actions runs these on every push and pull request, with gcc and clang, and builds unsigned `.eap` packages for `armv7hf` and `aarch64`. The packages are attached to each run as artifacts.

## Signing

Release packages are signed with the Axis ACAP Signing Service (https://www.axis.com/support/acap-signing). The service only signs a package whose `vendor` and `vendorId` match the signing account, so `app/manifest.json` carries that account's values. Signing appends a PGP signature to the unsigned `.eap`. Sign each architecture's package separately, and rebuild and re-sign for every release.

## Install

Signed releases install on AXIS OS 12 and 13 without changing any device setting:

```bash
curl --anyauth -u "$USER:$PASS" -F packfil=@signed_Line_Input_RTP_1_7_0_armv7hf.eap \
  "https://$DEVICE/axis-cgi/applications/upload.cgi"
```

An unsigned local build needs unsigned apps allowed first. On AXIS OS 12:

```bash
curl --anyauth -u "$USER:$PASS" -X POST \
  "http://$DEVICE/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true"
```

Open the app from the device's Apps page. The page is served at `/local/lineinrtp/`.

## Audio Manager Pro

Create an RTP source and leave multicast off.

- Codec: tick the one that matches the app's Codec setting, with the same payload type. The settings page shows the exact row to tick.

| App codec | Audio Manager Pro codec | Payload type | Quality | Bandwidth while sending |
| --- | --- | --- | --- | --- |
| G.711 (default) | G.711a - mono (8 kHz) | 8 | Telephone, to about 3.4 kHz | about 85 kbit/s |
| L16 16 kHz | L16 - mono (16 kHz) | 96 (settable 96–127) | Uncompressed, to about 7 kHz | about 280 kbit/s |
| L16 48 kHz | L16 - mono (48 kHz) | 96 (settable 96–127) | Uncompressed, full range; best for music | about 800 kbit/s |

- Listening port: the same port configured on the app (default 49000)
- Optional source IP: the C8110

Set the app's destination to the Audio Manager Pro server address. The settings page shows that target as `rtp://<address>:<port>` with a Copy button. The server sees packets only while the gate is open, in **Always on** mode, or in **Test tone** mode.

Set the jack to line input on the device Audio page before trusting the threshold. Silence on this C8110 sits near −90 dBFS.

## Configuration

| Parameter | Default | Meaning |
| --- | --- | --- |
| Enabled | no | Master switch. Off in the Mode control |
| ForceOn | no | Send the line input even when it is quiet. Always on in the Mode control |
| TestMode | no | Send a 1 kHz test tone instead of the line input. Test tone in the Mode control |
| Trigger | level | `level` (default), `port1`, or `port2`. A port uses that input instead of the audio level |
| DestinationIp | empty | Unicast IPv4 address of the RTP listener. Multicast, 240.0.0.0/4 (including 255.255.255.255), loopback 127.0.0.0/8, and 0.0.0.0/8 are rejected |
| DestinationPort | 49000 | UDP port |
| ThresholdDb | -45 | Activation level in dBFS peak, −90 to 0 |
| MarginDb | 6 | Close the gate this many dB below the activation level, 0 to 90. The closing level never goes below −90 dBFS |
| ActivationDelayMs | 200 | How long the trigger must stay active |
| DeactivationDelayS | 2 | How long it must stay quiet |
| MaxOnS | 0 | Force off after this many seconds. 0 disables the limit |
| InputNode | AudioDevice0Input0.Unprocessed | PipeWire source |
| Codec | pcma | `pcma` (G.711 A-law, 8 kHz, 20 ms packets) or `l16` (16-bit big-endian PCM, mono) |
| SampleRate | 48000 | L16 only: 16000 (20 ms packets) or 48000 (10 ms packets, so each fits in one Ethernet frame). The capture rate must be a whole multiple of it |
| PayloadType | 96 | L16 only: dynamic payload type, 96–127. Must match Audio Manager Pro |

`GET /local/lineinrtp/api/status` returns level, gate, packet counts, and the last error. It also returns:

- `errors`: every current error as `{area, message}`, where area is `config`, `capture` or `network`. `lastError` is the first of them.
- `history`: up to 41 s of 20 ms entries (`intervalMs`) of peak level (0–100 linear, fractional), gate code, and port states. Gate codes: 0 idle, 1 opening, 2 sending, 3 holding, 4 max-on re-arming, 5 test tone. `history.seq` is one past the newest entry's sequence number. Pass `?since=<sequence number>` to get only newer entries; the settings page does this on every poll.
- `configRev` and `configError`.

`POST /local/lineinrtp/api/config` saves the fields above. The reply carries a `rev`, and the save is complete once `/api/status` reports `configRev` at or above that value. Status is viewer. Configuration is admin.

## Known limitations

- G.711 is telephone quality and not a music codec. Use L16 48 kHz for music. Opus, which Audio Manager Pro also accepts, would cut L16's bandwidth by about ten times but needs libopus built into the app.
- L16 is mono. A stereo source is mixed down.
- Changing the codec starts a new RTP stream (new SSRC), so the receiver may take a moment to pick it up.
- The destination has to be a unicast IPv4 address. The app will not send multicast.
- Changing the input node reconnects when that PipeWire node is already known. A node that has not appeared yet is picked up on the next registry event.
- `AUDIT.md` lists the findings from the 1.5.0 review and which ones are still open.

## License

MIT. See `LICENSE`.
