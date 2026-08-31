#include "dmxcontroller.h"

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

bool DmxController::refreshRunning() const
{
    return m_timer.isActive();
}

void DmxController::startRefresh()
{
    bool rainbow = false;
    for (const Node &n : m_nodes) {
        for (const Fixture &fx : n.fixtures) {
            if (fx.effect == QLatin1String("rainbow") && fx.brightness > 0) {
                rainbow = true;
                break;
            }
        }
    }
    const int interval = (rainbow || fading()) ? 40 : 1000;
    if (m_timer.isActive() && m_timer.interval() == interval)
        return;
    m_timer.start(interval);
}

void DmxController::stopRefresh()
{
    m_timer.stop();
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

    const int rr = std::clamp(r, 0, 255) * br / 100;
    const int gg = std::clamp(g, 0, 255) * br / 100;
    const int bb = std::clamp(b, 0, 255) * br / 100;
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
    for (const Node &node : m_nodes) {
        if (node.host.trimmed().isEmpty() || node.port <= 0)
            continue;
        QByteArray dmx(512, char(0));
        paintUniverse(dmx, node);
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

    if (!anyOk && errorOut)
        *errorOut = lastErr.isEmpty() ? QStringLiteral("Art-Net send failed") : lastErr;
    return anyOk;
}

void DmxController::onTick()
{
    sendOnce(nullptr);
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
        const QColor target = scaledRgb(fx.color, fx.brightness, fx.r, fx.g, fx.b,
                                          fx.effect, m_rainbowPeriodMs);
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
