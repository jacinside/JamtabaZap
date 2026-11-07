#include "audioeffectx.h"

AudioEffectX::AudioEffectX(audioMasterCallback audioMaster, VstInt32 programs, VstInt32 parameters) :
    audioMaster(audioMaster)
{
    memset(&effect, 0, sizeof(effect));
    effect.magic = CCONST('V', 's', 't', 'P');
    effect.dispatcher = dispatchEffect;
    effect.process = processSilence;
    effect.setParameter = setParameterEffect;
    effect.getParameter = getParameterEffect;
    effect.processReplacing = processEffect;
    effect.numPrograms = programs;
    effect.numParams = parameters;
    effect.numInputs = 1;
    effect.numOutputs = 2;
    effect.version = 1;
    effect.object = this;
}

AudioEffectX::~AudioEffectX()
{
    delete editor;
}

void AudioEffectX::setFlag(int flag, bool state)
{
    if (state)
        effect.flags |= flag;
    else
        effect.flags &= ~flag;
}

void AudioEffectX::setEditor(AEffEditor *newEditor)
{
    editor = newEditor;
    setFlag(effFlagsHasEditor, editor != nullptr);
}

void AudioEffectX::canProcessReplacing(bool state)
{
    setFlag(effFlagsCanReplacing, state);
}

void AudioEffectX::programsAreChunks(bool state)
{
    setFlag(effFlagsProgramChunks, state);
}

VstTimeInfo *AudioEffectX::getTimeInfo(VstInt32 filter)
{
    return reinterpret_cast<VstTimeInfo *>(audioMaster(&effect, audioMasterGetTime, 0, filter, nullptr, 0));
}

bool AudioEffectX::getHostProductString(char *text)
{
    text[0] = 0;
    return audioMaster(&effect, audioMasterGetProductString, 0, 0, text, 0) != 0;
}

bool AudioEffectX::sizeWindow(VstInt32 width, VstInt32 height)
{
    return audioMaster(&effect, audioMasterSizeWindow, width, height, nullptr, 0) != 0;
}

VstIntPtr AudioEffectX::dispatch(VstInt32 opcode, VstInt32 /*index*/, VstIntPtr value, void *ptr, float opt)
{
    switch (opcode) {
    case effOpen:
        open();
        return 0;
    case effClose:
        close();
        return 1;
    case effSetProgramName:
        vst_strncpy(programName, static_cast<const char *>(ptr), kVstMaxProgNameLen);
        return 0;
    case effGetProgramName:
        vst_strncpy(static_cast<char *>(ptr), programName, kVstMaxProgNameLen);
        return 0;
    case effSetSampleRate:
        setSampleRate(opt);
        return 0;
    case effSetBlockSize:
        setBlockSize(static_cast<VstInt32>(value));
        return 0;
    case effMainsChanged:
        if (value)
            resume();
        else
            suspend();
        return 0;
    case effEditGetRect:
        return editor && editor->getRect(static_cast<ERect **>(ptr)) ? 1 : 0;
    case effEditOpen:
        return editor && editor->open(ptr) ? 1 : 0;
    case effEditClose:
        if (editor)
            editor->close();
        return 0;
    case effEditIdle:
        if (editor)
            editor->idle();
        return 0;
    case effIdle:
        return fxIdle();
    case effGetPlugCategory:
        return getPlugCategory();
    case effGetEffectName:
        return getEffectName(static_cast<char *>(ptr)) ? 1 : 0;
    case effGetVendorString:
        return getVendorString(static_cast<char *>(ptr)) ? 1 : 0;
    case effGetProductString:
        return getProductString(static_cast<char *>(ptr)) ? 1 : 0;
    case effGetVendorVersion:
        return getVendorVersion();
    case effCanDo:
        return canDo(static_cast<char *>(ptr));
    case effGetVstVersion:
        return 2400;
    default:
        return 0;
    }
}

VstIntPtr VSTCALLBACK AudioEffectX::dispatchEffect(AEffect *effect, int opcode, int index, VstIntPtr value, void *ptr, float opt)
{
    auto plugin = static_cast<AudioEffectX *>(effect->object);
    if (!plugin) // the host may call it after closing
        return 0;

    const VstIntPtr result = plugin->dispatch(opcode, index, value, ptr, opt);
    if (opcode == effClose) {
        effect->object = nullptr;
        delete plugin;
    }
    return result;
}

void VSTCALLBACK AudioEffectX::processEffect(AEffect *effect, float **inputs, float **outputs, int sampleFrames)
{
    if (auto plugin = static_cast<AudioEffectX *>(effect->object))
        plugin->processReplacing(inputs, outputs, sampleFrames);
}
