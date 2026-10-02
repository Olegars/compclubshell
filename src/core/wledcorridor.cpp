#include "wledcorridor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

WledCorridor::WledCorridor(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
{
}

void WledCorridor::start(const QString &serverUrl, int computerId)
{
    m_serverUrl = serverUrl;
    m_computerId = computerId;
    if (m_computerId <= 0 || m_serverUrl.isEmpty() || !m_nam)
        return;

    const bool fresh = !m_timer || !m_timer->isActive();
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(3000);
        connect(m_timer, &QTimer::timeout, this, &WledCorridor::poll);
    }
    if (!m_timer->isActive())
        m_timer->start();
    if (fresh)
        QTimer::singleShot(0, this, &WledCorridor::poll);
}

void WledCorridor::stop()
{
    if (m_timer)
        m_timer->stop();
}

void WledCorridor::poll()
{
    if (m_pollInFlight || m_computerId <= 0 || m_serverUrl.isEmpty() || !m_nam)
        return;

    m_pollInFlight = true;
    QUrl url(m_serverUrl + QStringLiteral("/api/shell/wled/cues"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("terminal_id"), QString::number(m_computerId));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    request.setTransferTimeout(4000);
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_pollInFlight = false;
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject())
            return;
        const QJsonObject obj = doc.object();
        const int pollMs = obj.value(QStringLiteral("poll_ms")).toInt(3000);
        if (m_timer && pollMs >= 1000 && pollMs <= 60000)
            m_timer->setInterval(pollMs);
        const QJsonArray cues = obj.value(QStringLiteral("cues")).toArray();
        for (const QJsonValue &value : cues) {
            if (value.isObject())
                playCue(value.toObject());
        }
    });
}

void WledCorridor::playCue(const QJsonObject &cue)
{
    const int cueId = cue.value(QStringLiteral("id")).toInt();
    const int controllerId = cue.value(QStringLiteral("controller_id")).toInt();
    const QString host = cue.value(QStringLiteral("host")).toString().trimmed();
    const int port = cue.value(QStringLiteral("port")).toInt(80);
    const int durationMs = qBound(0, cue.value(QStringLiteral("duration_ms")).toInt(), 120000);
    const QJsonObject play = cue.value(QStringLiteral("play")).toObject();
    const QJsonObject idle = cue.value(QStringLiteral("idle")).toObject();
    const bool hasIdle = cue.value(QStringLiteral("idle")).isObject() && !idle.isEmpty();

    if (cueId <= 0 || controllerId <= 0 || host.isEmpty() || host.contains(QLatin1Char('/'))
        || port < 1 || port > 65535 || play.isEmpty()) {
        ack(cueId, false, QStringLiteral("bad cue"));
        return;
    }

    cancelRestore(controllerId);
    postWled(host, port, play, [this, cueId, controllerId, host, port, durationMs, idle, hasIdle](bool ok, const QString &error) {
        ack(cueId, ok, error);
        if (!ok) {
            qWarning() << "[WLED] коридор" << host << error;
            return;
        }
        qWarning() << "[WLED] коридор" << host << "cue" << cueId;
        if (ok && durationMs > 0 && hasIdle)
            armRestore(controllerId, host, port, idle, durationMs);
    });
}

void WledCorridor::postWled(const QString &host, int port, const QJsonObject &state,
                            const std::function<void(bool, const QString &)> &done)
{
    if (!m_nam) {
        if (done)
            done(false, QStringLiteral("no network"));
        return;
    }

    const QUrl url(QStringLiteral("http://%1:%2/json/state").arg(host).arg(port));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(1500);
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(state).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && code >= 200 && code < 300;
        if (done)
            done(ok, ok ? QString() : reply->errorString());
    });
}

void WledCorridor::ack(int cueId, bool ok, const QString &error)
{
    if (cueId <= 0 || m_computerId <= 0 || m_serverUrl.isEmpty() || !m_nam)
        return;

    QJsonObject body;
    body.insert(QStringLiteral("terminal_id"), m_computerId);
    body.insert(QStringLiteral("ok"), ok);
    if (!error.isEmpty())
        body.insert(QStringLiteral("error"), error.left(400));

    QNetworkRequest request(QUrl(m_serverUrl + QStringLiteral("/api/shell/wled/cues/")
                                 + QString::number(cueId) + QStringLiteral("/ack")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    request.setTransferTimeout(4000);
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void WledCorridor::armRestore(int controllerId, const QString &host, int port,
                              const QJsonObject &idle, int durationMs)
{
    cancelRestore(controllerId);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_restore.insert(controllerId, timer);
    connect(timer, &QTimer::timeout, this, [this, controllerId, host, port, idle, timer]() {
        if (m_restore.value(controllerId) == timer)
            m_restore.remove(controllerId);
        timer->deleteLater();
        postWled(host, port, idle, [](bool, const QString &) {});
    });
    timer->start(durationMs);
}

void WledCorridor::cancelRestore(int controllerId)
{
    if (QTimer *old = m_restore.take(controllerId)) {
        old->stop();
        old->deleteLater();
    }
}
