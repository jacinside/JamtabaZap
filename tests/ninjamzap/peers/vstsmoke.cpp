// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
// Loads the VST2 plugin the way a DAW does, on real Windows: checks what it tells the host, opens
// its editor, runs audio through it for a while and closes it. Exits non-zero at the first failure.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#define FST_DONT_DEPRECATE_UNKNOWN
#include "fst.h"

static VstTimeInfo timeInfo;
static int hostWidth = 0;

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok)
        ExitProcess(1);
}

static VstIntPtr VSTCALLBACK host(AEffect *, int opcode, int index, VstIntPtr, void *ptr, float)
{
    switch (opcode) {
    case audioMasterVersion:
        return 2400;
    case audioMasterGetTime:
        return reinterpret_cast<VstIntPtr>(&timeInfo);
    case audioMasterGetProductString:
        strcpy(static_cast<char *>(ptr), "vstsmoke");
        return 1;
    case audioMasterSizeWindow:
        hostWidth = index;
        return 1;
    default:
        return 0;
    }
}

int main(int argc, char **argv)
{
    check(argc == 2, "usage: vstsmoke plugin.dll");
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll)
        printf("LoadLibrary error %lu\n", GetLastError());
    check(dll != nullptr, "the DLL loads");

    typedef AEffect *(*Main)(audioMasterCallback);
    auto pluginMain = reinterpret_cast<Main>(GetProcAddress(dll, "VSTPluginMain"));
    check(pluginMain != nullptr, "it exports VSTPluginMain");

    AEffect *effect = pluginMain(host);
    check(effect != nullptr, "it creates the effect");
    check(effect->magic == 0x56737450, "magic number");
    check(effect->uniqueID == 0x4a745a70, "its own unique id, not JamTaba's");
    check(effect->numInputs == 4 && effect->numOutputs == 2, "4 inputs, 2 outputs");
    check((effect->flags & effFlagsCanReplacing) != 0, "processes replacing");
    check((effect->flags & effFlagsHasEditor) != 0, "has an editor");

    auto call = [effect](int opcode, int index, VstIntPtr value, void *ptr, float opt) {
        return effect->dispatcher(effect, opcode, index, value, ptr, opt);
    };

    char text[256] = {0};
    check(call(effGetEffectName, 0, 0, text, 0) == 1 && !strcmp(text, "JamtabaZap"), "named JamtabaZap");
    check(call(effGetVstVersion, 0, 0, nullptr, 0) == 2400, "VST version 2400");
    check(call(effCanDo, 0, 0, const_cast<char *>("receiveVstTimeInfo"), 0) == 1, "wants the host's time");

    call(effOpen, 0, 0, nullptr, 0);
    call(effSetSampleRate, 0, 0, nullptr, 48000.f);
    call(effSetBlockSize, 0, 256, nullptr, 0);
    call(effMainsChanged, 0, 1, nullptr, 0);

    HWND window = CreateWindowA("STATIC", "vstsmoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 1000, 700,
                                nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    check(window != nullptr, "the host window exists");
    check(call(effEditOpen, 0, 0, window, 0) == 1, "the editor opens");

    ERect *rect = nullptr;
    check(call(effEditGetRect, 0, 0, &rect, 0) == 1 && rect, "the editor has a size");
    check(rect->right - rect->left > 100 && rect->bottom - rect->top > 100, "and it isn't empty");

    static float in[4][256], out[2][256];
    float *inputs[4] = {in[0], in[1], in[2], in[3]};
    float *outputs[2] = {out[0], out[1]};
    timeInfo.sampleRate = 48000;
    timeInfo.tempo = 120;

    double phase = 0;
    const DWORD start = GetTickCount();
    int blocks = 0;
    while (GetTickCount() - start < 15000) {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        for (int i = 0; i < 256; ++i) {
            in[0][i] = in[1][i] = 0.25f * static_cast<float>(sin(phase));
            phase += 2 * 3.14159265 * 440 / 48000;
        }
        effect->processReplacing(effect, inputs, outputs, 256);
        call(effEditIdle, 0, 0, nullptr, 0);
        call(effIdle, 0, 0, nullptr, 0);
        ++blocks;
        Sleep(5);
    }
    printf("processed %d blocks\n", blocks);
    check(blocks > 100, "audio ran with the editor open");

    call(effEditClose, 0, 0, nullptr, 0);
    check(IsWindow(window) != 0, "the host window survives the editor closing");
    call(effMainsChanged, 0, 0, nullptr, 0);
    call(effClose, 0, 0, nullptr, 0);
    printf("SMOKE OK\n");
    fflush(stdout);
    ExitProcess(0); // unloading Qt from a process that's ending proves nothing
}
