// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncedVideoStream.h"
#include "FFMpegCommon.h"

#include <QDateTime>
#include <QDebug>
#include <QMutex>
#include <QtConcurrent/QtConcurrent>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/pixdesc.h>
}

// Decoding on the GPU needs FFmpeg 4's hw_device_ctx API (the macOS x86_64 libs are FFmpeg 3.4).
#if LIBAVCODEC_VERSION_MAJOR >= 58
#define JT_GPU_DECODING
extern "C" {
    #include <libavutil/hwcontext.h>
}
#endif

namespace {

#ifdef JT_GPU_DECODING
// One GPU device for every stream's decoder, opened on first use; none (CPU decoding) when the
// system has none FFmpeg can use, or with JT_NO_GPU_DECODING set.
struct GpuDevice
{
    AVBufferRef *ref = nullptr;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
};

const GpuDevice &gpuDevice()
{
    static const GpuDevice device = [] {
        GpuDevice d;
        if (qEnvironmentVariableIsSet("JT_NO_GPU_DECODING")) {
            qInfo() << "Video decoding: CPU only, GPU decoding turned off (JT_NO_GPU_DECODING)";
            return d;
        }
        const AVHWDeviceType preferred[] = {
#if defined(Q_OS_WIN)
            AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2,
#elif defined(Q_OS_MAC)
            AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
            AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA,
#endif
        };
        for (auto type : preferred) {
            if (av_hwdevice_ctx_create(&d.ref, type, nullptr, nullptr, 0) == 0) {
                d.type = type;
                break;
            }
        }
        if (d.ref)
            qInfo().noquote() << "Video decoding: GPU available (" << av_hwdevice_get_type_name(d.type) << ")";
        else
            qInfo() << "Video decoding: CPU only, no GPU decoder on this system";
        return d;
    }();
    return device;
}

// The pixel format the H.264 decoder gives GPU frames in, for that device.
AVPixelFormat gpuPixelFormat(const AVCodec *codec, AVHWDeviceType type)
{
    for (int i = 0;; ++i) {
        auto config = avcodec_get_hw_config(codec, i);
        if (!config)
            return AV_PIX_FMT_NONE;
        if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return config->pix_fmt;
    }
}
#endif

constexpr int MAX_HELD_SWAPS = 4;
constexpr int MAX_AWAITING = 2;
constexpr int MAX_DECODED_WIDTH = 1280;

bool isZero(const ninjam::MessageGuid &guid)
{
    for (auto byte : guid) {
        if (byte)
            return false;
    }
    return true;
}

quint32 readBE32(const QByteArray &data, int offset = 0)
{
    auto p = reinterpret_cast<const uchar *>(data.constData()) + offset;
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
}

const QByteArray START_CODE("\x00\x00\x00\x01", 4);

// [2B SPS length][SPS][2B PPS length][PPS], with lengths that fill the chunk exactly and the right
// NAL types. A single byte isn't enough: a frame's length, like 00 00 07 80, can look like the start.
bool isParameterSets(const QByteArray &chunk)
{
    auto p = reinterpret_cast<const uchar *>(chunk.constData());
    if (chunk.size() < 6)
        return false;
    const int spsSize = (p[0] << 8) | p[1];
    if (spsSize == 0 || 2 + spsSize + 2 >= chunk.size() || (p[2] & 0x1F) != 7)
        return false;
    const int ppsSize = (p[2 + spsSize] << 8) | p[3 + spsSize];
    return ppsSize > 0 && 2 + spsSize + 2 + ppsSize == chunk.size() && (p[4 + spsSize] & 0x1F) == 8;
}

}

// H.264 decoding, always on the stream's single decoding thread.
class SyncedVideoDecoder
{
public:
    ~SyncedVideoDecoder()
    {
        close();
    }

    // parameterSets: [2B BE SPS length][SPS][2B BE PPS length][PPS]. Rebuilds only when they change.
    void setParameterSets(const QByteArray &parameterSets)
    {
        if (parameterSets == currentParameterSets && context)
            return;
        close();
        currentParameterSets = parameterSets;
        headers.clear();

        int offset = 0;
        while (offset + 2 <= parameterSets.size()) {
            auto p = reinterpret_cast<const uchar *>(parameterSets.constData()) + offset;
            const int size = (p[0] << 8) | p[1];
            if (offset + 2 + size > parameterSets.size())
                return;
            headers += START_CODE + parameterSets.mid(offset + 2, size);
            offset += 2 + size;
        }

        open();
    }

    // avcc: NAL units with 4-byte lengths. Returns an image if the frame came out and wantImage.
    void setDisplayWidth(int width) { displayWidth = width; }

    QImage decode(const QByteArray &avcc, bool wantImage)
    {
        if (!context)
            return QImage();

        QByteArray annexB;
        if (sendHeaders) {
            annexB = headers;
            sendHeaders = false;
        }
        int offset = 0;
        while (offset + 4 <= avcc.size()) {
            const int size = static_cast<int>(readBE32(avcc, offset));
            if (size <= 0 || offset + 4 + size > avcc.size())
                break;
            annexB += START_CODE + avcc.mid(offset + 4, size);
            offset += 4 + size;
        }

        AVPacket packet;
        av_init_packet(&packet);
        packet.data = reinterpret_cast<uint8_t *>(annexB.data());
        packet.size = annexB.size();

        int ret = avcodec_send_packet(context, &packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            if (onGpu())
                return fallBackToCpu(avcc, wantImage, ret);
            qWarning() << "Error decoding video:" << av_error_to_qt_string(ret);
            return QImage();
        }

        QImage image;
        while ((ret = avcodec_receive_frame(context, decoded)) == 0) {
            if (!cpuDecoder.isEmpty()) { // logged with the first frame, which brings the size
                logDecoder(context, cpuDecoder);
                cpuDecoder.clear();
            }
            if (wantImage)
                image = toImage();
        }
        if (copyError < 0)
            return fallBackToCpu(avcc, wantImage, copyError);
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && onGpu())
            return fallBackToCpu(avcc, wantImage, ret);
        return image;
    }

private:
    void open()
    {
        auto codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        context = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!context)
            return;
        context->thread_count = 1;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
#ifdef JT_GPU_DECODING
        gpuFormat = AV_PIX_FMT_NONE;
        const auto &device = gpuDevice();
        if (device.ref && !gpuFailed) {
            gpuFormat = gpuPixelFormat(codec, device.type);
            if (gpuFormat != AV_PIX_FMT_NONE) {
                context->hw_device_ctx = av_buffer_ref(device.ref);
                context->opaque = this;
                context->get_format = pickFormat;
            }
        }
        if (gpuFormat == AV_PIX_FMT_NONE && !gpuFailed)
            cpuDecoder = device.ref ? QStringLiteral("CPU, the GPU has no H.264 decoder") : QStringLiteral("CPU");
#else
        cpuDecoder = QStringLiteral("CPU (this build has no GPU decoding)");
#endif
        const int ret = avcodec_open2(context, codec, nullptr);
        if (ret < 0) {
            qCritical() << "Could not open the H.264 decoder:" << av_error_to_qt_string(ret);
            avcodec_free_context(&context);
            context = nullptr;
            return;
        }
        decoded = av_frame_alloc();
        sendHeaders = true;
    }

#ifdef JT_GPU_DECODING
    // The GPU format when the decoder offers it for this stream; otherwise (a profile or size the
    // GPU doesn't take) a CPU one, so the stream is decoded in software.
    static AVPixelFormat pickFormat(AVCodecContext *context, const AVPixelFormat *formats)
    {
        auto self = static_cast<SyncedVideoDecoder *>(context->opaque);
        for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == self->gpuFormat) {
                self->logDecoder(context, QStringLiteral("GPU (%1)").arg(av_hwdevice_get_type_name(gpuDevice().type)));
                return *format;
            }
        }
        self->gpuFormat = AV_PIX_FMT_NONE;
        self->logDecoder(context, QStringLiteral("CPU, the GPU (%1) doesn't take this stream").arg(av_hwdevice_get_type_name(gpuDevice().type)));
        for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            auto descriptor = av_pix_fmt_desc_get(*format);
            if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL))
                return *format;
        }
        return AV_PIX_FMT_NONE;
    }
#endif

    // Once per stream, and again when it changes: which decoder this stream is on.
    void logDecoder(const AVCodecContext *context, const QString &decoder)
    {
        if (decoder == loggedDecoder)
            return;
        loggedDecoder = decoder;
        qInfo().noquote() << QStringLiteral("Video stream %1 (%2x%3): decoding on the %4")
                             .arg(id).arg(context->width).arg(context->height).arg(decoder);
    }

    bool onGpu() const
    {
#ifdef JT_GPU_DECODING
        return gpuFormat != AV_PIX_FMT_NONE;
#else
        return false;
#endif
    }

    // The GPU failed this stream: decode it on the CPU from now on, starting again with this frame.
    QImage fallBackToCpu(const QByteArray &avcc, bool wantImage, int error)
    {
        qWarning().noquote() << QStringLiteral("Video stream %1: GPU video decoding failed (%2), decoding on the CPU from now on")
                                .arg(id).arg(av_error_to_qt_string(error));
        loggedDecoder = QStringLiteral("CPU, after the GPU failed");
        gpuFailed = true;
        copyError = 0;
        close();
        open();
        return decode(avcc, wantImage);
    }

    // The decoded frame in memory: GPU frames are copied back only when they are shown.
    const AVFrame *frameInMemory()
    {
#ifdef JT_GPU_DECODING
        if (decoded->format == gpuFormat && gpuFormat != AV_PIX_FMT_NONE) {
            if (!inMemory)
                inMemory = av_frame_alloc();
            av_frame_unref(inMemory);
            const int ret = av_hwframe_transfer_data(inMemory, decoded, 0);
            if (ret < 0) {
                copyError = ret;
                return nullptr;
            }
            return inMemory;
        }
#endif
        return decoded;
    }

    QImage toImage()
    {
        auto frame = frameInMemory();
        if (!frame)
            return QImage();

        // Converting straight to the size on screen is much cheaper than scaling the image afterwards.
        const int shownWidth = displayWidth.load();
        const int width = qBound(160, qMin(shownWidth > 0 ? shownWidth : MAX_DECODED_WIDTH, frame->width), MAX_DECODED_WIDTH) & ~1;
        const int height = (frame->height * width / qMax(frame->width, 1)) & ~1;
        if (width <= 0 || height <= 0)
            return QImage();

        sws = sws_getCachedContext(sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                   width, height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws)
            return QImage();

        QImage image(width, height, QImage::Format_RGB888);
        uint8_t *planes[4] = { image.bits(), nullptr, nullptr, nullptr };
        int strides[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides);
        return image;
    }

    void close()
    {
        if (context)
            avcodec_free_context(&context);
        if (decoded)
            av_frame_free(&decoded);
        if (inMemory)
            av_frame_free(&inMemory);
        if (sws)
            sws_freeContext(sws);
        context = nullptr;
        decoded = nullptr;
        inMemory = nullptr;
        sws = nullptr;
    }

    AVCodecContext *context = nullptr;
    AVFrame *decoded = nullptr;
    AVFrame *inMemory = nullptr; // a GPU frame copied back
#ifdef JT_GPU_DECODING
    AVPixelFormat gpuFormat = AV_PIX_FMT_NONE; // none: decoding on the CPU
#endif
    bool gpuFailed = false; // for good: this stream stays on the CPU
    int copyError = 0; // copying a GPU frame back failed
    QString loggedDecoder;
    QString cpuDecoder; // to log with the first frame when it isn't on the GPU
    const int id = nextId++;
    static std::atomic_int nextId;
    SwsContext *sws = nullptr;
    QByteArray currentParameterSets;
    QByteArray headers;
    bool sendHeaders = false;
    std::atomic_int displayWidth { 0 };
};

std::atomic_int SyncedVideoDecoder::nextId { 1 };

SyncedVideoStream::SyncedVideoStream(QObject *parent) :
    QObject(parent),
    framesDecoded(std::make_shared<std::atomic_int>(0)),
    framesShown(std::make_shared<std::atomic_int>(0)),
    decoder(std::make_shared<SyncedVideoDecoder>())
{
    decodingThread.setMaxThreadCount(1);
}

SyncedVideoStream::~SyncedVideoStream()
{
    decodingThread.clear();
    decodingThread.waitForDone();
}

void SyncedVideoStream::setDisplayWidth(int width)
{
    decoder->setDisplayWidth(width);
}

void SyncedVideoStream::beginInterval(const ninjam::MessageGuid &guid)
{
    auto interval = std::make_shared<Interval>();
    interval->guid = guid;
    arriving.insert(guid, interval);

    // Two BEGINs before a swap (network stall, or boundaries a few ms apart): the one still in
    // `next` waits for its audio instead of being lost.
    if (next && next->hasMarker) {
        if (awaiting.size() >= MAX_AWAITING)
            awaiting.removeFirst();
        awaiting.append(next);
    }
    next = interval;
}

void SyncedVideoStream::addData(const ninjam::MessageGuid &guid, const QByteArray &data, bool isLastPart)
{
    auto it = arriving.find(guid);
    if (it == arriving.end())
        return;

    // The interval stays reachable by GUID wherever it moved: its late chunks aren't lost.
    auto interval = it.value();
    interval->pending += data;
    parse(*interval);
    playIfLive();

    if (isLastPart) {
        interval->ended = true;
        arriving.erase(it);
    }
}

void SyncedVideoStream::parse(Interval &interval)
{
    while (interval.pending.size() >= 4) {
        const qint64 size = readBE32(interval.pending);
        if (interval.pending.size() < 4 + size)
            return;

        const QByteArray chunk = interval.pending.mid(4, static_cast<int>(size));
        interval.pending.remove(0, 4 + static_cast<int>(size));

        if (interval.chunkIndex == 0 && chunk.size() == 20) {
            std::copy_n(chunk.constData() + 4, 16, interval.audioGuid.data());
            interval.hasMarker = true;
        } else if (isParameterSets(chunk)) {
            interval.parameterSets = chunk;
        } else if (!chunk.isEmpty()) {
            interval.frames.append(chunk);
            interval.arrivals.append(QDateTime::currentMSecsSinceEpoch());
        }
        interval.chunkIndex++;
    }
}

void SyncedVideoStream::playIfLive()
{
    // Nothing to match it to: the sender's audio is in voice chat, or it isn't sending audio.
    if (next && next->hasMarker && (audioIsLive || isZero(next->audioGuid))) {
        next->live = true;
        play(next, "LIVE");
        next.reset();
    }
}

void SyncedVideoStream::play(const IntervalPtr &interval, const char *match)
{
    if (qEnvironmentVariableIsSet("JT_TEST_CONTROL"))
        qInfo() << "JT_TEST video play match" << match << "frames" << interval->frames.size()
                << "previous" << (playing ? playing->frames.size() : 0) << "requested" << framesRequested
                << "decoded" << framesDecoded->exchange(0) << "tickgap" << worstTickGap;
    worstTickGap = 0;
    if (qEnvironmentVariableIsSet("JT_TEST_CONTROL"))
        qInfo().noquote() << "JT_TEST shown" << framesShown->exchange(0);

    if (playing)
        lastFrameCount = playing->frames.size();

    playing = interval;
    framesRequested = 0;
}

void SyncedVideoStream::swap(const QList<AudioIntervalState> &audio)
{
    audioIsLive = !audio.isEmpty() && std::all_of(audio.begin(), audio.end(), [](const AudioIntervalState &a) { return a.voiceChat; });
    playIfLive();

    const auto isCurrent = [&](const ninjam::MessageGuid &g) {
        return std::any_of(audio.begin(), audio.end(), [&](const AudioIntervalState &a) { return a.playing && a.current == g; });
    };
    const auto isPrevious = [&](const ninjam::MessageGuid &g) {
        return std::any_of(audio.begin(), audio.end(), [&](const AudioIntervalState &a) { return a.previous == g; });
    };
    const auto isQueued = [&](const ninjam::MessageGuid &g) {
        return std::any_of(audio.begin(), audio.end(), [&](const AudioIntervalState &a) { return a.queued.contains(g); });
    };

    if (pendingPlay) {
        play(pendingPlay, "DS");
        pendingPlay.reset();
    }

    for (int i = 0; i < awaiting.size();) {
        const auto interval = awaiting.at(i);
        if (isCurrent(interval->audioGuid)) {
            pendingPlay = interval;
            awaiting.removeAt(i);
        } else if (!isQueued(interval->audioGuid)) {
            awaiting.removeAt(i); // its audio left the queue without playing
        } else {
            ++i;
        }
    }

    if (!next)
        return;

    const auto guid = next->audioGuid;
    if (!next->hasMarker)
        return; // the marker is still on its way
    if (isPrevious(guid)) {
        play(next, "PREV");
        next.reset();
    } else if (isCurrent(guid)) {
        pendingPlay = next;
        next.reset();
    } else if (isQueued(guid)) {
        if (awaiting.size() >= MAX_AWAITING)
            awaiting.removeFirst();
        awaiting.append(next);
        next.reset();
    } else if (++next->holds >= MAX_HELD_SWAPS) {
        if (qEnvironmentVariableIsSet("JT_TEST_CONTROL"))
            qInfo() << "JT_TEST video drop";
        next.reset();
    }
}

void SyncedVideoStream::tick(double progress, int liveDelayMs)
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (lastTick)
        worstTickGap = qMax(worstTickGap, now - lastTick);
    lastTick = now;

    if (!playing || playing->frames.isEmpty())
        return;

    int target = 0;
    const int available = playing->frames.size();
    if (playing->live) {
        target = framesRequested;
        while (target < available && playing->arrivals.at(target) + liveDelayMs <= now)
            ++target;
    } else {
        // Evenly over the interval: by its own frame count once it's complete, until then by the last one's.
        const int expected = playing->ended ? available : qMax(lastFrameCount, available);
        target = qMin(static_cast<int>(progress * expected) + 1, available);
    }
    if (target <= framesRequested)
        return;

    auto interval = playing;
    const int from = framesRequested;
    framesRequested = target;

    auto decoderRef = decoder;
    auto decodedCount = framesDecoded;
    auto shownCount = framesShown;
    QPointer<SyncedVideoStream> self(this);
    QtConcurrent::run(&decodingThread, [=]() {
        if (!interval->parameterSets.isEmpty())
            decoderRef->setParameterSets(interval->parameterSets);
        QImage image;
        for (int i = from; i < target; ++i)
            image = decoderRef->decode(interval->frames.at(i), i == target - 1), (*decodedCount)++;
        if (!image.isNull() && self) {
            (*shownCount)++;
            emit self->frameDecoded(image);
        }
        return 0;
    });
}
