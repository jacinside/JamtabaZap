// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PresenceReporter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>
#include <QDebug>

using login::PresenceReporter;

namespace {
const QString PRESENCE_URL = QStringLiteral("https://api.ninjamzap.com");
const int HEARTBEAT_MS = 2 * 60 * 1000; // the backend forgets a presence after 3 min without news
}

PresenceReporter::PresenceReporter(QObject *parent) :
    QObject(parent),
    heartbeat(this),
    sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    // Automation never reports to the real backend, only to the one it's given.
    const QString testUrl = qEnvironmentVariable("JT_TEST_PRESENCE_URL");
    if (!testUrl.isEmpty())
        baseUrl = testUrl;
    else if (!qEnvironmentVariableIsSet("JT_TEST_CONTROL"))
        baseUrl = PRESENCE_URL;

    const int testHeartbeat = qEnvironmentVariableIntValue("JT_TEST_PRESENCE_HEARTBEAT_MS");
    heartbeat.setInterval(testHeartbeat > 0 ? testHeartbeat : HEARTBEAT_MS);
    connect(&heartbeat, &QTimer::timeout, this, [this]() { post("/presence/connected", true); });
}

void PresenceReporter::connected(const QString &host, quint16 port, const QString &userName)
{
    if (!this->host.isEmpty())
        disconnected();

    this->host = host;
    this->port = port;
    this->userName = userName;
    post("/presence/connected", true);
    heartbeat.start();
}

void PresenceReporter::disconnected()
{
    if (host.isEmpty())
        return;

    heartbeat.stop();
    post("/presence/disconnected", false);
    host.clear();
}

void PresenceReporter::post(const QString &path, bool withSession)
{
    if (baseUrl.isEmpty() || host.isEmpty() || userName.isEmpty())
        return;

    QJsonObject body {
        { "host", host },
        { "port", int(port) },
        { "ninjamUsername", userName },
    };
    if (withSession) { // the disconnect takes only the three above: the backend refuses anything else
        body["sessionId"] = sessionId;
        body["platform"] = QStringLiteral("jamtaba");
        body["formFactor"] = QStringLiteral("desktop");
    }

    if (!http) {
        http = new QNetworkAccessManager(this);
        connect(http, &QNetworkAccessManager::finished, this, [](QNetworkReply *reply) {
            if (reply->error() != QNetworkReply::NoError)
                qWarning() << "presence report failed:" << reply->url().path() << reply->errorString();
            reply->deleteLater();
        });
    }

    QNetworkRequest request(QUrl(baseUrl + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JamtabaZap"));
    http->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}
