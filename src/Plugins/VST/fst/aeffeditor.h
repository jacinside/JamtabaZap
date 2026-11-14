// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef JT_AEFFEDITOR_H
#define JT_AEFFEDITOR_H

#include "aeffectx.h"

// The plugin's editor window, as the host sees it.
class AEffEditor
{
public:
    virtual ~AEffEditor() {}

    virtual bool getRect(ERect **rect) { *rect = nullptr; return false; }
    virtual bool open(void *window) { systemWindow = window; return true; }
    virtual void close() { systemWindow = nullptr; }
    virtual bool isOpen() { return systemWindow != nullptr; }
    virtual void idle() {}

protected:
    void *systemWindow = nullptr;
};

#endif
