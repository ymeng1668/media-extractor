# Media Implementation Sync

Source: `n9m_engine`, commit `febcca6b7de11d1ca9310c6fd20e8ce4b93763ec`.
Sync date: 2026-10-20.

Only this repository was changed. This is a standalone source port, not a
submodule, symlink, or link to the other project's shared library.

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

The projects still have separate source copies. Future fixes must be ported
and tested explicitly until a shared media library is adopted by both projects.

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
