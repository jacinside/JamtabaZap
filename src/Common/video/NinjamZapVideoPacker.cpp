#include "NinjamZapVideoPacker.h"

namespace {

void appendBE32(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

void appendBE16(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

// Index of the next 00 00 01 at or after `from`, or -1.
int nextStartCode(const QByteArray &data, int from)
{
    for (int i = from; i + 3 <= data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return i;
    }
    return -1;
}

}

namespace NinjamZapVideoPacker {

EncodedFrame fromAnnexB(const QByteArray &annexB)
{
    EncodedFrame frame;

    int start = nextStartCode(annexB, 0);
    while (start >= 0) {
        const int nalBegin = start + 3;
        const int next = nextStartCode(annexB, nalBegin);
        int nalEnd = next >= 0 ? next : annexB.size();
        if (next >= 0) {
            // the zero byte of a 4-byte start code belongs to the next one
            while (nalEnd > nalBegin && annexB[nalEnd - 1] == 0)
                --nalEnd;
        }

        if (nalEnd > nalBegin) {
            const QByteArray nal = annexB.mid(nalBegin, nalEnd - nalBegin);
            const int type = nal[0] & 0x1F;
            if (type == 7) {
                frame.sps = nal;
            } else if (type == 8) {
                frame.pps = nal;
            } else if (type >= 1 && type <= 5) {
                frame.idr = frame.idr || type == 5;
                appendBE32(frame.avcc, static_cast<quint32>(nal.size()));
                frame.avcc.append(nal);
            }
        }
        start = next;
    }

    return frame;
}

QByteArray syncMarker(quint32 intervalCounter, const ninjam::MessageGuid &audioGuid)
{
    QByteArray chunk;
    appendBE32(chunk, 20);
    appendBE32(chunk, intervalCounter);
    chunk.append(audioGuid.data(), static_cast<int>(audioGuid.size()));
    return chunk;
}

QByteArray parameterSets(const QByteArray &sps, const QByteArray &pps)
{
    QByteArray inner;
    appendBE16(inner, static_cast<quint16>(sps.size()));
    inner.append(sps);
    appendBE16(inner, static_cast<quint16>(pps.size()));
    inner.append(pps);

    QByteArray chunk;
    appendBE32(chunk, static_cast<quint32>(inner.size()));
    chunk.append(inner);
    return chunk;
}

QByteArray frameChunk(const QByteArray &avcc)
{
    QByteArray chunk;
    appendBE32(chunk, static_cast<quint32>(avcc.size()));
    chunk.append(avcc);
    return chunk;
}

QList<QByteArray> fragments(const QByteArray &chunk, int maxSize)
{
    QList<QByteArray> pieces;
    for (int offset = 0; offset < chunk.size(); offset += maxSize)
        pieces.append(chunk.mid(offset, maxSize));
    return pieces;
}

}
