// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TEST_AV_MARKS_H
#define TEST_AV_MARKS_H

#include <cmath>
#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QVector>
#include <algorithm>
#include <QtGlobal>

// Test mode (env JT_TEST_AVMARKS=1) to measure audio/video sync at a receiver: at the same
// positions of the interval, the frames sent turn white and the audio sent is a 1 kHz tone.
// Both come from the interval position, so any offset the receiver sees comes from the pipeline.
namespace TestAvMarks {

inline bool enabled()
{
    static const bool on = qEnvironmentVariableIsSet("JT_TEST_AVMARKS");
    return on;
}

// 200 ms marks, every 2 s. JT_TEST_AVMARKS=sparse: only at the start of each interval, to tell
// long delays from the next mark.
inline bool isMarked(qint64 intervalPosition, int sampleRate)
{
    static const bool sparse = qgetenv("JT_TEST_AVMARKS") == "sparse";
    if (sparse)
        return intervalPosition < sampleRate / 5;
    return intervalPosition % (sampleRate * 2) < sampleRate / 5;
}

inline QImage frame(const QSize &size, qint64 intervalPosition, int sampleRate)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(isMarked(intervalPosition, sampleRate) ? Qt::white : Qt::black);

    // JT_TEST_AVMARKS=detail: a moving noise pattern, for the encoder to have real work to do.
    static const bool detail = qgetenv("JT_TEST_AVMARKS") == "detail";
    if (detail && !isMarked(intervalPosition, sampleRate)) {
        static QVector<QRgb> noise;
        if (noise.isEmpty()) {
            noise.resize(1 << 18);
            quint32 seed = 12345;
            for (auto &value : noise) {
                seed = seed * 1664525u + 1013904223u;
                value = 0xFF000000u | (seed >> 8);
            }
        }
        const int shift = static_cast<int>(intervalPosition / 512);
        for (int y = 0; y < size.height(); ++y) {
            auto line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const auto from = (y * 1009 + shift * 37) % (noise.size() - size.width());
            std::copy_n(noise.constData() + from, size.width(), line);
        }
    }
    return image;
}

// Automation: when a tone starts in the output, in the same clock as the frames shown.
inline void noteOutput(float peak)
{
    static bool sounding = false;
    const bool now = peak > 0.05f;
    if (now != sounding)
        qInfo().noquote() << (now ? "JT_TEST tone" : "JT_TEST toneoff") << QDateTime::currentMSecsSinceEpoch();
    sounding = now;
}

inline void noteFrame(const QImage &frame)
{
    static bool white = false;
    const bool now = qGray(frame.pixel(frame.width() / 2, frame.height() / 2)) > 128;
    if (now && !white)
        qInfo().noquote() << "JT_TEST flash" << QDateTime::currentMSecsSinceEpoch();
    white = now;
}

inline float toneSample(qint64 intervalPosition, int sampleRate)
{
    if (!isMarked(intervalPosition, sampleRate))
        return 0.f;
    return 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * intervalPosition / sampleRate));
}

}

#endif
