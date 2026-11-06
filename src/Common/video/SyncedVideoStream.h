#ifndef SYNCED_VIDEO_STREAM_H
#define SYNCED_VIDEO_STREAM_H

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QMap>
#include <QObject>
#include <QThreadPool>
#include <atomic>
#include <memory>
#include <ninjam/Ninjam.h>

class SyncedVideoDecoder;

// What one audio channel of a user is playing, read when the audio intervals swap.
struct AudioIntervalState {
    bool playing = false;
    ninjam::MessageGuid current;
    ninjam::MessageGuid previous;
    QList<ninjam::MessageGuid> queued;
    bool voiceChat = false;
};

// The receiving side of synchronized video (ninjamzap-core docs/VIDEO_SYNC.md, sections 6 and 7),
// for one remote user: gathers the video intervals as they arrive, matches each one to the audio
// through the GUID in its marker, and decodes frames at the pace they're shown, so an interval of
// HD video doesn't sit in memory as images.
class SyncedVideoStream : public QObject
{
    Q_OBJECT

public:
    explicit SyncedVideoStream(QObject *parent = nullptr);
    ~SyncedVideoStream();

    void beginInterval(const ninjam::MessageGuid &guid);
    void addData(const ninjam::MessageGuid &guid, const QByteArray &data, bool isLastPart);

    // At each local interval boundary, with the state of the user's audio channels.
    void swap(const QList<AudioIntervalState> &audio);

    // From the UI timer. progress: position in the local interval, 0..1. liveDelayMs: how long
    // the user's voice chat audio waits before it's heard, to show live video in step with it.
    void tick(double progress, int liveDelayMs);

    bool isPlaying() const { return static_cast<bool>(playing); }

    void setDisplayWidth(int width);

signals:
    void frameDecoded(const QImage &frame);

private:
    struct Interval {
        ninjam::MessageGuid guid;
        ninjam::MessageGuid audioGuid;
        bool hasMarker = false;
        bool ended = false;
        QByteArray pending;     // bytes of a chunk still arriving
        int chunkIndex = 0;
        QByteArray parameterSets;
        QList<QByteArray> frames;
        QList<qint64> arrivals;    // when each frame arrived, for live intervals
        bool live = false;
        int holds = 0;
    };
    using IntervalPtr = std::shared_ptr<Interval>;

    void parse(Interval &interval);
    void play(const IntervalPtr &interval, const char *match);
    void playIfLive();
    bool audioIsLive = false;

    QMap<ninjam::MessageGuid, IntervalPtr> arriving;
    IntervalPtr next;
    IntervalPtr pendingPlay;
    QList<IntervalPtr> awaiting;
    IntervalPtr playing;
    int framesRequested = 0;
    std::shared_ptr<std::atomic_int> framesDecoded;
    std::shared_ptr<std::atomic_int> framesShown;
    qint64 lastTick = 0;
    qint64 worstTickGap = 0;
    int lastFrameCount = 0;

    std::shared_ptr<SyncedVideoDecoder> decoder;
    QThreadPool decodingThread;
};

#endif
