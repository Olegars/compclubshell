#include "fanrelaycontroller.h"

#include <QDebug>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtGlobal>
#include <QUrl>

QString FanRelayController::commandUrl(const QString &host, int modulePort, const QString &cmd)
{
    // W5100 web UI: http://192.168.1.4/30000/6 — port is a path segment, TCP is :80
    return QStringLiteral("http://%1/%2/%3")
        .arg(host.trimmed(), QString::number(modulePort), cmd);
}

QString FanRelayController::commandForChannel(int channel, bool on)
{
    if (channel < 1 || channel > 16)
        return {};
    const int cmd = (channel - 1) * 2 + (on ? 1 : 0);
    return QStringLiteral("%1").arg(cmd, 2, 10, QLatin1Char('0'));
}

FanRelayController::Result FanRelayController::setChannel(
    const QString &host, int modulePort, int channel, bool on, int timeoutMs)
{
    const QString cmd = commandForChannel(channel, on);
    if (cmd.isEmpty() || host.trimmed().isEmpty() || modulePort <= 0) {
        return {false, QStringLiteral("invalid relay target"), {}};
    }

    const QString url = commandUrl(host, modulePort, cmd);

    qWarning().noquote() << "[FAN-W5100] GET" << url;

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(timeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    QTimer killer;
    killer.setSingleShot(true);
    QObject::connect(&killer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    killer.start(timeoutMs);
    loop.exec();

    Result out;
    if (!reply->isFinished()) {
        reply->abort();
        out.ok = false;
        out.error = QStringLiteral("timeout");
    } else if (reply->error() != QNetworkReply::NoError) {
        out.ok = false;
        out.error = reply->errorString();
        out.body = QString::fromUtf8(reply->readAll());
    } else {
        out.ok = true;
        out.body = QString::fromUtf8(reply->readAll()).trimmed();
    }
    reply->deleteLater();
    return out;
}

FanRelayController::Result FanRelayController::readStatus(
    const QString &host, int modulePort, int timeoutMs)
{
    if (host.trimmed().isEmpty() || modulePort <= 0) {
        return {false, QStringLiteral("invalid relay target"), {}};
    }

    const QString url = commandUrl(host, modulePort, QStringLiteral("99"));

    qWarning().noquote() << "[FAN-W5100] GET" << url;

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(timeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    QTimer killer;
    killer.setSingleShot(true);
    QObject::connect(&killer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    killer.start(timeoutMs);
    loop.exec();

    Result out;
    if (!reply->isFinished()) {
        reply->abort();
        out.ok = false;
        out.error = QStringLiteral("timeout");
    } else if (reply->error() != QNetworkReply::NoError) {
        out.ok = false;
        out.error = reply->errorString();
        out.body = QString::fromUtf8(reply->readAll());
    } else {
        out.ok = true;
        out.body = QString::fromUtf8(reply->readAll()).trimmed();
    }
    reply->deleteLater();
    return out;
}

int FanRelayController::channelStateFromStatus(const QString &statusBody, int channel)
{
    if (channel < 1 || channel > 16)
        return -1;

    QString bits;
    // Ответ платы часто HTML: <center><p>ASCII:   0100000000000000
    const int asciiIdx = statusBody.indexOf(QStringLiteral("ASCII:"), 0, Qt::CaseInsensitive);
    const QString scan = asciiIdx >= 0 ? statusBody.mid(asciiIdx) : statusBody;
    for (QChar c : scan) {
        if (c == QLatin1Char('0') || c == QLatin1Char('1'))
            bits.append(c);
        if (bits.size() >= 16)
            break;
    }
    if (bits.size() < 16)
        return -1;

    // MSB = relay 1
    const QChar bit = bits.at(channel - 1);
    if (bit == QLatin1Char('1'))
        return 1;
    if (bit == QLatin1Char('0'))
        return 0;
    return -1;
}

int FanRelayController::speedFromStatus(const QString &statusBody, int channelK1, int channelK2)
{
    const int k1 = channelStateFromStatus(statusBody, channelK1);
    const int k2 = channelStateFromStatus(statusBody, channelK2);
    if (k1 < 0 || k2 < 0)
        return -1;
    if (k2 > 0)
        return 3;
    if (k1 > 0)
        return 2;
    return 1;
}

int FanRelayController::setSpeed(const QString &host, int modulePort, int channelK1, int channelK2,
                                 int speed, QString *errorOut, int timeoutMs, int softStepMs)
{
    if (channelK1 < 1 || channelK1 > 16 || channelK2 < 1 || channelK2 > 16
        || channelK1 == channelK2) {
        if (errorOut)
            *errorOut = QStringLiteral("invalid K1/K2 channels");
        return -1;
    }

    int s = speed;
    if (s <= 0)
        s = 1;
    if (s > 3)
        s = 3;

    // Safe order: drop K2 before raising K1 when leaving high; drop K1 before raising K2 for high.
    auto applyLevel = [&](int level) -> bool {
        auto applyOne = [&](int ch, bool on) -> bool {
            const QString cmd = commandForChannel(ch, on);
            qWarning().noquote()
                << "[FAN-W5100] setSpeed" << level
                << "K" << ch << (on ? "ON" : "OFF")
                << "cmd" << cmd
                << "url" << commandUrl(host, modulePort, cmd);
            const Result r = setChannel(host, modulePort, ch, on, timeoutMs);
            if (!r.ok) {
                const QString err = QStringLiteral("ch%1 %2: %3")
                                        .arg(ch)
                                        .arg(on ? QStringLiteral("ON") : QStringLiteral("OFF"))
                                        .arg(r.error);
                if (errorOut)
                    *errorOut = err;
                qWarning().noquote() << "[FAN-W5100] FAIL" << err;
                return false;
            }
            qWarning().noquote() << "[FAN-W5100] OK body=" << r.body.left(40);
            return true;
        };

        if (level == 3) {
            if (!applyOne(channelK1, false))
                return false;
            if (!applyOne(channelK2, true))
                return false;
        } else if (level == 2) {
            if (!applyOne(channelK2, false))
                return false;
            if (!applyOne(channelK1, true))
                return false;
        } else {
            if (!applyOne(channelK2, false))
                return false;
            if (!applyOne(channelK1, false))
                return false;
        }
        return true;
    };

    int current = -1;
    {
        const Result st0 = readStatus(host, modulePort, timeoutMs);
        if (st0.ok)
            current = speedFromStatus(st0.body, channelK1, channelK2);
    }

    // 50%↔100% (1↔3): короткий заход через 75%, чтобы смягчить каскад.
    if (current > 0 && qAbs(current - s) >= 2 && softStepMs > 0) {
        qWarning().noquote()
            << "[FAN-W5100] soft-step" << current << "→ 2 →" << s
            << "dwell" << softStepMs << "ms";
        if (!applyLevel(2))
            return -1;
        QEventLoop pause;
        QTimer::singleShot(softStepMs, &pause, &QEventLoop::quit);
        pause.exec();
    }

    if (!applyLevel(s))
        return -1;

    const Result st = readStatus(host, modulePort, timeoutMs);
    if (!st.ok) {
        if (errorOut && errorOut->isEmpty())
            *errorOut = st.error;
        return s; // command likely applied
    }
    const int decoded = speedFromStatus(st.body, channelK1, channelK2);
    return decoded > 0 ? decoded : s;
}
