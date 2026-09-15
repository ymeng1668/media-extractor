# media-extractor

`media-extractor` extracts raw audio/video payloads and frame-extension metadata from documented Streamax media frame files, and can pass through existing MP4 inputs.

This project is intentionally scoped to documented Streamax media-frame files:

- It only implements the media frame format described by the documents under `doc/` in the source reference tree.
- It does not implement packet framing, device login, camera query/download, live view, server mode, encryption handling, or any network protocol.
- It treats `.264` / `.265` inputs as Streamax media-frame files or raw Annex-B video payloads.
- It treats `.mp4` inputs as existing containers and remuxes/copies them with `ffmpeg`.
- When an MP4 has appended documented Streamax media-frame metadata after the normal MP4 boxes, it parses that tail for JSON/text sidecars without adding packet or network protocol support.

## Build

```bash
cmake -S . -B build
cmake --build build
```

The executable is written to:

```bash
./bin/media_extractor
```

## Usage

Single file:

```bash
./bin/media_extractor input.264 --o .raw --json --text
```

Single file to MP4:

```bash
./bin/media_extractor input.264 --o ./out/.mp4 --json
```

Existing MP4 to output MP4:

```bash
./bin/media_extractor input.mp4 --o ./out/.mp4 --json
```

Face privacy rendering from Type 13 `face_rect` metadata:

```bash
# Draw boxes around detected face/head regions.
./bin/media_extractor --face-frame input.mp4 --o ./debug/input_face_frame.mp4 --json

# Black out detected face/head regions.
./bin/media_extractor --face-mask --input-folder ../media --r ./private/.mp4 --json

# Blur strength is 0..100. 0 disables blur, 100 is max blur.
./bin/media_extractor --face-blur=25 input.mp4 --o ./private/input_blur.mp4
./bin/media_extractor --face-blur=75 input.mp4 --o ./private/input_blur_strong.mp4
```

Batch folder:

```bash
./bin/media_extractor --input-folder ../media --r ./out/.raw --json
```

Batch folder to an output directory:

```bash
./bin/media_extractor --input-folder ../media --r ./out --json
```

Batch folder to MP4:

```bash
./bin/media_extractor --input-folder ../media --r ./out/.mp4 --json
```

Batch MP4 inputs with appended Streamax metadata:

```bash
./bin/media_extractor --input-folder ../../velocitor/media/914 --r ./out/.mp4 --json
```

Help:

```bash
./bin/media_extractor -h
```

## Output

For a Streamax media-frame input, raw extraction writes:

- `<base>.h264` or `<base>.h265` for video payloads.
- `<base>.audio` for documented audio frame payloads.
- `<base>.json` when `--json` is supplied.
- `<base>.txt` when `--text` is supplied.

Raw Annex-B inputs are detected and copied to the requested raw video output path.
In batch mode, an extensionless `--r` value is treated as an output directory,
even when the directory does not exist yet.

For `.mp4`, `.mkv`, or `.mov` output specs, Streamax media-frame inputs are first
extracted to a temporary raw video payload, then muxed into the requested
container with `ffmpeg` video stream copy. Existing MP4 inputs are remuxed/copied
directly with `ffmpeg -c copy`. The Streamax media-frame mux path is video-only
for now; documented audio frames are not decoded or muxed yet.

For MP4 inputs, the extractor also scans the bytes after the final valid
top-level MP4 box. If a documented Streamax media-frame tail is found there,
the tail is parsed in metadata-only mode. The MP4 video/audio streams still come
from the container; sidecar JSON/text comes from the appended frame extensions.

`--face-mask`, `--face-blur[=0..100]`, and `--face-frame` force H.264 MP4
output. They use the Type 13 DSC `rLast:x/y/w/h/...` field exposed as
`face_rect`, scale that metadata coordinate space to the decoded video size when
needed, and transcode with ffmpeg/libx264 using `-preset veryfast -crf 27
-movflags +faststart`. Face metadata is not always present on every frame, so
the renderer keeps the last face region active for up to 30 frames and only
turns it off after that gap. `--face-blur` uses a light ROI blur by default
(`25`) so face edges remain visible; `0` disables the blur overlay and `100` is
the strongest blur.

## Metadata

The parser currently summarizes documented extension data:

- Type 1: video info.
- Type 2: real-time timestamp.
- Type 3: virtual timestamp / video PTS.
- Type 5: audio info.
- Type 9: video encoding indicator.
- Type 11: GPS frame, when present in the documented RMFI GPS layout.
- Type 13: AI information wrapper (`RMFI2_VIDEOAIINFO`) plus best-effort ASCII diagnostics from the AI body.
- Type 15: industry extension / OSD and mosaic information.

Type 13 AI body decoding is not a replacement for the proprietary Streamax AI
parser library. It can identify recorded labels such as `dsc_face_detect`,
`dsc_eys_status`, `dsc_yawn_and_pose`, `dsc_call`, and `dsc_belt` when visible
in the body. It also exports observed DSC privacy fields such as `face_rect`,
`face_count`, `face_status`, `face_score`, `mask_state`, `mask_score`,
`mask_rect`, and `phone_rect` when those ASCII diagnostics are present. The face
privacy renderer uses `face_rect`; other candidate regions are exported for
review but are not used for face masking by default.

JSON output is written with RapidJSON pretty formatting. Each metadata object is
frame based and includes `video_frame_idx`, `duration_s`, `kind`, `text`, and a
`fields` object.
