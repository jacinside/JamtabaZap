// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NINJAMZAP_VIDEO_PACKER_H
#define NINJAMZAP_VIDEO_PACKER_H

#include <QByteArray>
#include <QList>
#include <ninjam/Ninjam.h>

// Builds the chunks of a synchronized video interval as described in
// ninjamzap-core docs/VIDEO_SYNC.md, section 7. Every chunk starts with a 4-byte
// big-endian length; frames carry AVCC NAL units under that wrapper.
namespace NinjamZapVideoPacker {

struct EncodedFrame {
    QByteArray sps;     // empty when the packet has none
    QByteArray pps;
    QByteArray avcc;    // VCL NAL units, each with a 4-byte big-endian length
    bool idr = false;   // has an IDR slice
};

// x264 emits Annex-B: parameter sets travel separately on the wire, and SEI / AUD are dropped.
EncodedFrame fromAnnexB(const QByteArray &annexB);

QByteArray syncMarker(quint32 intervalCounter, const ninjam::MessageGuid &audioGuid);
QByteArray parameterSets(const QByteArray &sps, const QByteArray &pps);
QByteArray frameChunk(const QByteArray &avcc);

// A chunk split in pieces that fit one UPLOAD_INTERVAL_WRITE.
QList<QByteArray> fragments(const QByteArray &chunk, int maxSize = 8192);

}

#endif
