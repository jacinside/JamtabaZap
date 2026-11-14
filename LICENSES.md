# Licenses

JamtabaZap is a fork of JamTaba with synchronized video for NinjamZap rooms. Its code comes from three sources, each with its own terms.

## JamtabaZap's own code: GPL 3 or later

The files written for JamtabaZap carry a header like this:

```
// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
```

They are licensed under the GNU General Public License, version 3 or (at your option) any later version. The full text is in [`LICENSES/GPL-3.0-or-later.txt`](LICENSES/GPL-3.0-or-later.txt).

These files are:

- Synchronized video: `src/Common/video/` (H.264 packing in the NinjamZap wire format, the receiving side that matches video to audio intervals, quality presets)
- Room presence for the NinjamZap backend: `src/Common/loginserver/PresenceReporter.*`
- The VST2 plugin base built on FST: `src/Plugins/VST/fst/`, and the `src/third_party/fst/aeffectx.h` wrapper
- Tests: `tests/ninjamzap/`
- Build and packaging: `.github/workflows/jamtabazap.yml`, `.github/scripts/`

The changes JamtabaZap makes inside files inherited from JamTaba are offered under the same license, to the extent they can be separated from the code around them. That code remains under its original terms (see below).

The video follows the public NinjamZap specification in [ninjamzap-core](https://github.com/jacinside/ninjamzap-core), which is also GPL 3.

## Code inherited from JamTaba

All other code belongs to its authors: Elieser de Jesus and the JamTaba contributors, and defterai for the version this fork builds on. The upstream repository has no license file ([issue #1210](https://github.com/elieserdejesus/JamTaba/issues/1210)), and this fork does not add one or change its terms. Nothing here grants rights to that code.

The Steinberg VST2 SDK files that came with JamTaba have been removed from this fork. They remain in the history inherited from the original repository.

## Third-party components

| Component | Where | License |
|---|---|---|
| FST (free VST2 interface) | `src/third_party/fst/fst.h` | GPL 3 or later |
| FFmpeg, built with `--enable-gpl` and libx264 | linked in the release builds | GPL 2 or later, as built |
| x264 | linked in the release builds | GPL 2 or later |
| Qt 5.15 | linked in the release builds | LGPL 3 (open-source edition) |
| RtMidi | `src/Libs/RtMidi` | MIT-style (see the header of `RtMidi.h`) |
| SingleApplication | `src/Libs/SingleApplication` | MIT (`LICENSE` in that folder) |
| StackWalker | `src/Common/log/stackwalker` | BSD 3-Clause (`LICENSE` in that folder) |
| Apple AU SDK sample code | `AU_SDK` | Apple's sample code license (in the file headers) |
| PortAudio, libogg, libvorbis, miniupnpc, minimp3 | from the build images | their upstream licenses; not re-checked against the exact versions used |

Because FST, FFmpeg and x264 are under the GPL, the release builds are subject to the GPL.

## Open questions

- The inherited JamTaba code has no license, so the terms for redistributing the complete application, as source or as builds, are not settled. JamtabaZap's own code is licensed as described above either way.
- The license versions of the libraries that come from the build images have not been checked one by one.

## Name and logo

The NinjamZap name and logo, and the NinjamZap rooms service, are not covered by any license in this repository.

This page describes the licensing as we understand it. It is not legal advice.
