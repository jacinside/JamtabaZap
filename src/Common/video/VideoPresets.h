// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VIDEO_PRESETS_H
#define VIDEO_PRESETS_H

#include <QSize>

// The same levels the NinjamZap apps offer.
struct VideoPreset {
    const char *name;
    QSize size;
    int fps;
    int bitrate;
};

namespace VideoPresets {

enum { SD = 0, VGA = 1, HD = 2, Count = 3, Default = VGA };

inline const VideoPreset &get(int index)
{
    static const VideoPreset presets[Count] = {
        { "SD",  QSize(320, 240),  10, 50000 },
        { "VGA", QSize(640, 480),  15, 200000 },
        { "HD",  QSize(1280, 720), 24, 1200000 },
    };
    return presets[(index >= 0 && index < Count) ? index : Default];
}

}

#endif
