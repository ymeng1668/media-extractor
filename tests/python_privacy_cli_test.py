"""Exercise Python privacy through media_extractor without downloading detector models."""
import json
import hashlib
import pathlib
import re
import struct
import subprocess
import sys
import tempfile


APP, FFMPEG, FFPROBE, REAL_PLUGIN = sys.argv[1:5]


def run(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)


def success(args):
    result = run(args)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    return result.stdout


def probe(path, *args):
    return json.loads(success([FFPROBE, "-v", "error", *args, "-of", "json", str(path)]))


def timestamps(path):
    return [float(row["best_effort_timestamp_time"]) for row in probe(
        path, "-select_streams", "v:0", "-show_frames",
        "-show_entries", "frame=best_effort_timestamp_time")["frames"]]


def packets(path, stream):
    return probe(path, "-select_streams", stream, "-show_packets", "-show_data_hash", "sha256",
                 "-show_entries", "packet=pts_time,duration_time,data_hash")["packets"]


with tempfile.TemporaryDirectory(prefix="extractor-python-privacy-") as directory:
    root = pathlib.Path(directory)
    plugin = root / "plugin"
    plugin.mkdir()
    (plugin / "privacy_redaction.py").write_text('''
import hashlib
class PrivacyRedactionProcessor:
    def __init__(self, expected_pts=None, failure="", fail_after=0,
                 mode="solid", expected_mode=None, **options):
        assert options["source_width"] == 64
        assert options["source_height"] == 64
        assert options["source_fps"] > 0
        if expected_mode is not None:
            assert options["apply_redaction"] == (mode != "highlight")
        self.expected_pts = expected_pts
        self.expected_rgb = options.get("expected_rgb")
        self.failure = failure
        self.fail_after = fail_after
        self.index = 0
        assert expected_mode is None or mode == expected_mode

    def process(self, frame, metadata):
        assert metadata["frame_index"] == self.index + 1
        if self.expected_rgb is not None:
            assert hashlib.sha256(frame.tobytes()).hexdigest() == self.expected_rgb[self.index]
        if self.expected_pts is not None:
            assert abs(metadata["pts_us"] / 1000000 - self.expected_pts[self.index]) < 0.00001
        current = self.index
        self.index += 1
        if current >= self.fail_after:
            if self.failure == "raise":
                raise RuntimeError("intentional inference failure")
            if self.failure == "invalid":
                return {"valid": False, "reason": "intentional invalid result"}
            if self.failure == "blocked":
                return {"valid": True, "blocked": True}
        frame[16:48, 16:48] = 0
        return {"valid": True}
''')
    source = root / "source.mp4"
    subtitle = root / "subtitle.srt"
    subtitle.write_text("1\n00:00:01,000 --> 00:00:01,400\nmetadata\n")
    generated = run([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "color=white:size=64x64:rate=25:duration=0.4",
                     "-f", "lavfi", "-i", "sine=frequency=440:duration=0.4", "-i", str(subtitle),
                     "-vf", "setpts='PTS+if(gte(N,4),0.08/TB,0)+1/TB'", "-af", "asetpts=PTS+1/TB",
                     "-fps_mode", "vfr", "-c:v", "libx264", "-bf", "2", "-c:a", "aac", "-c:s", "mov_text", str(source)])
    if b"Unknown encoder" in generated.stderr:
        sys.exit(77)
    assert generated.returncode == 0, generated.stderr.decode(errors="replace")
    expected_pts = timestamps(source)
    assert len(expected_pts) == 10 and expected_pts[4] - expected_pts[3] > 0.1
    config = root / "config.json"
    output = root / "redacted.mp4"
    base = [APP, "--privacy-redaction", "--privacy-plugin-dir", str(plugin),
            "--privacy-python", sys.executable, "--privacy-config", str(config)]
    config.write_text(json.dumps({"expected_pts": expected_pts, "mode": "solid"}))
    success([*base, str(source), "--o", str(output)])
    assert timestamps(output) == expected_pts, f"Video presentation timestamps changed: {timestamps(output)} != {expected_pts}"
    assert packets(output, "a:0") == packets(source, "a:0"), "Audio timestamps or payload changed"
    assert packets(output, "s:0") == packets(source, "s:0"), "Subtitle timestamps or payload changed"
    pixels = success([FFMPEG, "-v", "error", "-i", str(output), "-map", "0:v:0", "-fps_mode", "passthrough",
                      "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"])
    assert len(pixels) == 10 * 64 * 64
    for index in range(10):
        assert pixels[index * 4096 + 24 * 64 + 24] < 30, "Processed pixels were not encoded"
        assert pixels[index * 4096 + 8 * 64 + 8] > 200, "Unmasked pixels were changed"

    # A late failure must not publish the earlier successful frames or replace
    # an existing output, and every mode flag must reach the processor.
    original = output.read_bytes()
    for failure in ("raise", "invalid", "blocked"):
        config.write_text(json.dumps({"failure": failure, "fail_after": 4}))
        failed = run([*base, str(source), "--o", str(output)])
        assert failed.returncode != 0 and output.read_bytes() == original
        assert not list(root.glob(".media_python_*")), "Rendering temporary files leaked"
    for mode in ("solid", "pixelate", "blur", "highlight"):
        config.write_text(json.dumps({"mode": "solid", "expected_mode": mode}))
        success([*base, "--privacy-mode", mode, str(source), "--o", str(output)])

    # The graph pipeline preserves range during YUV420P normalization, then
    # uses default limited-range RGB conversion for inference. Direct RGB
    # decoding of this full-range fixture changes the detector's input pixels.
    full_range = root / "full-range.mp4"
    success([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=64x64:rate=5",
             "-frames:v", "3", "-c:v", "libx264", "-pix_fmt", "yuvj420p", str(full_range)])
    rgb = success([FFMPEG, "-v", "error", "-i", str(full_range), "-vf",
        "scale=64:64:flags=bicubic:out_range=full,format=yuv420p,"
        "scale=64:64:flags=bicubic:in_range=limited,format=rgb24",
        "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1"])
    assert len(rgb) == 3 * 64 * 64 * 3
    config.write_text(json.dumps({"expected_rgb": [
        hashlib.sha256(rgb[start:start + 12288]).hexdigest() for start in range(0, len(rgb), 12288)]}))
    success([*base, str(full_range), "--o", str(output)])

    config.write_text("{}")
    # Privacy output is always MP4, including default/raw/unknown output specs.
    for spec, expected in ((str(root / "auto" / ".raw"), root / "auto" / "source_mp4_fmt.mp4"),
                           (str(root / "custom.bin"), root / "custom.mp4")):
        success([*base, str(source), "--o", spec])
        assert timestamps(expected) == expected_pts
    for legacy in ("--face-mask", "--face-frame", "--face-blur=50", "--forward-frame", "--forward-blur=50"):
        for flags in ((legacy, "--privacy-redaction"), ("--privacy-redaction", legacy)):
            failed = run([APP, *flags, str(source), "--o", str(output)])
            assert failed.returncode != 0 and b"cannot be combined" in failed.stderr
    assert run([*base, str(source), "--o", str(source)]).returncode != 0
    assert run([APP, "--privacy-mode", "blur", str(source)]).returncode != 0
    assert run([*base, "--privacy-mode", "unknown", str(source)]).returncode != 0
    config.write_text("[]")
    assert run([*base, str(source), "--o", str(output)]).returncode != 0

    # Use the real entry point to verify missing models block the app as well.
    config.write_text(json.dumps({"targets": ["face"], "model_auto_discovery": False,
        "face_model": str(root / "missing.onnx"), "auto_object_track": False, "person_head_fallback": False}))
    missing_output = root / "missing.mp4"
    failed = run([*base, "--privacy-plugin-dir", REAL_PLUGIN, str(source), "--o", str(missing_output)])
    assert failed.returncode != 0 and b"privacy_fail_closed" in failed.stderr and not missing_output.exists()

    # Media-frame extraction must run image processing without requiring Type 13 data.
    encoded = success([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "color=white:size=64x64:rate=25",
        "-frames:v", "8", "-c:v", "libx264", "-x264-params", "aud=1:keyint=8:bframes=0", "-f", "h264", "pipe:1"])
    offsets = [m.start() for m in re.finditer(b"\x00\x00\x00\x01\x09", encoded)]
    assert len(offsets) == 8
    offsets.append(len(encoded))
    recording = bytearray()
    for index, (start, end) in enumerate(zip(offsets, offsets[1:])):
        payload = encoded[start:end]
        extensions = b"\x01\x08\x00\x00" + struct.pack("<I", 64 | (64 << 12) | (25 << 24))
        recording += (b"\x00" + (b"2dc" if index == 0 else b"3dc") + len(payload).to_bytes(3, "little") +
                      b"\x00" + len(extensions).to_bytes(3, "little") + b"\x01" + extensions + payload)
    recording_path = root / "media.264"
    recording_path.write_bytes(recording)
    config.write_text("{}")
    success([*base, str(recording_path), "--o", str(output)])
    assert len(timestamps(output)) == 8
    raw = root / "source.264"
    raw.write_bytes(encoded)
    success([*base, str(raw), "--o", str(output)])
    assert len(timestamps(output)) == 8

    # A new processor instance must be used for each file in a batch.
    batch = root / "batch"
    batch.mkdir()
    for name in ("a.mp4", "b.mp4"):
        (batch / name).write_bytes(source.read_bytes())
    config.write_text(json.dumps({"expected_pts": expected_pts}))
    batch_output = root / "batch-output"
    success([*base, "-i", str(batch), "--r", str(batch_output / ".mp4")])
    results = list(batch_output.glob("*.mp4"))
    assert len(results) == 2
    assert all(timestamps(path) == expected_pts for path in results)
    print("Python privacy CLI: pixels, VFR/B-frames, audio/subtitles, failures, modes, media frames, raw and batch passed")
