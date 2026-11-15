#ifndef FFMPEGMUXER_H
#define FFMPEGMUXER_H

#include <QObject>
#include <QImage>
#include <QSize>
#include <QFile>
#include <QDebug>
#include <QThreadPool>
#include <QMutex>
#include <atomic>

#include "FFMpegCommon.h"

#include <memory>

// adapted from FFMpeg muxing.c example

class FFMpegMuxer : public QObject
{
    Q_OBJECT

public:
    FFMpegMuxer(QObject *parent = nullptr);
    ~FFMpegMuxer();

    void finish();

    // One encoder for the whole session, an IDR on request and no frame reordering, as
    // synchronized video needs. dataEncoded's second parameter then means "key frame".
    void setContinuousMode(bool continuous);
    void requestKeyFrame();

    // Opens the encoder again before the next frame, with the current size, rate and bitrate.
    void restart();
    void setVideoBitRate(uint bitRate);
    int takeSkippedFrames() { return skippedFrames.exchange(0); }
    QString takeTimingReport(); // where the time per frame went since the last call, for the log
    void addCaptureTime(qint64 microseconds); // the camera frame's pick, scale and preview, on the main thread

    void encodeImage(const QImage &image, bool async = true);
    void encodeAudioFrame();

    void setVideoResolution(const QSize &resolution);
    QSize getVideoResolution() const;

    void setVideoFrameRate(qreal frameRate);

    enum VideoQuality
    {
        VideoQualityLow    = 64000,     // 64 kbps
        VideoQualityMedium = 96000,     // 96 kbps
        VideoQualityHigh   = 128000,    // 128 kbps
        VideoQualityBest   = 400000     // 400 kbps (default value in ffmpeg examples)
    };


    /**
     * @brief Set video bitrate
     * @param quality - Pre-defined bit rate values in VideoQuality enum
     */
    void setVideoQuality(VideoQuality quality);

    int64_t getCurrentVideoPresentationTimeStamp() const;

signals:
    void dataEncoded(const QByteArray &data, bool isFirstPacket);
    void encodingFinished();

public slots:
    void startNewInterval();

private:

    bool finishCurrentInterval();
    bool prepareToEncodeNewInterval();

    bool addVideoStream(AVCodecID codecID, AVDictionary **opts);

    bool openVideoCodec(AVCodec *codec, AVDictionary **opts);
    void openAudioCodec(AVCodec *codec);

    bool doEncodeVideoFrame(const QImage &image);
    bool doEncodeAudioFrame(); // TODO add a SamplesBuffer parameter

    AVFrame *allocAudioFrame(enum AVSampleFormat sampleFormat, uint64_t channelLayout, int sampleRate, int nbSamples);
    AVFrame *allocPicture(enum AVPixelFormat pixelFormat, int width, int height);
    void imageToYuvPicture(const QImage &image, AVFrame *picture, int width, int height);
    void fillFrameWithImageData(const QImage &image);
    void RGBtoYUV420P(const uint8_t *bufferRGB, uint8_t *bufferYUV, uint rgbIncrement, bool swapRGB, int width, int height);

    void initialize();

    bool encodeVideo;
    bool encodeAudio;

    int64_t videoPts; // pts (presentation time stamp) of the next frame that will be generated
    quint64 encodedFrames;

    // internal streams
    class VideoOutputStream;
    class AudioOutputStream;

    //std::unique_ptr<VideoOutputStream> videoStream;
    std::unique_ptr<AudioOutputStream> audioStream;

    AVCodec *codec;
    AVCodecContext *codecContext;
    AVFrame *frame;
    AVFrame *tempFrame;
    SwsContext *swsContext;

    QSize videoResolution;
    qreal videoFrameRate;
    uint videoBitRate;

    bool initialized;
    bool startNewIntervalRequested;
    bool continuousMode;
    std::atomic_bool keyFrameRequested;
    std::atomic_int pendingFrames;
    std::atomic_int skippedFrames;
    SwsContext *rgbToYuvContext;
    static constexpr int MAX_PENDING_FRAMES = 2;

    QThreadPool threadPool;

    // Diagnostics: where each frame's time goes (see takeTimingReport)
    struct Timing {
        int submitted = 0, encoded = 0, rescaled = 0;
        qint64 convertUs = 0, convertMaxUs = 0, encodeUs = 0, encodeMaxUs = 0;
        qint64 captureUs = 0, captureMaxUs = 0, gapMaxMs = 0;
        qint64 lastSubmitMs = 0;
        int imageFormat = -1;
    } timing;
    QMutex timingMutex;
};

inline void FFMpegMuxer::setVideoBitRate(uint bitRate)
{
    this->videoBitRate = bitRate;
}

inline void FFMpegMuxer::setVideoQuality(VideoQuality quality)
{
    this->videoBitRate = static_cast<uint>(quality);
}

inline void FFMpegMuxer::setVideoFrameRate(qreal frameRate)
{
    this->videoFrameRate = frameRate;
}

inline int64_t FFMpegMuxer::getCurrentVideoPresentationTimeStamp() const
{
    return videoPts;
}

inline void FFMpegMuxer::setVideoResolution(const QSize &resolution)
{
    qDebug() << "Setting video resolution to" << resolution;

    this->videoResolution = resolution;
}

inline QSize FFMpegMuxer::getVideoResolution() const
{
    return videoResolution;
}

#endif // FFMPEGMUXER_H
