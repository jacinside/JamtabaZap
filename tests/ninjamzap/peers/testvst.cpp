// A VST2 plugin for the suite, built on the same header as JamtabaZap's host. It plays a constant
// tone, and writes everything the host asks of it, and what it finds out about the host, to the file
// in JT_TEST_VST_LOG.
#include "aeffectx.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

audioMasterCallback host = nullptr;
FILE *logFile = nullptr;
const char STATE[] = "testvst-state";
float sampleRate = 0;
double phase = 0;
int blocks = 0;

void note(const char *format, long a = 0, double b = 0)
{
    if (!logFile) {
        if (const char *path = getenv("JT_TEST_VST_LOG"))
            logFile = fopen(path, "a");
    }
    if (logFile) {
        fprintf(logFile, format, a, b);
        fputc('\n', logFile);
        fflush(logFile);
    }
}

void askHost(AEffect *effect)
{
    note("hostversion %ld", host(effect, audioMasterVersion, 0, 0, nullptr, 0));

    // The canary finds out if the host writes past the length it's allowed.
    char vendor[64 + 8];
    memset(vendor, 'X', sizeof(vendor));
    host(effect, audioMasterGetVendorString, 0, 0, vendor, 0);
    note(vendor[64] == 'X' ? "vendor canary ok" : "vendor canary OVERWRITTEN");
    vendor[63] = 0;
    if (logFile)
        fprintf(logFile, "vendor %s\n", vendor);

    note("host samplerate %ld", host(effect, audioMasterGetSampleRate, 0, 0, nullptr, 0));
    note("host blocksize %ld", host(effect, audioMasterGetBlockSize, 0, 0, nullptr, 0));
}

void askTime(AEffect *effect)
{
    auto info = reinterpret_cast<VstTimeInfo *>(host(effect, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid, nullptr, 0));
    if (!info) {
        note("time none");
        return;
    }
    note("time tempo %ld %.0f", (info->flags & kVstTempoValid) ? 1 : 0, info->tempo);
    note("time signature %ld %.0f", info->timeSigNumerator, info->timeSigDenominator);
    note("time ppq %ld %.3f", (info->flags & kVstPpqPosValid) ? 1 : 0, info->ppqPos);
    note("time playing %ld", (info->flags & kVstTransportPlaying) ? 1 : 0);
    note("time samplerate %ld %.0f", 0, info->sampleRate);
}

VstIntPtr dispatcher(AEffect *effect, int opcode, int index, VstIntPtr value, void *ptr, float opt)
{
    switch (opcode) {
    case effOpen:
        note("open");
        askHost(effect);
        return 0;
    case effClose:
        note("close");
        return 0;
    case effSetSampleRate:
        sampleRate = opt;
        note("samplerate %ld", static_cast<long>(opt));
        return 0;
    case effSetBlockSize:
        note("blocksize %ld", static_cast<long>(value));
        return 0;
    case effMainsChanged:
        note("mains %ld", static_cast<long>(value));
        return 0;
    case effStartProcess:
        note("startprocess");
        return 0;
    case effStopProcess:
        note("stopprocess");
        return 0;
    case effGetEffectName:
        strcpy(static_cast<char *>(ptr), "TestVst");
        return 1;
    case effGetVendorString:
        strcpy(static_cast<char *>(ptr), "NinjamZap Tests");
        return 1;
    case effGetVstVersion:
        return 2400;
    case effCanDo:
        note(strcmp(static_cast<char *>(ptr), "receiveVstMidiEvent") == 0 ? "cando receive midi" : "cando other");
        return strcmp(static_cast<char *>(ptr), "receiveVstMidiEvent") == 0 ? 1 : 0;
    case effGetChunk:
        *static_cast<const void **>(ptr) = STATE;
        note("getchunk");
        return sizeof(STATE);
    case effSetChunk:
        note(value == sizeof(STATE) && memcmp(ptr, STATE, sizeof(STATE)) == 0 ? "setchunk same" : "setchunk DIFFERENT");
        return 0;
    case effProcessEvents: {
        auto events = static_cast<VstEvents *>(ptr);
        for (int e = 0; events && e < events->numEvents; ++e) {
            auto event = reinterpret_cast<VstMidiEvent *>(events->events[e]);
            if (event->type == kVstMidiType)
                note("midi %ld %.0f", event->midiData[0], static_cast<double>(event->midiData[1]));
        }
        return 1;
    }
    default:
        return 0;
    }
}

void processReplacing(AEffect *effect, float **, float **outputs, int frames)
{
    if (++blocks % 200 == 1)
        askTime(effect);
    const double step = 2 * M_PI * 440 / (sampleRate > 0 ? sampleRate : 48000);
    for (int f = 0; f < frames; ++f) {
        const float sample = static_cast<float>(0.5 * sin(phase));
        phase += step;
        outputs[0][f] = sample;
        outputs[1][f] = sample;
    }
}

} // namespace

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback callback)
{
    host = callback;
    static AEffect effect;
    memset(&effect, 0, sizeof(effect));
    effect.magic = kEffectMagic;
    effect.dispatcher = dispatcher;
    effect.processReplacing = processReplacing;
    effect.numInputs = 2;
    effect.numOutputs = 2;
    effect.flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    effect.uniqueID = 0x4a54564c;
    return &effect;
}
