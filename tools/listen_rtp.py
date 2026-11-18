"""Listen for the ACAP's RTP stream, print packet stats, and optionally record it.

    python tools/listen_rtp.py --port 49000 --seconds 30 --wav stream.wav

Decodes G.711 A-law (payload type 8) and L16 (big-endian 16-bit PCM, mono).
L16 uses a dynamic payload type (96-127 by default 96), so pass --rate to match
the app's sample rate (48000 by default). The WAV file holds exactly what
arrived, with silence where the gate was closed or packets were lost, so you
can compare it with the source and tell the stream's quality from the device's.
"""

import argparse
import math
import socket
import struct
import time
import wave

MAX_GAP_S = 10  # longest silence written for a closed gate


def alaw_to_linear(value):
    """G.711 A-law byte to 16-bit linear PCM (ITU-T G.711 / Sun g711.c alaw2linear)."""
    value ^= 0x55
    segment = (value & 0x70) >> 4
    sample = (value & 0x0F) << 4
    if segment == 0:
        sample += 8
    else:
        sample = (sample + 0x108) << (segment - 1)
    return sample if value & 0x80 else -sample


def payload_format(payload_type, l16_rate):
    """(codec name, sample rate) for a payload type, or None if unsupported."""
    if payload_type == 8:
        return ("PCMA", 8000)
    if payload_type == 11:
        return ("L16", 44100)
    if 96 <= payload_type <= 127:
        return ("L16", l16_rate)
    return None


def decode_payload(payload_type, payload, l16_rate=48000):
    """Decodes one packet's payload to a list of 16-bit samples."""
    fmt = payload_format(payload_type, l16_rate)
    if fmt is None:
        return []
    if fmt[0] == "PCMA":
        return [alaw_to_linear(b) for b in payload]
    count = len(payload) // 2
    return list(struct.unpack("!%dh" % count, payload[: count * 2]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=49000)
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--rate", type=int, default=48000, help="L16 sample rate for dynamic payload types")
    parser.add_argument("--wav", help="write what arrives to this WAV file")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)
    deadline = time.time() + args.seconds
    count = 0
    last_seq = None
    next_ts = None
    gaps = 0
    peak = 0
    sources = set()
    fmt = None
    out = None
    samples_written = 0
    try:
        while time.time() < deadline:
            try:
                packet, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            if len(packet) < 12:
                continue
            version = packet[0] >> 6
            payload_type = packet[1] & 0x7F
            marker = packet[1] >> 7
            seq, ts = struct.unpack("!HI", packet[2:8])
            payload = packet[12:]
            this_fmt = payload_format(payload_type, args.rate)
            if this_fmt is None:
                continue
            if last_seq is not None and (last_seq + 1) & 0xFFFF != seq:
                gaps += 1
            last_seq = seq
            samples = decode_payload(payload_type, payload, args.rate)
            if samples:
                peak = max(peak, max(abs(s) for s in samples))
            sources.add(addr[0])
            count += 1
            if count == 1:
                fmt = this_fmt
                print(
                    "first from %s:%d version=%d pt=%d (%s %d Hz) marker=%d seq=%d bytes=%d (%.1f ms)"
                    % (addr[0], addr[1], version, payload_type, fmt[0], fmt[1], marker, seq, len(payload),
                       1000.0 * len(samples) / fmt[1])
                )
                if args.wav:
                    out = wave.open(args.wav, "wb")
                    out.setnchannels(1)
                    out.setsampwidth(2)
                    out.setframerate(fmt[1])
            if out is not None and this_fmt == fmt:
                # Silence for the time the timestamp skipped: a closed gate or lost packets.
                if next_ts is not None:
                    skipped = (ts - next_ts) & 0xFFFFFFFF
                    if 0 < skipped <= MAX_GAP_S * fmt[1]:
                        out.writeframes(b"\x00\x00" * skipped)
                        samples_written += skipped
                out.writeframes(struct.pack("<%dh" % len(samples), *samples))
                samples_written += len(samples)
                next_ts = (ts + len(samples)) & 0xFFFFFFFF
    finally:
        if out is not None:
            out.close()
    peak_db = 20 * math.log10(peak / 32768.0) if peak > 0 else float("-inf")
    print("packets=%d gaps=%d peak=%d (%.1f dBFS) sources=%s"
          % (count, gaps, peak, peak_db, ",".join(sorted(sources)) or "-"))
    if args.wav:
        if out is None:
            print("no audio received, %s not written" % args.wav)
        else:
            print("wrote %s: %.1f s at %d Hz" % (args.wav, samples_written / float(fmt[1]), fmt[1]))


if __name__ == "__main__":
    main()
