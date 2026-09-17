#include "chromaemulator.h"

#include <QColor>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>

namespace {
QColor chromaBgr(int v)
{
    return QColor(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff);
}

void collectInts(const QJsonValue &v, QList<QColor> *out)
{
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        for (const QJsonValue &x : a)
            collectInts(x, out);
        return;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.contains(QStringLiteral("red")) || o.contains(QStringLiteral("r"))) {
            out->append(QColor(
                o.value(QStringLiteral("red")).toInt(o.value(QStringLiteral("r")).toInt()),
                o.value(QStringLiteral("green")).toInt(o.value(QStringLiteral("g")).toInt()),
                o.value(QStringLiteral("blue")).toInt(o.value(QStringLiteral("b")).toInt())));
            return;
        }
        if (o.contains(QStringLiteral("color"))) {
            collectInts(o.value(QStringLiteral("color")), out);
            return;
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            collectInts(it.value(), out);
        return;
    }
    if (v.isDouble()) {
        const int n = v.toInt();
        if (n != 0)
            out->append(chromaBgr(n));
    }
}

QColor average(const QList<QColor> &cs)
{
    if (cs.isEmpty())
        return QColor(0, 0, 0);
    qint64 r = 0, g = 0, b = 0;
    for (const QColor &c : cs) {
        r += c.red();
        g += c.green();
        b += c.blue();
    }
    return QColor(int(r / cs.size()), int(g / cs.size()), int(b / cs.size()));
}
}

ChromaEmulator::ChromaEmulator(ReactiveLighting *hub, QObject *parent)
    : QObject(parent)
    , m_hub(hub)
{
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(8000);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        m_hasSession = false;
        if (m_hub)
            m_hub->release(QStringLiteral("chroma"));
    });
    m_server.setHandler([this](const LocalHttpRequest &req) { return onHttp(req); });
}

void ChromaEmulator::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (on) {
        if (!m_server.listen(kPort)) {
            qWarning() << "[Chroma] port" << kPort << "busy — Synapse или другой эмулятор";
        } else {
            qWarning() << "[Chroma] REST 127.0.0.1:" << kPort;
        }
    } else {
        m_watchdog.stop();
        m_hasSession = false;
        if (m_hub)
            m_hub->release(QStringLiteral("chroma"));
        m_server.close();
    }
}

LocalHttpResponse ChromaEmulator::onHttp(const LocalHttpRequest &req)
{
    LocalHttpResponse res;
    if (!m_enabled)
        return res;

    QString path = req.path;
    if (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    const QString method = req.method;

    if (path.compare(QLatin1String("/razer/chromasdk"), Qt::CaseInsensitive) == 0) {
        if (method == QLatin1String("GET")) {
            res.body = "{\"version\":\"3.20.2\",\"core\":\"3.20.2\","
                       "\"device_supported\":[\"keyboard\",\"mouse\",\"headset\","
                       "\"mousepad\",\"keypad\",\"chromalink\"]}";
            return res;
        }
        if (method == QLatin1String("POST")) {
            ++m_session;
            m_hasSession = true;
            sessionAlive();
            res.body = QStringLiteral("{\"sessionid\":%1,\"uri\":\"http://127.0.0.1:%2/chromasdk\"}")
                           .arg(m_session).arg(kPort).toUtf8();
            return res;
        }
        if (method == QLatin1String("DELETE")) {
            m_hasSession = false;
            m_watchdog.stop();
            if (m_hub)
                m_hub->release(QStringLiteral("chroma"));
            res.body = "{\"result\":0}";
            return res;
        }
    }

    if (path.startsWith(QLatin1String("/chromasdk"), Qt::CaseInsensitive)) {
        sessionAlive();
        if (method == QLatin1String("DELETE")) {
            m_hasSession = false;
            m_watchdog.stop();
            if (m_hub)
                m_hub->release(QStringLiteral("chroma"));
            res.body = "{\"result\":0}";
            return res;
        }
        if (path.endsWith(QLatin1String("/heartbeat")) || method == QLatin1String("PUT")
            || method == QLatin1String("POST")) {
            const QJsonObject root = QJsonDocument::fromJson(req.body).object();
            if (!root.isEmpty() && !path.endsWith(QLatin1String("/heartbeat")))
                applyEffect(root);
            res.body = "{\"result\":0}";
            return res;
        }
    }

    res.body = "{\"result\":0}";
    return res;
}

void ChromaEmulator::applyEffect(const QJsonObject &root)
{
    if (!m_hub)
        return;
    const QString effect = root.value(QStringLiteral("effect")).toString().toUpper();
    if (effect == QLatin1String("CHROMA_NONE") || effect == QLatin1String("CHROMA_OFF")) {
        m_hub->release(QStringLiteral("chroma"));
        return;
    }
    const QColor c = colorFromEffect(root);
    if (!c.isValid() || (c.red() + c.green() + c.blue()) < 12) {
        m_hub->release(QStringLiteral("chroma"));
        return;
    }
    m_hub->playPreset(QStringLiteral("chroma"), ReactiveLighting::Ambient,
                      QStringLiteral("chroma"),
                      ReactiveLighting::hexColor(c.red(), c.green(), c.blue()), 100);
}

QColor ChromaEmulator::colorFromEffect(const QJsonObject &root) const
{
    QList<QColor> cs;
    if (root.contains(QStringLiteral("param")))
        collectInts(root.value(QStringLiteral("param")), &cs);
    else
        collectInts(root, &cs);
    return average(cs);
}

void ChromaEmulator::sessionAlive()
{
    m_hasSession = true;
    m_watchdog.start();
}
