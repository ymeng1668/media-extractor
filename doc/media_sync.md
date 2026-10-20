# Media Format Core

## Shared Library Integration

The independent `media-format-core` repository is now the authoritative
media-only implementation (CMake alias `media-format::core`).
Both this application and `n9m_engine` consume it as a pinned submodule at
`deps/media-format-core`. Neither application includes the other as a submodule.
The core has no dependency on either application.

Public headers live in the core's `include/media-format/`, in namespace
`media_format`. Both applications use the same
frame parser, timestamp matcher, privacy renderer, indexed-MP4 validator,
ADPCM decoder and libavformat/libavcodec video/PCM muxer. The muxer accepts
generic timed-text cues; it has no protocol-event parsing or camera control.
The applications retain their respective JSON/text and CLI contracts.

`MEDIA_EXTRACTOR_BUILD_APP` controls this application's executable and
`MEDIA_FORMAT_BUILD_TESTS` controls the shared core's tests.
Core code uses PIC, Windows DLL exports, and the shared MSVC CRT.
Tests have `media_format_` names to avoid parent collisions, and the
`media_format_boundary` test guards against protocol source leaking into the
media-only project. The previous target name is no longer provided.

Normal raw-video/PCM muxing no longer launches ffmpeg. External ffmpeg/ffprobe
remain required for privacy rendering and container operations.
Deploy `libmedia-format-core.so` / `media-format-core.dll` with consumers.
Changes to the shared implementation are committed in the core repository
once, then both consumers' submodule pins are updated.

## Historical Initial Port

Source: `n9m_engine`, commit `febcca6b7de11d1ca9310c6fd20e8ce4b93763ec`.
Sync date: 2026-10-20.

The initial port changed only this repository. The following describes that
baseline before the shared-library integration above.

## Ported Components

- Media frame headers, extension decoding, GPS and Type 13/15 summaries/alarms.
- Shared metadata timing matcher and clock-segment bounds.
- Optional vendor SDK adapter and forward category snapshot logic.
- Face/forward rendering, independent strengths and single-encode composition.
- Indexed MP4 footer validation and camera-clock-to-container PTS mapping.
- ADPCM decoder, four-byte prefix removal and continuous decode state.
- Timing, snapshot, combined rendering and MP4 safety regression tests.

The parser and timing algorithms retain the source implementation, adapted
to the `media_extractor` namespace. The media-header adapter preserves this
tool's `MetadataEvent`/RapidJSON output contract. Privacy rendering and MP4
metadata validation were moved from CLI functions into reusable core modules.
The CLI remains local rather than importing camera/network command handling.

## Intentional Boundaries

- No packet framing, device connections, authentication, camera control,
  download protocol, server, live RTSP/RTMP forwarding, or AES decryption.
- No dependency on the sibling checkout at build time or runtime.
- File conversion keeps this project's output naming and JSON structure.
- Audio conversion uses external ffmpeg with the synchronized ADPCM decoder,
  rather than importing the other application's libavformat URL writer.
- No generated or test media is included in the repository.

These initial separate source copies have since been replaced by the shared
library integration described above.

## SDK Archive

The vendor SDK archive was copied unchanged from the source repository:

`doc/streamax_ainfo_parse_api/2026-09-22/streamax_ai_sdk.zip`

SHA-256:
`d818006b5d817622ac471d9475985701117c0623114176da0604e110595cc0e5`

The SDK is optional for normal extraction and face-only rendering. It is
loaded for forward ADAS decoding only. Vendor documentation is retained under
`doc/`; the custom MP4 index/footer layout is sample-derived, not documented
as a general vendor container specification.

## Validation

- Linux build and four CTest suites passed (forward snapshots, media/audio
  extraction, privacy timing, combined rendering).
- Both `914` MP4 samples rendered successfully; all 315 frames per file and
  presentation timestamps were preserved.
- All seven `921` recordings rendered successfully, with timing-match counts
  matching the source implementation's earlier validation.
- All nine generated videos decoded without errors and their JSON sidecars
  parsed successfully.
- The source repository remained unchanged. Windows builds were not tested.
