// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef JT_AUDIOEFFECTX_H
#define JT_AUDIOEFFECTX_H

// A small VST2 plugin base on top of FST (src/third_party/fst), a free implementation of the
// interface. It has what JamTabaVSTPlugin uses and nothing else.

#include <cstddef>
#include <cstring>

#include "aeffectx.h"
#include "aeffeditor.h"

enum { kVstMaxEffectNameLen = 32 };

inline void vst_strncpy(char *destination, const char *source, size_t maxLength)
{
    strncpy(destination, source, maxLength);
    destination[maxLength] = 0;
}

class AudioEffectX
{
public:
    AudioEffectX(audioMasterCallback audioMaster, VstInt32 programs, VstInt32 parameters);
    virtual ~AudioEffectX();

    AEffect *getAeffect() { return &effect; }
    AEffEditor *getEditor() const { return editor; }

    bool sizeWindow(VstInt32 width, VstInt32 height);

    virtual void processReplacing(float **inputs, float **outputs, VstInt32 sampleFrames) = 0;

    virtual void open() {}
    virtual void close() {}
    virtual void suspend() {}
    virtual void resume() {}
    virtual void setSampleRate(float rate) { sampleRate = rate; }
    virtual void setBlockSize(VstInt32 size) { blockSize = size; }

    virtual VstInt32 canDo(char *) { return 0; }
    virtual VstInt32 fxIdle() { return 0; }
    virtual bool needIdle() { return false; }
    virtual bool getEffectName(char *) { return false; }
    virtual bool getVendorString(char *) { return false; }
    virtual bool getProductString(char *) { return false; }
    virtual VstInt32 getVendorVersion() { return 0; }
    virtual VstInt32 getNumMidiInputChannels() { return 0; }
    virtual VstInt32 getNumMidiOutputChannels() { return 0; }
    virtual VstPlugCategory getPlugCategory() { return kPlugCategEffect; }

protected:
    void setNumInputs(VstInt32 inputs) { effect.numInputs = inputs; }
    void setNumOutputs(VstInt32 outputs) { effect.numOutputs = outputs; }
    void setUniqueID(VstInt32 id) { effect.uniqueID = id; }
    void setEditor(AEffEditor *newEditor);
    void canProcessReplacing(bool state);
    void programsAreChunks(bool state);
    void canDoubleReplacing(bool) {} // only single precision here

    VstTimeInfo *getTimeInfo(VstInt32 filter);
    bool getHostProductString(char *text);

    audioMasterCallback audioMaster;
    AEffEditor *editor = nullptr;
    float sampleRate = 44100.f;
    VstInt32 blockSize = 1024;

private:
    VstIntPtr dispatch(VstInt32 opcode, VstInt32 index, VstIntPtr value, void *ptr, float opt);
    void setFlag(int flag, bool state);

    static VstIntPtr VSTCALLBACK dispatchEffect(AEffect *, int opcode, int index, VstIntPtr value, void *ptr, float opt);
    static void VSTCALLBACK processEffect(AEffect *, float **inputs, float **outputs, int sampleFrames);
    static void VSTCALLBACK processSilence(AEffect *, float **, float **, int) {}
    static void VSTCALLBACK setParameterEffect(AEffect *, int, float) {}
    static float VSTCALLBACK getParameterEffect(AEffect *, int) { return 0.f; }

    AEffect effect;
    char programName[kVstMaxProgNameLen + 1] = "Default";
};

typedef AudioEffectX AudioEffect;

#endif
