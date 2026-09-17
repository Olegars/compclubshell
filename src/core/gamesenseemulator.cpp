#include "gamesenseemulator.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace {
bool extractRgb(const QJsonValue &v, int *r, int *g, int *b)
{
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 3)
            return false;
        *r = a.at(0).toInt();
        *g = a.at(1).toInt();
        *b = a.at(2).toInt();
        return true;
    }
    if (!v.isObject())
        return false;
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("red")) || o.contains(QStringLiteral("r"))) {
        *r = o.value(QStringLiteral("red")).toInt(o.value(QStringLiteral("r")).toInt());
        *g = o.value(QStringLiteral("green")).toInt(o.value(QStringLiteral("g")).toInt());
        *b = o.value(QStringLiteral("blue")).toInt(o.value(QStringLiteral("b")).toInt());
        return true;
    }
    return false;
}

bool findColor(const QJsonObject &obj, int *r, int *g, int *b)
{
    if (obj.contains(QStringLiteral("color")) && extractRgb(obj.value(QStringLiteral("color")), r, g, b))
        return true;
    const QJsonObject frame = obj.value(QStringLiteral("frame")).toObject();
    if (!frame.isEmpty()) {
        if (extractRgb(frame.value(QStringLiteral("color")), r, g, b))
            return true;
        if (findColor(frame, r, g, b))
            return true;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().isObject() && findColor(it.value().toObject(), r, g, b))
            return true;
    }
    return false;
}
}

GameSenseEmulator::GameSenseEmulator(ReactiveLighting *hub, QObject *parent)
    : QObject(parent)
    , m_hub(hub)
{
    m_server.setHandler([this](const LocalHttpRequest &req) { return onHttp(req); });
}

void GameSenseEmulator::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (on) {
        bool ok = m_server.listen(51213);
        if (!ok)
            ok = m_server.listenEphemeral();
        if (!ok) {
            qWarning() << "[GameSense] listen failed";
            return;
        }
        const quint16 port = m_server.serverPort();
        if (!writeCoreProps(port))
            qWarning() << "[GameSense] cannot write coreProps.json";
        else
            qWarning() << "[GameSense] 127.0.0.1:" << port;
    } else {
        if (m_hub)
            m_hub->release(QStringLiteral("gamesense"));
        if (m_hub) {
            m_hub->release(QStringLiteral("gamesense.bomb"));
            m_hub->release(QStringLiteral("gamesense.win"));
            m_hub->release(QStringLiteral("gamesense.death"));
            m_hub->release(QStringLiteral("gamesense.hit"));
        }
        m_server.close();
        removeCoreProps();
    }
}

LocalHttpResponse GameSenseEmulator::onHttp(const LocalHttpRequest &req)
{
    LocalHttpResponse res;
    res.body = "{\"ok\":true}";
    if (!m_enabled)
        return res;

    QString path = req.path.toLower();
    if (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);

    if (path == QLatin1String("/reactor/light") && req.method == QLatin1String("POST")) {
        const QJsonObject root = QJsonDocument::fromJson(req.body).object();
        handleGameEvent(root);
        return res;
    }

    if (path.endsWith(QLatin1String("/game_event")) || path == QLatin1String("/game_event")
        || path == QLatin1String("/reactor/light")) {
        handleGameEvent(QJsonDocument::fromJson(req.body).object());
        return res;
    }

    return res;
}

void GameSenseEmulator::handleGameEvent(const QJsonObject &root)
{
    if (!m_hub)
        return;

    if (root.value(QStringLiteral("color")).isString()) {
        const QString c = root.value(QStringLiteral("color")).toString().trimmed();
        if (!c.isEmpty()) {
            m_hub->pulse(QStringLiteral("ingest"),
                         root.value(QStringLiteral("strobe")).toBool(false)
                             ? ReactiveLighting::Alert
                             : ReactiveLighting::Ambient,
                         c,
                         root.value(QStringLiteral("brightness")).toInt(100),
                         root.value(QStringLiteral("strobe")).toBool(false),
                         root.value(QStringLiteral("ttl_ms")).toInt(0),
                         QStringLiteral("ingest"));
            return;
        }
    }

    QJsonObject data = root.value(QStringLiteral("data")).toObject();
    if (data.isEmpty())
        data = root;

    const QString event = root.value(QStringLiteral("event")).toString().toLower();
    const QString game = root.value(QStringLiteral("game")).toString();
    const int value = data.value(QStringLiteral("value")).toInt(-1);

    int r = 0, g = 0, b = 0;
    const bool hasColor = findColor(data, &r, &g, &b) || findColor(root, &r, &g, &b);

    auto hint = [&](const QString &fallback) {
        return game.isEmpty() ? fallback : (game.toLower() + QLatin1Char(' ') + fallback);
    };

    if (event.contains(QLatin1String("bomb")) || event.contains(QLatin1String("explod"))) {
        m_hub->playPreset(QStringLiteral("gamesense.bomb"), ReactiveLighting::Alert,
                          hint(QStringLiteral("бомба")));
        return;
    }
    if (event.contains(QLatin1String("health")) && value > 0 && !hasColor) {
        m_hub->release(QStringLiteral("gamesense.death"));
        m_hub->release(QStringLiteral("gamesense"));
        return;
    }
    if (event.contains(QLatin1String("death")) || event.contains(QLatin1String("dead"))
        || (event.contains(QLatin1String("health")) && value == 0)) {
        m_hub->playPreset(QStringLiteral("gamesense.death"), ReactiveLighting::Event,
                          hint(QStringLiteral("смерть")));
        return;
    }
    if (event.contains(QLatin1String("win")) || event.contains(QLatin1String("victory"))
        || event.contains(QLatin1String("round_over"))) {
        m_hub->playPreset(QStringLiteral("gamesense.win"), ReactiveLighting::Round,
                          hint(QStringLiteral("победа")));
        return;
    }
    if (event.contains(QLatin1String("kill")) || event.contains(QLatin1String("hit"))
        || event.contains(QLatin1String("damage"))) {
        m_hub->playPreset(QStringLiteral("gamesense.hit"), ReactiveLighting::Event,
                          hint(QStringLiteral("удар")));
        return;
    }

    if (hasColor && (r + g + b) >= 12) {
        m_hub->playPreset(QStringLiteral("gamesense"), ReactiveLighting::Ambient,
                          game.isEmpty() ? QStringLiteral("gamesense") : game.toLower(),
                          ReactiveLighting::hexColor(r, g, b), 100);
        return;
    }

    if (root.contains(QStringLiteral("color"))) {
        int cr = 0, cg = 0, cb = 0;
        if (extractRgb(root.value(QStringLiteral("color")), &cr, &cg, &cb) && cr + cg + cb >= 12) {
            m_hub->pulse(QStringLiteral("gamesense"), ReactiveLighting::Ambient,
                         ReactiveLighting::hexColor(cr, cg, cb),
                         root.value(QStringLiteral("brightness")).toInt(100),
                         root.value(QStringLiteral("strobe")).toBool(false),
                         root.value(QStringLiteral("ttl_ms")).toInt(0),
                         QStringLiteral("ingest"));
        }
    }
}

bool GameSenseEmulator::writeCoreProps(quint16 port)
{
    const QByteArray json = QStringLiteral("{\"address\":\"127.0.0.1:%1\"}\n").arg(port).toUtf8();
    m_written.clear();
    bool ok = false;
    for (const QString &path : corePropsPaths()) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            qWarning() << "[GameSense] cannot write" << path << f.errorString();
            continue;
        }
        f.write(json);
        m_written.append(path);
        ok = true;
        qWarning() << "[GameSense] wrote" << path;
    }
    return ok;
}

void GameSenseEmulator::removeCoreProps()
{
    for (const QString &path : m_written) {
        if (QFile::exists(path) && !QFile::remove(path))
            qWarning() << "[GameSense] cannot remove" << path;
    }
    m_written.clear();
}

QStringList GameSenseEmulator::corePropsPaths() const
{
    QString root = qEnvironmentVariable("PROGRAMDATA");
    if (root.isEmpty())
        root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    root = QDir::fromNativeSeparators(root);
    return {
        root + QStringLiteral("/SteelSeries/SteelSeries Engine 3/coreProps.json"),
        root + QStringLiteral("/SteelSeries/GG/coreProps.json"),
    };
}
