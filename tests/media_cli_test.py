"""Payload, audio, JSON and protocol-boundary regression checks."""
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

APP, FFMPEG = sys.argv[1:3]


def run(args):
    return subprocess.run(args, capture_output=True)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def ext(kind, body):
    return bytes([kind]) + (len(body) + 4).to_bytes(3, "little") + body


def frame(kind, payload, entries):
    extensions = b"".join(entries)
    return (b"\0" + kind + len(payload).to_bytes(3, "little") + b"\0" +
            len(extensions).to_bytes(3, "little") + bytes([len(entries)]) +
            extensions + payload)


with tempfile.TemporaryDirectory(prefix="media-extraction-") as folder:
    root = pathlib.Path(folder)
    proc = run([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "color=white:size=64x64:rate=25",
                "-frames:v", "8", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "veryfast",
                "-x264-params", "aud=1:keyint=8:bframes=0", "-f", "h264", "pipe:1"])
    if b"Unknown encoder" in proc.stderr:
        sys.exit(77)
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    encoded = proc.stdout
    offsets = [m.start() for m in re.finditer(b"\0\0\0\1\x09", encoded)]
    check(len(offsets) == 8, "Expected eight access units")
    offsets.append(len(encoded))
    audio_payload = bytes(4) + b"\x11" * 64
    data = bytearray()
    for i, (a, b) in enumerate(zip(offsets, offsets[1:])):
        text = b"rLast:16/16/16/16\0Rx:0/0/64/64\0lat:37.392626\0lng:-122.108033\0"
        # Deliberately put video PTS after AI metadata to exercise entry-order independence.
        entries = [ext(1, struct.pack("<I", 64 | (64 << 12) | (25 << 24))),
                   ext(13, struct.pack("<IQI", 0, 1000 + i * 40, len(text)) + text),
                   ext(15, bytes(12)),
                   ext(3, struct.pack("<Q", 1000 + i * 40))]
        data += frame(b"2dc" if i == 0 else b"3dc", encoded[a:b], entries)
        data += frame(b"4dc", audio_payload, [])
    source = root / "input.264"
    source.write_bytes(data)
    rawdir = root / "raw"
    proc = run([APP, str(source), "--o", str(rawdir / ".raw"), "--json"])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    check(next(rawdir.glob("*.h264")).read_bytes() == encoded, "Video payload changed")
    check(next(rawdir.glob("*.audio")).read_bytes() == audio_payload * 8, "Raw audio payload changed")
    pcm = next(rawdir.glob("*.pcm")).read_bytes()
    check(len(pcm) == 8 * 64 * 4, "ADPCM prefix not stripped or PCM length incorrect")
    values = struct.unpack("<" + "h" * (len(pcm) // 2), pcm)
    check(values[128] > values[0], "ADPCM decoder state reset between frames")
    metadata = json.loads(next(rawdir.glob("*.json")).read_text())
    # Preserve the pretty-printed metadata event schema.
    events = metadata["events"] if isinstance(metadata, dict) else metadata
    ai = [e for e in events if e["kind"] == "T13_DATA"]
    check(len(ai) == 8 and ai[0]["fields"]["video_pts"] == "1000", "AI/video PTS fields lost")
    check(any(e["kind"] == "GPS" and e["fields"]["lat"] == "37.392626" for e in events),
          "Embedded GPS extraction missing")
    check(not any(e["kind"].endswith("_HEX") for e in events), "Hex exported without --hex")
    output = root / "audio.mp4"
    proc = run([APP, str(source), "--o", str(output)])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    probe = run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,codec_type",
                 "-of", "json", str(output)])
    streams = json.loads(probe.stdout)["streams"]
    check(any(s["codec_name"] == "aac" for s in streams), "Container audio missing")
    check(any(s["codec_name"] == "h264" for s in streams), "Container video missing")
    check(any(s["codec_name"] == "mov_text" for s in streams), "Default telemetry subtitles missing")
    subtitles = run([FFMPEG, "-v", "error", "-i", str(output), "-map", "0:s:0", "-f", "srt", "pipe:1"])
    check(subtitles.returncode == 0 and b"GPS" in subtitles.stdout and b"T13_DATA" in subtitles.stdout,
          "Readable telemetry cues missing")
    decoded = run([FFMPEG, "-v", "error", "-i", str(output), "-f", "null", "-"])
    check(decoded.returncode == 0 and not decoded.stderr, "Mux output does not decode cleanly")
    rawonly = root / "rawonly"
    proc = run([APP, str(source), "--o", str(rawonly / ".raw"), "--no-decode-audio"])
    check(proc.returncode == 0 and not list(rawonly.glob("*.pcm")), "Raw-audio option ignored")
    no_subtitles = root / "no-subtitles.mp4"
    proc = run([APP, str(source), "--o", str(no_subtitles), "--sub-track", "off", "--json", "--hex"])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    streams = json.loads(run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(no_subtitles)]).stdout)["streams"]
    check(not any(s["codec_type"] == "subtitle" for s in streams), "--sub-track off ignored")
    hex_events = json.loads(no_subtitles.with_suffix(".json").read_text())
    check(len([e for e in hex_events if e["kind"] == "T13_HEX"]) == 8, "Raw Type 13 data missing")
    check(len([e for e in hex_events if e["kind"] == "T15_HEX" and e["fields"]["hex"] == "00" * 12]) == 8,
          "Raw Type 15 bytes changed or missing")
    check(next(e["fields"]["hex"] for e in hex_events if e["kind"] == "T13_HEX").endswith(text.hex()),
          "Raw Type 13 bytes changed")
    styled = root / "styled.mp4"
    proc = run([APP, str(source), "--o", str(styled), "--hex", "--json", "--text",
                "--sub-color", "#123456", "--sub-pos", "top"])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    check("T13_HEX" in styled.with_suffix(".txt").read_text(), "Text hex export missing")
    subtitles = run([FFMPEG, "-v", "error", "-i", str(styled), "-map", "0:s:0", "-f", "srt", "pipe:1"])
    check(b"hex=" not in subtitles.stdout and b"T13_HEX" not in subtitles.stdout, "Raw hex leaked into subtitles")
    stream = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "s:0", "-show_streams", "-show_data",
                            "-of", "json", str(styled)]).stdout)["streams"][0]
    extra = bytes.fromhex("".join(line.split(":", 1)[1].split("  ")[0].replace(" ", "")
                                for line in stream["extradata"].splitlines() if ":" in line))
    check(extra[5] == 255 and extra[26:30] == bytes.fromhex("123456ff"), "Subtitle style ignored")
    for options in (["--sub-track", "bad"], ["--sub-color", "#12345z"], ["--sub-pos", "left"],
                    ["--r", ".mp4"]):
        check(run([APP, str(source), *options]).returncode != 0, "Invalid option accepted: " + str(options))
    for option in ["--server", "--download", "--raw-stream", "--host", "--aes-key", "--no-decrypt-video", "--live-view"]:
        check(run([APP, option]).returncode != 0, "Transport/decryption option unexpectedly accepted")
print("Payload extraction, ADPCM/AAC, metadata fields and protocol boundaries passed.")
