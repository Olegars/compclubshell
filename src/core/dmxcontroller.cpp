#include "dmxcontroller.h"

#include <QColor>
#include <QDateTime>
#include <QHostAddress>
#include <algorithm>
#include <initializer_list>

DmxController::DmxController(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &DmxController::onTick);
}

void DmxController::setNodes(const QVector<Node> &nodes, int fadeMs)
{
    m_fadeFrom = m_liveColors;
    m_nodes = nodes;
    if (fadeMs > 0 && !m_fadeFrom.isEmpty()) {
        m_fadeMs = fadeMs;
        m_fadeStartMs = QDateTime::currentMSecsSinceEpoch();
    } else {
        m_fadeMs = 0;
    }
}

bool DmxController::fading() const
{
    return m_fadeMs > 0;
}

void DmxController::setRainbowPeriodMs(int ms)
{
    m_rainbowPeriodMs = std::max(1000, ms);
}

void DmxController::setAllBrightness(int brightness)
{
    const int br = std::clamp(brightness, 0, 100);
    for (Node &n : m_nodes) {
        for (Fixture &fx : n.fixtures)
            fx.brightness = br;
    }
}

void DmxController::setOverride(const QString &color, int brightness, const QString &effect, bool strobe)
{
    OverrideSpec spec;
    spec.color = color;
    spec.brightness = brightness;
    spec.effect = effect;
    spec.strobe = strobe;
    spec.fadeMs = strobe ? 0 : 280;
    setOverride(spec);
}

void DmxController::setOverride(const OverrideSpec &spec)
{
    const QString c = spec.color.isEmpty() ? QStringLiteral("red") : spec.color;
    const int br = std::clamp(spec.brightness, 0, 100);
    const QString fx = spec.effect.isEmpty() ? QStringLiteral("none") : spec.effect;
    const int onMs = std::max(0, spec.strobeOnMs);
    const int offMs = std::max(0, spec.strobeOffMs);
    const int holdMs = std::max(50, spec.cycleHoldMs);
    const int fadeMs = spec.strobe ? 0 : std::max(0, spec.fadeMs);
    if (m_overrideActive && m_overrideColor == c && m_overrideBrightness == br
        && m_overrideEffect == fx && m_overrideStrobe == spec.strobe
        && m_overrideStrobeOnMs == onMs && m_overrideStrobeOffMs == offMs
        && m_overrideCycle == spec.cycleColors && m_overrideCycleHoldMs == holdMs) {
        startRefresh();
        return;
    }
    m_fadeFrom = m_liveColors;
    m_fadeMs = fadeMs;
    m_fadeStartMs = QDateTime::currentMSecsSinceEpoch();
    m_overrideActive = true;
    m_overrideStrobe = spec.strobe;
    m_overrideStrobeOnMs = onMs;
    m_overrideStrobeOffMs = offMs;
    m_overrideColor = c;
    m_overrideBrightness = br;
    m_overrideEffect = fx;
    m_overrideCycle = spec.cycleColors;
    m_overrideCycleHoldMs = holdMs;
    startRefresh();
}

void DmxController::clearOverride(int fadeMs)
{
    if (!m_overrideActive)
        return;
    m_fadeFrom = m_liveColors;
    m_fadeMs = std::max(0, fadeMs);
    m_fadeStartMs = QDateTime::currentMSecsSinceEpoch();
    m_overrideActive = false;
    m_overrideStrobe = false;
    startRefresh();
}

bool DmxController::refreshRunning() const
{
    return m_timer.isActive();
}

void DmxController::startRefresh()
{
    bool rainbow = false;
    bool strobe = m_overrideActive && m_overrideStrobe;
    const bool cycling = m_overrideActive && m_overrideEffect == QLatin1String("cycle")
        && m_overrideCycle.size() >= 2;
    for (const Node &n : m_nodes) {
        for (const Fixture &fx : n.fixtures) {
            if (fx.effect == QLatin1String("rainbow") && fx.brightness > 0) {
                rainbow = true;
                break;
            }
        }
    }
    const int interval = (rainbow || fading() || strobe || cycling || m_overrideActive) ? 40 : 1000;
    if (m_timer.isActive() && m_timer.interval() == interval)
        return;
    m_timer.start(interval);
}

void DmxController::stopRefresh()
{
    if (m_localMirror) {
        startRefresh();
        return;
    }
    m_timer.stop();
}

void DmxController::setLocalMirror(bool on)
{
    m_localMirror = on;
    if (!on) {
        m_mirrorValid = false;
        return;
    }
    startRefresh();
    if (m_nodes.isEmpty())
        paintGhost();
    else
        sendOnce(nullptr);
}

void DmxController::emitMirror(const QColor &color)
{
    if (!m_localMirror)
        return;
    if (m_mirrorValid && m_mirrorColor == color)
        return;
    m_mirrorValid = true;
    m_mirrorColor = color;
    emit renderedColor(color);
}

void DmxController::paintGhost()
{
    Node ghost;
    ghost.host = QStringLiteral("local");
    Fixture fx;
    fx.start = 1;
    fx.count = 1;
    fx.layout = QStringLiteral("rgb");
    fx.brightness = 0;
    ghost.fixtures.append(fx);
    QByteArray dmx(512, char(0));
    paintUniverse(dmx, ghost);
    emitMirror(m_liveColors.value(fixtureKey(ghost, fx)));
}

QColor DmxController::scaledRgb(const QString &color, int brightness, int r, int g, int b,
                                 const QString &effect, int rainbowPeriodMs)
{
    const int br = std::clamp(brightness, 0, 100);
    if (br <= 0)
        return QColor(0, 0, 0);

    if (effect == QLatin1String("rainbow") || color == QLatin1String("rainbow")) {
        const int period = std::max(1000, rainbowPeriodMs);
        const qint64 ms = QDateTime::currentMSecsSinceEpoch();
        const int hue = int((ms % period) * 360 / period);
        return QColor::fromHsv(hue, 255, int(br * 255 / 100));
    }

    int rr = r, gg = g, bb = b;
    if (color.startsWith(QLatin1Char('#'))) {
        const QColor named(color);
        if (named.isValid()) {
            rr = named.red();
            gg = named.green();
            bb = named.blue();
        }
    } else if (color == QLatin1String("red")) { rr = 255; gg = 32; bb = 32; }
    else if (color == QLatin1String("blue")) { rr = 40; gg = 90; bb = 255; }
    else if (color == QLatin1String("green")) { rr = 34; gg = 197; bb = 94; }
    else if (color == QLatin1String("yellow")) { rr = 234; gg = 179; bb = 8; }
    else if (color == QLatin1String("purple")) { rr = 168; gg = 85; bb = 247; }
    else if (color == QLatin1String("white")) { rr = 255; gg = 255; bb = 255; }
    else if (color == QLatin1String("orange")) { rr = 255; gg = 138; bb = 60; }
    else if (color == QLatin1String("cold_white")) { rr = 200; gg = 220; bb = 255; }

    rr = std::clamp(rr, 0, 255) * br / 100;
    gg = std::clamp(gg, 0, 255) * br / 100;
    bb = std::clamp(bb, 0, 255) * br / 100;
    return QColor(rr, gg, bb);
}

bool DmxController::sendOnce(QString *errorOut)
{
    if (m_nodes.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("no Art-Net node");
        return false;
    }

    bool anyOk = false;
    QString lastErr;
    bool captured = false;
    QColor primary;
    for (const Node &node : m_nodes) {
        QByteArray dmx(512, char(0));
        paintUniverse(dmx, node);
        if (!captured && !node.fixtures.isEmpty()) {
            primary = m_liveColors.value(fixtureKey(node, node.fixtures.first()));
            captured = true;
        }
        if (node.host.trimmed().isEmpty() || node.port <= 0)
            continue;
        const QByteArray pkt = buildArtDmx(node.universe, dmx);
        const qint64 n = m_sock.writeDatagram(pkt, QHostAddress(node.host.trimmed()), quint16(node.port));
        if (m_sequence == 255)
            m_sequence = 1;
        else
            ++m_sequence;
        if (n == pkt.size()) {
            anyOk = true;
        } else {
            lastErr = m_sock.errorString();
            if (lastErr.isEmpty())
                lastErr = QStringLiteral("Art-Net write failed");
        }
    }

    if (captured)
        emitMirror(primary);

    if (!anyOk && errorOut)
        *errorOut = lastErr.isEmpty() ? QStringLiteral("Art-Net send failed") : lastErr;
    return anyOk;
}

void DmxController::onTick()
{
    if (!m_nodes.isEmpty())
        sendOnce(nullptr);
    else if (m_localMirror)
        paintGhost();
    if (m_fadeMs > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_fadeStartMs;
        if (elapsed >= m_fadeMs) {
            m_fadeMs = 0;
            startRefresh();
        }
    }
}

QString DmxController::fixtureKey(const Node &node, const Fixture &fx) const
{
    return QStringLiteral("%1:%2:%3").arg(node.host).arg(node.universe).arg(fx.start);
}

QColor DmxController::lerpColor(const QColor &from, const QColor &to, float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return QColor(
        int(from.red() + (to.red() - from.red()) * t),
        int(from.green() + (to.green() - from.green()) * t),
        int(from.blue() + (to.blue() - from.blue()) * t));
}

QByteArray DmxController::buildArtDmx(int universe, const QByteArray &dmx) const
{
    QByteArray pkt;
    pkt.reserve(18 + 512);
    pkt.append(QByteArray("Art-Net", 8));
    pkt.append(char(0x00));
    pkt.append(char(0x50)); // OpOutput little-endian
    pkt.append(char(0x00));
    pkt.append(char(0x0E)); // protocol 14
    pkt.append(char(m_sequence));
    pkt.append(char(0x00)); // physical
    const int uni = std::clamp(universe, 0, 32767);
    pkt.append(char(uni & 0xFF));
    pkt.append(char((uni >> 8) & 0x7F));
    pkt.append(char(0x02)); // length 512 big-endian
    pkt.append(char(0x00));
    pkt.append(dmx.left(512));
    if (pkt.size() < 18 + 512)
        pkt.append(QByteArray(18 + 512 - pkt.size(), char(0)));
    return pkt;
}

void DmxController::paintUniverse(QByteArray &dmx, const Node &node)
{
    float fadeT = 1.f;
    if (m_fadeMs > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_fadeStartMs;
        fadeT = std::clamp(float(elapsed) / float(m_fadeMs), 0.f, 1.f);
    }

    for (const Fixture &fx : node.fixtures) {
        QString color = fx.color;
        int brightness = fx.brightness;
        QString effect = fx.effect;
        if (m_overrideActive) {
            color = m_overrideColor;
            brightness = m_overrideBrightness;
            effect = m_overrideEffect;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const QStringList &cycle = m_overrideCycle;
            if (effect == QLatin1String("cycle") && cycle.size() >= 2) {
                const int hold = std::max(50, m_overrideCycleHoldMs);
                const int n = cycle.size();
                const qint64 slot = now / hold;
                const int idx = int(slot % n);
                const int prev = (idx + n - 1) % n;
                const qint64 phase = now % hold;
                color = cycle.at(idx);
                effect = QStringLiteral("none");
                if (m_fadeMs > 0 && phase < m_fadeMs) {
                    int fadeBr = brightness;
                    if (m_overrideStrobe) {
                        const int onMs = std::max(1, m_overrideStrobeOnMs);
                        const int offMs = std::max(0, m_overrideStrobeOffMs);
                        const int period = onMs + offMs;
                        if (offMs > 0 && (now % period) >= onMs)
                            fadeBr = 0;
                    }
                    const QColor from = scaledRgb(cycle.at(prev), fadeBr, fx.r, fx.g, fx.b,
                                                  QStringLiteral("none"), m_rainbowPeriodMs);
                    const QColor to = scaledRgb(color, fadeBr, fx.r, fx.g, fx.b,
                                                QStringLiteral("none"), m_rainbowPeriodMs);
                    const float t = std::clamp(float(phase) / float(m_fadeMs), 0.f, 1.f);
                    const QColor mixed = lerpColor(from, to, t);
                    const QString key = fixtureKey(node, fx);
                    QColor out = mixed;
                    m_liveColors.insert(key, out);
                    writeFixture(dmx, fx, out);
                    continue;
                }
            }
            if (m_overrideStrobe) {
                const int onMs = std::max(1, m_overrideStrobeOnMs);
                const int offMs = std::max(0, m_overrideStrobeOffMs);
                const int period = onMs + offMs;
                if (offMs > 0 && (now % period) >= onMs)
                    brightness = 0;
            }
        }
        const QColor target = scaledRgb(color, brightness, fx.r, fx.g, fx.b,
                                          effect, m_rainbowPeriodMs);
        const QString key = fixtureKey(node, fx);
        QColor out = target;
        if (m_fadeMs > 0 && m_fadeFrom.contains(key))
            out = lerpColor(m_fadeFrom.value(key), target, fadeT);
        m_liveColors.insert(key, out);
        writeFixture(dmx, fx, out);
    }
}

void DmxController::writeFixture(QByteArray &dmx, const Fixture &fx, const QColor &c) const
{
    const int width = (fx.layout == QLatin1String("dimmer_rgb")
                       || fx.layout == QLatin1String("rgbw"))
        ? 4
        : 3;
    const int count = std::max(1, fx.count);
    for (int i = 0; i < count; ++i) {
        const int ch = fx.start + i * width; // 1-based
        const int idx = ch - 1;
        if (idx < 0 || idx + width > dmx.size())
            continue;

        if (fx.layout == QLatin1String("dimmer_rgb")) {
            const int dim = std::max({c.red(), c.green(), c.blue()});
            dmx[idx] = char(dim);
            dmx[idx + 1] = char(c.red());
            dmx[idx + 2] = char(c.green());
            dmx[idx + 3] = char(c.blue());
        } else if (fx.layout == QLatin1String("rgbw")) {
            const bool white = (fx.color == QLatin1String("white")
                                && fx.effect != QLatin1String("rainbow"));
            if (white) {
                dmx[idx] = char(0);
                dmx[idx + 1] = char(0);
                dmx[idx + 2] = char(0);
                dmx[idx + 3] = char(c.red());
            } else {
                dmx[idx] = char(c.red());
                dmx[idx + 1] = char(c.green());
                dmx[idx + 2] = char(c.blue());
                dmx[idx + 3] = char(0);
            }
        } else {
            dmx[idx] = char(c.red());
            dmx[idx + 1] = char(c.green());
            dmx[idx + 2] = char(c.blue());
        }
    }
}
