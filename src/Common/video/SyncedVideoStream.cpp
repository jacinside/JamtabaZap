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
}

namespace {

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

        auto codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        context = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!context)
            return;
        context->thread_count = 1;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
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
            qWarning() << "Error decoding video:" << av_error_to_qt_string(ret);
            return QImage();
        }

        QImage image;
        while (avcodec_receive_frame(context, decoded) == 0) {
            if (wantImage)
                image = toImage();
        }
        return image;
    }

private:
    QImage toImage()
    {
        // Converting straight to the size on screen is much cheaper than scaling the image afterwards.
        const int shownWidth = displayWidth.load();
        const int width = qBound(160, qMin(shownWidth > 0 ? shownWidth : MAX_DECODED_WIDTH, decoded->width), MAX_DECODED_WIDTH) & ~1;
        const int height = (decoded->height * width / qMax(decoded->width, 1)) & ~1;
        if (width <= 0 || height <= 0)
            return QImage();

        sws = sws_getCachedContext(sws, decoded->width, decoded->height, static_cast<AVPixelFormat>(decoded->format),
                                   width, height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws)
            return QImage();

        QImage image(width, height, QImage::Format_RGB888);
        uint8_t *planes[4] = { image.bits(), nullptr, nullptr, nullptr };
        int strides[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
        sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height, planes, strides);
        return image;
    }

    void close()
    {
        if (context)
            avcodec_free_context(&context);
        if (decoded)
            av_frame_free(&decoded);
        if (sws)
            sws_freeContext(sws);
        context = nullptr;
        decoded = nullptr;
        sws = nullptr;
    }

    AVCodecContext *context = nullptr;
    AVFrame *decoded = nullptr;
    SwsContext *sws = nullptr;
    QByteArray currentParameterSets;
    QByteArray headers;
    bool sendHeaders = false;
    std::atomic_int displayWidth { 0 };
};

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
