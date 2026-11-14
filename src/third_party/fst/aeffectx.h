// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
// JamTaba includes the VST2 interface by this name. FST is a free implementation of it, written
// without Steinberg's SDK: https://git.iem.at/zmoelnig/FST (GPL 3 or later).
#ifndef JT_FST_AEFFECTX_H
#define JT_FST_AEFFECTX_H

#define FST_DONT_DEPRECATE_UNKNOWN
#include "fst.h"

// Not defined by FST.
enum {
    kVstMidiEventIsRealtime = 1
};

#endif
