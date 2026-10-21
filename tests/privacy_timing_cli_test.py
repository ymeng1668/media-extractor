"""Exercise face timestamp alignment through extraction, muxing and rendering."""
import pathlib
import itertools
import json
import re
import struct
import subprocess
import sys
import tempfile


APP, FFMPEG = sys.argv[1:3]


def run(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def extension(kind, body):
    return bytes([kind]) + (len(body) + 4).to_bytes(3, "little") + body


with tempfile.TemporaryDirectory(prefix="media-privacy-timing-") as folder:
    root = pathlib.Path(folder)
    encoded = run([FFMPEG, "-v", "error", "-f", "lavfi", "-i",
                   "color=white:size=64x64:rate=25", "-frames:v", "8",
                   "-vf", "drawgrid=w=4:h=4:t=1:c=black,drawbox=x=0:y=0:w=32:h=64:c=white:t=fill",
                   "-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "veryfast",
                   "-x264-params", "aud=1:keyint=8:bframes=0", "-f", "h264", "pipe:1"])
    if b"Unknown encoder" in encoded.stderr:
        print("Skipping: ffmpeg lacks libx264")
        sys.exit(77)
    check(encoded.returncode == 0, encoded.stderr.decode(errors="replace"))
    offsets = [m.start() for m in re.finditer(b"\x00\x00\x00\x01\x09", encoded.stdout)]
    check(len(offsets) == 8 and offsets[0] == 0, "Expected eight AUD-delimited access units")
    offsets.append(len(encoded.stdout))
    payloads = [encoded.stdout[a:b] for a, b in zip(offsets, offsets[1:])]

    def fixture(name, pts, carrier, ai_pts):
        result = bytearray()
        for index, payload in enumerate(payloads):
            entries = [extension(1, struct.pack("<I", 64 | (64 << 12) | (25 << 24)))]
            if pts is not None:
                entries.append(extension(3, struct.pack("<Q", pts[index])))
            if index == carrier:
                text = b"rLast:16/16/16/16\x00Rx:0/0/64/64\x00"
                entries.append(extension(13, struct.pack("<IQI", 0, ai_pts, len(text)) + text))
            ext = b"".join(entries)
            result += (b"\x00" + (b"2dc" if index == 0 else b"3dc") +
                       len(payload).to_bytes(3, "little") + b"\x00" +
                       len(ext).to_bytes(3, "little") + bytes([len(entries)]) + ext + payload)
        path = root / (name + ".264")
        path.write_bytes(result)
        return path

    def render(name, pts, carrier, ai_pts, masked):
        source = fixture(name, pts, carrier, ai_pts)
        output = root / (name + ".mp4")
        proc = run([APP, str(source), "--face-mask",
                    "--ffmpeg", FFMPEG, "--o", str(output)])
        log = proc.stderr.decode(errors="replace")
        if masked is None:
            check(proc.returncode != 0 and not output.exists(), "Unsafe timing produced output: " + log)
            return
        check(proc.returncode == 0, log)
        decoded = run([FFMPEG, "-v", "error", "-i", str(output),
                       "-map", "0:v:0", "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"])
        check(decoded.returncode == 0, decoded.stderr.decode(errors="replace"))
        check(len(decoded.stdout) == 8 * 64 * 64, "Output frame count changed")
        for index in range(8):
            value = decoded.stdout[index * 64 * 64 + 24 * 64 + 24]
            check(value < 30 if index in masked else value > 200,
                  f"{name}: wrong mask on frame {index}, luma={value}\n{log}")

    render("delayed", [1000 + 40 * i for i in range(8)], 6, 1040, set(range(1, 8)))
    render("reordered", [1000, 1080, 1040, 1120, 1160, 1200, 1240, 1280], 6, 1040, set(range(2, 8)))
    render("gap", [1000, 1040, 1080, 1120, 5000, 5040, 5080, 5120], 3, 1040, {1, 2, 3})
    render("reset", [10000, 10040, 10080, 10120, 1000, 1040, 1080, 1120], 6, 1040, {5, 6, 7})
    render("missing_pts", None, 6, 1040, None)
    render("unmatched", [1000, 1040, 1800, 1840, 1880, 1920, 1960, 2000], 6, 1400, None)

    source_mp4 = root / "delayed.mp4"
    for option in ["--face-frame", "--face-mask", "--face-blur=50", "--forward-frame", "--forward-blur=50"]:
        output = root / "unsafe.mp4"
        proc = run([APP, str(source_mp4), option, "--o", str(output)])
        check(proc.returncode != 0 and not output.exists() and b"no verified mapping" in proc.stderr,
              "Unverified MP4 timing accepted: " + option)
    remuxed = root / "remuxed.mp4"
    proc = run([APP, str(source_mp4), "--json", "--o", str(remuxed)])
    check(proc.returncode == 0 and remuxed.exists(), "Normal MP4 conversion regressed")

    for options in [["--face-frame", "--face-mask"], ["--face-blur=20", "--face-frame"],
                    ["--forward-frame", "--forward-blur=70"]]:
        proc = run([APP, *options, "--help"])
        check(proc.returncode != 0 and b"select only one" in proc.stderr,
              "Conflicting modes accepted: " + str(options))
    for options in itertools.product(["--face-frame", "--face-mask", "--face-blur=25"],
                                     ["--forward-frame", "--forward-blur=70"]):
        for ordered in [options, options[::-1]]:
            proc = run([APP, *ordered, "--help"])
            check(proc.returncode == 0, "Compatible modes rejected: " + str(ordered))
            proc = run([APP, str(source_mp4), *ordered, "--o", str(root / "unsafe.mp4")])
            check(proc.returncode != 0 and b"no verified mapping" in proc.stderr,
                  "Combined modes bypassed MP4 timing safety")

    if len(sys.argv) > 3:
        source = fixture("combined", [1000 + 40 * i for i in range(8)], 6, 1040)
        def combined(options):
            output = root / "combined.mp4"
            proc = run([APP, str(source), *options, "--ai-sdk-dir", sys.argv[3],
                        "--ffmpeg", FFMPEG, "--json", "--o", str(output)])
            check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
            decoded = run([FFMPEG, "-v", "error", "-i", str(output), "-map", "0:v:0",
                           "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"])
            check(decoded.returncode == 0 and len(decoded.stdout) == 8 * 64 * 64 * 3,
                  "Combined graph failed or changed frame count")
            return decoded.stdout, proc.stderr

        baseline, _ = combined(["--forward-blur=0"])
        pixels, log = combined(["--forward-blur=70", "--face-frame"])
        reverse, _ = combined(["--face-frame", "--forward-blur=70"])
        check(pixels == reverse, "Argument order changed combined rendering")
        check(b"Forward blur strength: 70/100" in log, "Forward strength overwritten")
        face = [pixels[(4 * 4096 + y * 64 + x) * 3:][:3]
                for y in range(8, 40) for x in range(8, 32)]
        check(any(g > r + 40 and g > b + 40 for r, g, b in face), "Face outline absent")
        def spread(data, frame):
            values = [data[(frame * 4096 + y * 64 + x) * 3]
                      for y in range(24, 40) for x in range(40, 56)]
            return max(values) - min(values)
        check(spread(pixels, 4) < spread(baseline, 4) / 2, "Forward blur absent")
        check(spread(pixels, 0) > 150, "Forward blur started before AI timestamp")
        for face_mode in ["--face-mask", "--face-blur=0", "--face-blur=25"]:
            for forward_mode in ["--forward-frame", "--forward-blur=0", "--forward-blur=70"]:
                _, log = combined([face_mode, forward_mode])
                if face_mode == "--face-blur=25":
                    check(b"Face blur strength: 25/100" in log, "Face strength overwritten")

    raw = root / "plain.h264"
    raw.write_bytes(encoded.stdout)
    base_mp4 = root / "base.mp4"
    proc = run([FFMPEG, "-v", "error", "-r", "25", "-i", str(raw), "-c:v", "copy", str(base_mp4)])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    def indexed_record(index, marker=20201030, dsc_only=False):
        # Delivered at sample 6, but AI PTS belongs to sample 1 after footer alignment.
        text = b"rLast:16/16/16/16\x00Rx:0/0/64/64\x00"
        if dsc_only:
            text = b"!\x00" + text
        ext = (extension(5, struct.pack("<II", marker, index)) +
               extension(13, struct.pack("<IQI", 0, 9001040, len(text)) + text))
        return b"\x003dc" + bytes(4) + len(ext).to_bytes(3, "little") + b"\x02" + ext

    indexed = root / "indexed.mp4"
    def with_footer(tail, origin=9001000):
        base = base_mp4.read_bytes()
        return base + tail + struct.pack("<QQ10I", len(base), origin,
                                        2026, 9, 14, 16, 49, 59, 56, 0xff313233, 0, 0)
    indexed.write_bytes(with_footer(indexed_record(6)))
    output = root / "indexed-mask.mp4"
    proc = run([APP, str(indexed), "--face-mask", "--json", "--hex", "--o", str(output)])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    decoded = run([FFMPEG, "-v", "error", "-i", str(output), "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"])
    check(decoded.returncode == 0 and len(decoded.stdout) == 8 * 4096, "Indexed rendering changed frame count")
    indexed_events = json.loads(output.with_suffix(".json").read_text())
    check(any(e["kind"] == "T13_HEX" and e["video_frame_idx"] == 6 for e in indexed_events),
          "Indexed hex must use the carrier sample index")
    for index in range(8):
        value = decoded.stdout[index * 4096 + 24 * 64 + 24]
        check(value < 30 if index >= 1 else value > 200,
              "MP4 metadata carrier index used instead of footer-aligned AI timestamp")
    for name, tail in [("bad_magic", indexed_record(6, 0)), ("out_of_range", indexed_record(8)),
                       ("reversed", indexed_record(6) + indexed_record(2)),
                       ("duplicate", indexed_record(6) * 2), ("truncated", indexed_record(6)[:-1])]:
        invalid = root / (name + ".mp4")
        invalid.write_bytes(with_footer(tail))
        bad_output = root / (name + "-out.mp4")
        proc = run([APP, str(invalid), "--face-frame", "--o", str(bad_output)])
        check(proc.returncode != 0 and not bad_output.exists(), "Invalid MP4 index accepted: " + name)
    for name, data in [("no_footer", base_mp4.read_bytes() + indexed_record(6)),
                       ("bad_anchor", with_footer(indexed_record(6), 1000)),
                       ("bad_footer", with_footer(indexed_record(6))[:-12] + bytes(12))]:
        invalid = root / (name + ".mp4")
        invalid.write_bytes(data)
        bad_output = root / (name + "-out.mp4")
        proc = run([APP, str(invalid), "--face-frame", "--o", str(bad_output)])
        check(proc.returncode != 0 and not bad_output.exists(), "Invalid MP4 footer accepted: " + name)
    if len(sys.argv) > 3:
        source = indexed
        pixels, log = combined(["--forward-blur=70", "--face-frame"])
        check(b"MP4 camera-clock anchor: 9001000 ms" in log, "MP4 forward path ignored clock footer")
        check(spread(pixels, 0) > 150 and spread(pixels, 4) < 100,
              "MP4 forward region not aligned to footer-based AI timestamp")
        indexed.write_bytes(with_footer(indexed_record(6, dsc_only=True)))
        pixels, log = combined(["--forward-blur=70", "--face-frame"])
        check(b"no forward objects will be hidden" in log and spread(pixels, 4) > 150,
              "Combined driver-only file did not warn about missing forward coverage")
        no_adas_output = root / "no-adas.mp4"
        proc = run([APP, str(indexed), "--forward-blur=70", "--ai-sdk-dir", sys.argv[3],
                    "--o", str(no_adas_output)])
        check(proc.returncode != 0 and not no_adas_output.exists(), "Forward-only file with no ADAS accepted")

    vfr = root / "vfr.mp4"
    proc = run([FFMPEG, "-v", "error", "-i", str(base_mp4),
                "-vf", "settb=1/1000,setpts=80*N-40*floor(N/3)+10*gte(N\\,4)",
                "-fps_mode", "passthrough", "-enc_time_base", "1:1000",
                "-c:v", "libx264", "-bf", "0", str(vfr)])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    base_mp4 = vfr
    indexed.write_bytes(with_footer(indexed_record(6)))
    proc = run([APP, str(indexed), "--face-frame", "--o", str(output)])
    check(proc.returncode == 0, proc.stderr.decode(errors="replace"))
    def timestamps(path):
        probe = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
                     "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(path)])
        check(probe.returncode == 0, probe.stderr.decode(errors="replace"))
        return [f["best_effort_timestamp_time"] for f in json.loads(probe.stdout)["frames"]]
    check(timestamps(indexed) == timestamps(output), "VFR presentation timestamps changed")

print("Face masks follow AI PTS; holds stop at discontinuities; unsafe mappings are refused.")
