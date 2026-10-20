# media-extractor

`media-extractor` extracts raw audio/video payloads and frame-extension metadata from documented Streamax media frame files, and can pass through existing MP4 inputs.

This project is scoped to Streamax media-frame files and validated MP4 exports:

- Its media decoder follows the documents in `doc/`, vendor reference code, and recorded-format validation. The MP4 footer layout is inferred from samples, not a formal vendor specification.
- It does not implement packet framing, device login, camera query/download, live view, server mode, encryption handling, or any network protocol.
- It treats `.264` / `.265` inputs as Streamax media-frame files or raw Annex-B video payloads.
- It treats `.mp4` inputs as existing containers and remuxes/copies them with `ffmpeg`.
- When an MP4 has appended Streamax media-frame metadata after the normal MP4 boxes, it parses that tail for JSON/text sidecars without adding packet or network protocol support.

The media-only implementation is maintained in the separate
[`media-format-core`](https://github.com/ymeng1668/media-format-core) repository,
pinned here as `deps/media-format-core`. Both this CLI and the camera-protocol
consumer link that library independently. This project has no dependency on
the camera-protocol consumer and is not its submodule. See
[the integration record](doc/media_sync.md).

## Build

Requires C++17, RapidJSON, and FFmpeg development libraries (`libavformat`,
`libavcodec`, `libavutil`, `libswresample`; discovered through pkg-config on
Unix/MinGW). External `ffmpeg`/`ffprobe` are still needed for privacy rendering
and MP4 container processing, but raw-video/PCM muxing uses the shared library.
For standalone MSVC builds, set `FFMPEG_DIR` and, for static FFmpeg, supply its
transitive dependencies through `MEDIA_FORMAT_FFMPEG_EXTRA_LIBRARIES`.

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable is written to:

```bash
./bin/media_extractor
```

`bin/` also contains `libmedia-format-core.so` on Linux or
`media-format-core.dll` on Windows. Deploy it beside the executable.

To build only the shared dependency through this checkout:

```bash
cmake -S . -B build-lib -DMEDIA_EXTRACTOR_BUILD_APP=OFF -DBUILD_TESTING=OFF
cmake --build build-lib
```

CMake links `media-format::core` (actual target `media-format-core`).
Public headers are in `deps/media-format-core/include/media-format/`, with
APIs in namespace `media_format`. Core tests have unique `media_format_` names
and can be disabled with `MEDIA_FORMAT_BUILD_TESTS=OFF`.
Set `MEDIA_EXTRACTOR_RUNTIME_DIR` for CLI output and `MEDIA_FORMAT_RUNTIME_DIR`
for shared-library output. The default puts both in `bin/`.
The core is position-independent and uses the shared MSVC CRT.
For installed forward rendering on Linux, pass `--ai-sdk-dir` and include that
SDK's `lib`/`opencv453/lib` directories in `LD_LIBRARY_PATH` before launch.
For local core development, `MEDIA_FORMAT_SOURCE_DIR` may point to a sibling
checkout; normal builds use the committed core submodule revision.

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

# Forward ADAS rectangles and combined rendering.
./bin/media_extractor --forward-frame input.265 --o ./debug/forward.mp4
./bin/media_extractor --forward-blur=70 --face-frame --json \
  --input-folder ../../velocitor/media/914 --r ./private/.mp4
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
- `<base>.pcm` for decoded ADPCM (16-bit little-endian PCM, enabled by default).
- `<base>.json` when `--json` is supplied.
- `<base>.txt` when `--text` is supplied.

Raw Annex-B inputs are detected and copied to the requested raw video output path.
In batch mode, an extensionless `--r` value is treated as an output directory,
even when the directory does not exist yet.

For `.mp4`, `.mkv`, or `.mov` output specs, Streamax media-frame inputs are first
extracted to a temporary raw video payload, then muxed into the requested
container with `ffmpeg` video stream copy. Existing MP4 inputs are remuxed/copied
directly with `ffmpeg -c copy`. Streamax ADPCM is decoded with the supplied
`deps/media-format-core/third_party/AdpcmCodec`, skipping each frame's four-byte audio prefix and
preserving decoder state across frames. It is encoded to AAC for container
output. Other audio codecs remain raw, with a warning when omitted from a
container. `--no-decode-audio` retains raw audio without adding it to a container.
Raw `.audio` files preserve their original frame payloads, including prefixes.
Raw video payloads are not decrypted; encrypted recordings remain unsupported.

For MP4 inputs, the extractor also scans the bytes after the final valid
top-level MP4 box. If a documented Streamax media-frame tail is found there,
the tail is parsed in metadata-only mode. The MP4 video/audio streams still come
from the container; sidecar JSON/text comes from the appended frame extensions.

`--face-mask`, `--face-blur[=0..100]`, and `--face-frame` force H.264 MP4
output. They use the Type 13 DSC `rLast:x/y/w/h/...` field exposed as
`face_rect`, scale that metadata coordinate space to the decoded video size when
needed, and transcode with ffmpeg/libx264 using `-preset veryfast -crf 27
-movflags +faststart`. Face metadata is not always present on every frame, so
the renderer keeps the last face region active for up to 30 frames, bounded
by the clip and timestamp segment. `--face-blur` uses a light ROI blur by default
(`25`) so face edges remain visible; `0` disables the blur overlay and `100` is
the strongest blur.

### Timing and Forward Privacy

Face and forward observations are aligned by AI PTS, not by the metadata
carrier index, with a maximum distance of 50 ms. Timestamp jumps over 1000 ms
split segments. Invalid timing fails rendering; moving boxes are not merged
into long-lived unions.

`--forward-frame` highlights ADAS vehicles in green and pedestrians in yellow.
`--forward-blur[=0..100]` blurs those whole regions (default 50). Fresh results
replace previous rectangles independently by category; empty results clear
them. Missing updates hold the last regions for up to 30 frames. This is not
general image-based face/plate detection and cannot guarantee privacy coverage.

One face mode and one forward mode can be combined, with independent strengths,
in one H.264 encode (`veryfast`, CRF 27, faststart). Missing metadata for one
category produces an explicit coverage warning; SDK errors and invalid timing
fail. Forward-only rendering with no supported ADAS results fails.

MP4 privacy rendering requires the indexed Streamax export layout validated on
the `914` recordings: a 56-byte clock footer, ordered in-range sample indices,
and non-reordered video starting at PTS zero. Video PTS is translated into the
camera clock using the footer anchor. Sparse metadata does not shift overlays,
and MP4 variable frame timestamps are preserved. Other MP4 layouts are refused
for privacy rendering but remain available for ordinary remux/JSON extraction.
Raw Annex-B has no metadata timeline and cannot be privacy-rendered.

The optional vendor SDK is loaded only for `--forward-*`. Its archive is under
`doc/streamax_ainfo_parse_api/2026-09-22/`; CMake extracts it into the build
directory. Override with `-DMEDIA_AI_SDK_DIR=/path/to/platform` at configure time
or `--ai-sdk-dir /path/to/platform` at runtime. A relocated Linux SDK may need
its `lib` and `opencv453/lib` directories in `LD_LIBRARY_PATH`. The supplied
Linux runtime is x86-64 with a glibc 2.38 baseline; Windows requires matching
x64/x86 vendor DLLs. Windows behavior has not been validated by the Linux tests.

## Metadata

The parser currently summarizes documented extension data:

- Type 1: video info.
- Type 2: real-time timestamp.
- Type 3: virtual timestamp / video PTS.
- Type 5: audio info.
- Type 9: video encoding indicator.
- Type 11: GPS frame, when present in the documented RMFI GPS layout.
- Type 10 and embedded GPS records: additional latitude/longitude, heading,
  speed and altitude extraction where present.
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
`fields` object. Type 13/15 summaries and alarm decoding now use the synchronized
media decoder; consumers should tolerate additional fields and events.
During validated MP4 privacy rendering, `video_frame_idx` is the explicit
carrier sample index. Ordinary MP4 JSON extraction retains metadata-record
indices. Neither is a substitute for AI PTS when positioning overlays.
