#ifndef PRESENCE_REPORTER_H
#define PRESENCE_REPORTER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QTimer>

namespace login {

// Tells the NinjamZap backend which room we're in, as a guest, the way the NinjamZap app does:
// it lights up our presence in the rooms list and notifies the people subscribed to the room.
class PresenceReporter : public QObject
{
    Q_OBJECT

public:
    explicit PresenceReporter(QObject *parent = nullptr);

    void connected(const QString &host, quint16 port, const QString &userName);
    void disconnected();

private:
    void post(const QString &path, bool withSession);

    QNetworkAccessManager *http = nullptr; // made on the first report: none at all when nothing is reported
    QTimer heartbeat;
    QString baseUrl;
    QString sessionId;
    QString host;
    quint16 port = 0;
    QString userName;
};

} // namespace login

#endif // PRESENCE_REPORTER_H
