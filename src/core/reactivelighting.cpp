#include "reactivelighting.h"
#include "chromaemulator.h"
#include "dmxcontroller.h"
#include "gamesenseemulator.h"
#include "valvegsi.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <algorithm>

ReactiveLighting::ReactiveLighting(DmxController *dmx, QObject *parent)
    : QObject(parent)
    , m_dmx(dmx)
{
    m_valve = new ValveGsi(this, this);
    m_chroma = new ChromaEmulator(this, this);
    m_sense = new GameSenseEmulator(this, this);
}

void ReactiveLighting::setListen(bool on)
{
    if (m_valve)
        m_valve->setEnabled(on);
}

void ReactiveLighting::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (on) {
        m_chroma->setEnabled(true);
        m_sense->setEnabled(true);
        setHint(QStringLiteral("ждём игру"));
    } else {
        m_chroma->setEnabled(false);
        m_sense->setEnabled(false);
        QStringList keep;
        for (auto it = m_layers.constBegin(); it != m_layers.constEnd(); ++it) {
            if (it.key().startsWith(QLatin1String("pc_"))
                || it.key().startsWith(QLatin1String("session_")))
                keep.append(it.key());
        }
        QHash<QString, Layer> leftover;
        for (const QString &k : keep)
            leftover.insert(k, m_layers.value(k));
        clearLayers();
        m_layers = leftover;
        applyTop();
        if (m_layers.isEmpty() && m_dmx)
            m_dmx->clearOverride(500);
        if (m_layers.isEmpty())
            setHint(QString());
    }
}

void ReactiveLighting::setEventPresets(const QJsonObject &events)
{
    m_presets.clear();
    for (auto it = events.begin(); it != events.end(); ++it) {
        if (!it.value().isObject())
            continue;
        m_presets.insert(it.key(), parsePreset(it.value().toObject(), fallbackPreset(it.key())));
    }
}

bool ReactiveLighting::playPreset(const QString &id, Priority pri, const QString &hint,
                                  const QString &gameColor, int gameBrightness)
{
    if (id.isEmpty())
        return false;
    const EventPreset p = presetFor(id);
    const bool lifecycle = id.startsWith(QLatin1String("pc_"))
        || id.startsWith(QLatin1String("session_"));
    if (lifecycle)
        releaseLifecycleExcept(id);
    if (!p.enabled) {
        if (lifecycle) {
            release(id);
            applyTop();
        }
        return false;
    }

    QString color = p.color;
    int brightness = p.brightness;
    if (color.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        if (gameColor.trimmed().isEmpty())
            return false;
        color = gameColor.trimmed();
        if (gameBrightness >= 0)
            brightness = std::clamp(gameBrightness, 0, 100);
    }

    const bool overlay = p.strobe
        || (p.effect == QLatin1String("cycle") && p.cycleColors.size() >= 2)
        || p.durationMs > 0
        || (p.effect == QLatin1String("rainbow") && p.durationMs > 0);
    if (lifecycle && !overlay) {
        release(id);
        applyTop();
        if (!hint.isEmpty())
            setHint(hint);
        return true;
    }

    Layer &layer = m_layers[id];
    if (layer.ttl) {
        layer.ttl->stop();
        layer.ttl->deleteLater();
        layer.ttl = nullptr;
    }
    layer.pri = pri;
    layer.color = color;
    layer.brightness = std::clamp(brightness, 0, 100);
    layer.effect = p.effect;
    layer.strobe = p.strobe;
    layer.strobeOnMs = p.strobeOnMs;
    layer.strobeOffMs = p.strobeOffMs;
    layer.cycleColors = p.cycleColors;
    layer.cycleHoldMs = p.cycleHoldMs;
    layer.fadeMs = p.fadeMs;
    layer.hint = hint.isEmpty() ? id : hint;
    layer.stamp = QDateTime::currentMSecsSinceEpoch();
    armTtl(layer, id, p.durationMs);
    applyTop();
    return true;
}

void ReactiveLighting::pulse(const QString &source, Priority pri, const QString &color, int brightness,
                             bool strobe, int ttlMs, const QString &hint)
{
    const bool lifecycle = source.startsWith(QLatin1String("pc_"))
        || source.startsWith(QLatin1String("session_"));
    if ((!m_enabled && !lifecycle) || source.isEmpty() || color.trimmed().isEmpty())
        return;

    if (m_presets.contains(source)) {
        playPreset(source, pri, hint, color, brightness);
        return;
    }

    Layer &layer = m_layers[source];
    if (layer.ttl) {
        layer.ttl->stop();
        layer.ttl->deleteLater();
        layer.ttl = nullptr;
    }
    layer.pri = pri;
    layer.color = color.trimmed();
    layer.brightness = std::clamp(brightness, 0, 100);
    layer.effect = QStringLiteral("none");
    layer.strobe = strobe;
    layer.strobeOnMs = 90;
    layer.strobeOffMs = 90;
    layer.cycleColors.clear();
    layer.cycleHoldMs = 400;
    layer.fadeMs = strobe ? 0 : 280;
    layer.hint = hint.isEmpty() ? source : hint;
    layer.stamp = QDateTime::currentMSecsSinceEpoch();
    armTtl(layer, source, ttlMs);
    applyTop();
}

void ReactiveLighting::release(const QString &source)
{
    auto it = m_layers.find(source);
    if (it == m_layers.end())
        return;
    if (it->ttl) {
        it->ttl->stop();
        it->ttl->deleteLater();
    }
    m_layers.erase(it);
    applyTop();
}

QString ReactiveLighting::hexColor(int r, int g, int b)
{
    return QStringLiteral("#%1%2%3")
        .arg(std::clamp(r, 0, 255), 2, 16, QLatin1Char('0'))
        .arg(std::clamp(g, 0, 255), 2, 16, QLatin1Char('0'))
        .arg(std::clamp(b, 0, 255), 2, 16, QLatin1Char('0'));
}

void ReactiveLighting::applyTop()
{
    if (!m_dmx)
        return;
    if (m_layers.isEmpty()) {
        m_dmx->clearOverride(700);
        if (m_enabled)
            setHint(QStringLiteral("ждём игру"));
        return;
    }

    const Layer *best = nullptr;
    for (auto it = m_layers.constBegin(); it != m_layers.constEnd(); ++it) {
        if (!best
            || it->pri > best->pri
            || (it->pri == best->pri && it->stamp > best->stamp)) {
            best = &it.value();
        }
    }
    if (!best)
        return;

    DmxController::OverrideSpec spec;
    spec.color = best->color;
    spec.brightness = best->brightness;
    spec.effect = best->effect;
    spec.strobe = best->strobe;
    spec.strobeOnMs = best->strobeOnMs;
    spec.strobeOffMs = best->strobeOffMs;
    spec.cycleColors = best->cycleColors;
    spec.cycleHoldMs = best->cycleHoldMs;
    spec.fadeMs = best->fadeMs;
    m_dmx->setOverride(spec);
    setHint(best->hint);
}

void ReactiveLighting::setHint(const QString &text)
{
    if (m_lastEvent == text)
        return;
    m_lastEvent = text;
    emit lastEventChanged();
}

void ReactiveLighting::clearLayers()
{
    for (auto it = m_layers.begin(); it != m_layers.end(); ++it) {
        if (it->ttl) {
            it->ttl->stop();
            it->ttl->deleteLater();
        }
    }
    m_layers.clear();
}

void ReactiveLighting::armTtl(Layer &layer, const QString &source, int ttlMs)
{
    if (ttlMs <= 0)
        return;
    layer.ttl = new QTimer(this);
    layer.ttl->setSingleShot(true);
    const QString src = source;
    connect(layer.ttl, &QTimer::timeout, this, [this, src]() { release(src); });
    layer.ttl->start(ttlMs);
}

void ReactiveLighting::releaseLifecycleExcept(const QString &keep)
{
    const QStringList keys = m_layers.keys();
    for (const QString &k : keys) {
        if (k == keep)
            continue;
        if (k.startsWith(QLatin1String("pc_")) || k.startsWith(QLatin1String("session_")))
            release(k);
    }
}

ReactiveLighting::EventPreset ReactiveLighting::presetFor(const QString &id) const
{
    if (m_presets.contains(id))
        return m_presets.value(id);
    return fallbackPreset(id);
}

ReactiveLighting::EventPreset ReactiveLighting::fallbackPreset(const QString &id) const
{
    EventPreset p;
    p.enabled = true;
    p.fadeMs = 300;
    p.brightness = 100;
    if (id == QLatin1String("pc_on") || id == QLatin1String("session_end")) {
        p.color = QStringLiteral("white");
        p.brightness = 80;
        p.fadeMs = 1200;
    } else if (id == QLatin1String("session_start")) {
        p.color = QStringLiteral("green");
        p.brightness = 80;
        p.fadeMs = 2500;
    } else if (id == QLatin1String("pc_shutdown")) {
        p.color = QStringLiteral("white");
        p.brightness = 80;
        p.durationMs = 800;
        p.fadeMs = 800;
    } else if (id == QLatin1String("pc_off")) {
        p.color = QStringLiteral("white");
        p.brightness = 0;
        p.fadeMs = 800;
    } else if (id == QLatin1String("cs2.bomb") || id == QLatin1String("gamesense.bomb")) {
        p.color = QStringLiteral("red");
        p.strobe = true;
        p.fadeMs = 0;
    } else if (id == QLatin1String("cs2.win") || id == QLatin1String("dota.win")
               || id == QLatin1String("gamesense.win")
               || id == QLatin1String("arena.win")) {
        p.color = id == QLatin1String("arena.win") ? QStringLiteral("yellow") : QStringLiteral("blue");
        p.durationMs = id == QLatin1String("arena.win") ? 3000 : 2500;
        p.strobe = id == QLatin1String("arena.win");
        p.fadeMs = id == QLatin1String("arena.win") ? 0 : p.fadeMs;
    } else if (id == QLatin1String("cs2.death") || id == QLatin1String("dota.death")
               || id == QLatin1String("gamesense.death")) {
        p.color = QStringLiteral("white");
        p.brightness = 12;
        p.fadeMs = 200;
    } else if (id == QLatin1String("gamesense.hit")) {
        p.color = QStringLiteral("red");
        p.brightness = 80;
        p.durationMs = 400;
        p.fadeMs = 0;
    } else if (id == QLatin1String("cs2.ambient.winter")) {
        p.color = QStringLiteral("cold_white");
        p.brightness = 85;
        p.fadeMs = 800;
    } else if (id == QLatin1String("cs2.ambient.inferno")) {
        p.color = QStringLiteral("orange");
        p.brightness = 80;
        p.fadeMs = 500;
    } else if (id == QLatin1String("cs2.flash")) {
        p.color = QStringLiteral("white");
        p.brightness = 100;
        p.durationMs = 800;
        p.fadeMs = 0;
    } else if (id == QLatin1String("chroma") || id == QLatin1String("gamesense")) {
        p.color = QStringLiteral("auto");
        p.fadeMs = 200;
    }
    return p;
}

ReactiveLighting::EventPreset ReactiveLighting::parsePreset(const QJsonObject &obj,
                                                            const EventPreset &base)
{
    EventPreset p = base;
    p.enabled = jsonBool(obj, QStringLiteral("enabled"), base.enabled);
    p.durationMs = int(std::clamp(jsonNum(obj, QStringLiteral("duration_sec"),
                                          base.durationMs / 1000.0), 0.0, 120.0) * 1000);
    const QString color = obj.value(QStringLiteral("color")).toString(base.color).trimmed().toLower();
    if (!color.isEmpty())
        p.color = color;
    const QString effect = obj.value(QStringLiteral("effect")).toString(base.effect).trimmed().toLower();
    if (effect == QLatin1String("rainbow") || effect == QLatin1String("cycle")
        || effect == QLatin1String("none"))
        p.effect = effect;
    p.brightness = std::clamp(int(jsonNum(obj, QStringLiteral("brightness"), base.brightness)), 0, 100);
    p.strobe = jsonBool(obj, QStringLiteral("strobe"), base.strobe);
    p.strobeOnMs = std::clamp(int(jsonNum(obj, QStringLiteral("strobe_on_ms"), base.strobeOnMs)), 0, 5000);
    p.strobeOffMs = std::clamp(int(jsonNum(obj, QStringLiteral("strobe_off_ms"), base.strobeOffMs)), 0, 5000);
    p.cycleHoldMs = int(std::clamp(jsonNum(obj, QStringLiteral("cycle_hold_sec"),
                                           base.cycleHoldMs / 1000.0), 0.05, 30.0) * 1000);
    p.fadeMs = int(std::clamp(jsonNum(obj, QStringLiteral("fade_sec"), base.fadeMs / 1000.0),
                              0.0, 30.0) * 1000);
    p.cycleColors.clear();
    const QJsonArray arr = obj.value(QStringLiteral("cycle_colors")).toArray();
    for (const QJsonValue &v : arr) {
        const QString c = v.toString().trimmed().toLower();
        if (!c.isEmpty() && !p.cycleColors.contains(c))
            p.cycleColors.append(c);
        if (p.cycleColors.size() >= 8)
            break;
    }
    if (p.effect == QLatin1String("cycle") && p.cycleColors.size() < 2) {
        p.cycleColors = QStringList{QStringLiteral("red"), QStringLiteral("blue")};
    }
    if (p.effect == QLatin1String("rainbow"))
        p.color = QStringLiteral("rainbow");
    return p;
}

double ReactiveLighting::jsonNum(const QJsonObject &obj, const QString &key, double fallback)
{
    if (!obj.contains(key))
        return fallback;
    const QJsonValue v = obj.value(key);
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        return ok ? d : fallback;
    }
    if (v.isBool())
        return v.toBool() ? 1.0 : 0.0;
    return fallback;
}

bool ReactiveLighting::jsonBool(const QJsonObject &obj, const QString &key, bool fallback)
{
    if (!obj.contains(key))
        return fallback;
    const QJsonValue v = obj.value(key);
    if (v.isBool())
        return v.toBool();
    if (v.isDouble())
        return v.toDouble() != 0.0;
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s == QLatin1String("1") || s == QLatin1String("true") || s == QLatin1String("yes"))
            return true;
        if (s == QLatin1String("0") || s == QLatin1String("false") || s == QLatin1String("no"))
            return false;
    }
    return fallback;
}
