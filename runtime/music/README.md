# Music libraries

Stand-ins for the sound libraries games shipped beside DirectSound. A port compiles the ones its
game imports; each plays through the runtime's mixer (`runtime/win32/sound.h`).

| File | Stand-in for | Needs |
| --- | --- | --- |
| `mss32.c` | Miles Sound System 6.0 / 6.5 (`mss32.dll`): samples, 3D samples, streams (WAV, MP3), MIDI sequences through DLS | FFmpeg (libavformat, libavcodec, libswresample) for streams; `midi.c` for MIDI (optional: without it sequences are silent) |
| `midi.c` | MIDI files through DLS collections, for Miles | `third_party/dmusic` |
| `dmusic7.c` | DirectMusic of DirectX 6 / 7 (LEGO Island 2's) | `third_party/dmusic` |
| `fmod.c` (+ `stb_vorbis_impl.c`) | FMOD 3 (`fmod.dll`): the 17 `FSOUND_*` calls Penguins! uses, samples (with a Microsoft ADPCM decoder) and Ogg streams | `third_party/stb_vorbis` |
| `dmusic8.c` | DirectMusic 8 (LEGO Island Xtreme Stunts'); `PORT_DMUSIC_SEARCH_DIR` names a folder whose segment files are found by name | `third_party/dmusic` |

`dmusic7.c` and `dmusic8.c` are alternatives (a port compiles one): each was written for one
game's use of its DirectMusic, and they still differ in about 1,250 lines. Merge them when a third
DirectMusic game needs it.

Build `third_party/dmusic` (GothicKit's DirectMusic with TinySoundFont, MIT) with
`-DDM_STATIC=1 -DDM_BUILD=1 -D_DM_USE_NATIVE_THREAD=1 -Ithird_party/dmusic/include
-Ithird_party/dmusic/src -Ithird_party/dmusic/vendor/TinySoundFont`, every `src/*.c` and
`src/*/*.c` except `Thread.Win32.c` and `Thread.Posix.c`, plus `vendor/TinySoundFont/tsf.c`.

Miles volume classes: a port sorts sample files into classes with `port_miles_class(path,
previous)` (0 effects, 1 is music, 2 and up its own) and sets each class's level with
`miles_set_class_gain`, for its options menu's sliders.
