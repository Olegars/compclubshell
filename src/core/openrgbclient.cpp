#include "openrgbclient.h"

#include <QDateTime>
#include <QTcpSocket>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cstring>

namespace {

constexpr quint32 kOurProtocol = 5;
constexpr quint32 kPktCount = 0;
constexpr quint32 kPktData = 1;
constexpr quint32 kPktVersion = 40;
constexpr quint32 kPktClientName = 50;
constexpr quint32 kPktListUpdated = 100;
constexpr quint32 kPktResize = 1000;
constexpr quint32 kPktUpdateZone = 1051;
constexpr quint32 kPktCustom = 1100;

constexpr int kPhaseIdle = 0;
constexpr int kPhaseConnecting = 1;
constexpr int kPhaseWaitProto = 2;
constexpr int kPhaseWaitCount = 3;
constexpr int kPhaseWaitDev = 4;
constexpr int kPhaseReady = 5;

struct RgbCursor {
    const char *p = nullptr;
    const char *end = nullptr;
    bool ok = true;

    bool need(int n)
    {
        if (!ok || n < 0 || (end - p) < n) {
            ok = false;
            return false;
        }
        return true;
    }

    quint32 u32()
    {
        if (!need(4))
            return 0;
        quint32 v = 0;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }

    quint16 u16()
    {
        if (!need(2))
            return 0;
        quint16 v = 0;
        std::memcpy(&v, p, 2);
        p += 2;
        return v;
    }

    bool skip(int n)
    {
        if (!need(n))
            return false;
        p += n;
        return true;
    }

    QString str()
    {
        const quint16 n = u16();
        if (!ok)
            return {};
        if (n > 4096 || !need(n)) {
            ok = false;
            return {};
        }
        QByteArray raw(p, int(n));
        p += n;
        while (raw.endsWith('\0'))
            raw.chop(1);
        return QString::fromUtf8(raw);
    }
};

bool skipMatrix(RgbCursor &c)
{
    const quint16 bytes = c.u16();
    if (!c.ok)
        return false;
    if (bytes > 16384)
        return c.ok = false;
    return c.skip(int(bytes));
}

bool skipMode(RgbCursor &c, quint32 protocol)
{
    c.str();
    if (protocol < 6)
        c.u32();
    c.u32();
    c.u32();
    c.u32();
    if (protocol >= 3) {
        c.u32();
        c.u32();
    }
    c.u32();
    c.u32();
    c.u32();
    if (protocol >= 3)
        c.u32();
    c.u32();
    c.u32();
    const quint16 colors = c.u16();
    if (!c.ok || colors > 128)
        return c.ok = false;
    return c.skip(int(colors) * 4);
}

bool readZone(RgbCursor &c, quint32 protocol, QString *name, int *leds)
{
    const QString zoneName = c.str();
    c.u32();
    c.u32();
    c.u32();
    const quint32 count = c.u32();
    if (!skipMatrix(c))
        return false;
    if (protocol >= 4) {
        const quint16 segments = c.u16();
        if (!c.ok || segments > 64)
            return c.ok = false;
        for (quint16 i = 0; i < segments; ++i) {
            c.str();
            c.u32();
            c.u32();
            c.u32();
            if (protocol >= 6) {
                if (!skipMatrix(c))
                    return false;
                c.u32();
            }
        }
    }
    if (protocol >= 5)
        c.u32();
    if (protocol >= 6) {
        c.u32();
        const quint16 modes = c.u16();
        if (!c.ok || modes > 64)
            return c.ok = false;
        for (quint16 i = 0; i < modes; ++i) {
            if (!skipMode(c, protocol))
                return false;
        }
        c.str();
    }
    if (!c.ok || count > 2048)
        return c.ok = false;
    if (name)
        *name = zoneName;
    if (leds)
        *leds = int(count);
    return true;
}

} // namespace

OpenRgbClient::DevInfo OpenRgbClient::parseDevice(const QByteArray &payload, quint32 protocol)
{
    DevInfo info;
    if (protocol < 1 || protocol > 6)
        return info;
    RgbCursor c;
    c.p = payload.constData();
    c.end = c.p + payload.size();
    info.type = c.u32();
    info.name = c.str();
    if (protocol >= 1)
        info.vendor = c.str();
    c.str();
    c.str();
    c.str();
    c.str();
    const quint16 numModes = c.u16();
    c.u32();
    if (!c.ok || numModes > 64)
        return info;
    for (quint16 i = 0; i < numModes; ++i) {
        if (!skipMode(c, protocol))
            return info;
    }
    const quint16 numZones = c.u16();
    if (!c.ok || numZones > 64)
        return info;
    info.zones.reserve(numZones);
    for (quint16 i = 0; i < numZones; ++i) {
        ZoneInfo zone;
        if (!readZone(c, protocol, &zone.name, &zone.leds))
            return info;
        info.zones.append(zone);
    }
    info.ok = c.ok;
    return info;
}

OpenRgbClient::OpenRgbClient(const Config &config, QObject *parent)
    : QObject(parent)
    , m_cfg(config)
{
    if (m_cfg.host.trimmed().isEmpty())
        m_cfg.host = QStringLiteral("127.0.0.1");
    if (m_cfg.port <= 0 || m_cfg.port > 65535)
        m_cfg.port = 6742;
    m_cfg.maxFps = std::clamp(m_cfg.maxFps, 1, 25);
    m_minIntervalMs = std::max(40, 1000 / m_cfg.maxFps);
    m_cfg.retrySec = std::clamp(m_cfg.retrySec, 1, 30);
    m_retryMs = m_cfg.retrySec * 1000;
    m_cfg.ledCount = std::clamp(m_cfg.ledCount, 0, 512);
    m_cfg.zoneIndex = std::clamp(m_cfg.zoneIndex, -1, 64);
}

void OpenRgbClient::start()
{
    if (m_sock)
        return;
    m_sock = new QTcpSocket(this);
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    m_flush = new QTimer(this);
    m_flush->setSingleShot(true);
    m_testTimer = new QTimer(this);
    m_testTimer->setSingleShot(true);
    m_watch = new QTimer(this);
    m_watch->setSingleShot(true);
    m_watch->setInterval(5000);

    connect(m_sock, &QTcpSocket::connected, this, [this]() {
        m_phase = kPhaseWaitProto;
        m_buf.clear();
        QByteArray ver(4, 0);
        const quint32 proto = kOurProtocol;
        std::memcpy(ver.data(), &proto, 4);
        if (!sendPacket(0, kPktVersion, ver)) {
            drop(QStringLiteral("не удалось запросить протокол"));
            return;
        }
        m_watch->start();
        setStatus(false, QStringLiteral("рукопожатие…"));
    });
    connect(m_sock, &QTcpSocket::readyRead, this, &OpenRgbClient::onReadyRead);
    connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (m_stopped || m_dropping)
            return;
        QString why = m_sock->errorString();
        if (err == QAbstractSocket::ConnectionRefusedError) {
            why = QStringLiteral("служба не запущена (%1:%2). Запустите OpenRGB --server")
                      .arg(m_cfg.host)
                      .arg(m_cfg.port);
        }
        drop(why);
    });
    connect(m_sock, &QTcpSocket::disconnected, this, [this]() {
        if (m_stopped || m_dropping || m_phase == kPhaseIdle || m_phase == kPhaseConnecting)
            return;
        drop(QStringLiteral("сокет закрыт"));
    });
    connect(m_retry, &QTimer::timeout, this, [this]() {
        if (!m_stopped)
            connectNow();
    });
    connect(m_flush, &QTimer::timeout, this, &OpenRgbClient::flushSend);
    connect(m_testTimer, &QTimer::timeout, this, [this]() {
        m_testActive = false;
        m_hasSent = false;
        if (m_ready)
            setStatus(true, m_readyLabel);
        scheduleSend();
    });
    connect(m_watch, &QTimer::timeout, this, [this]() {
        if (!m_ready && !m_stopped)
            drop(QStringLiteral("таймаут ответа OpenRGB"));
    });

    setStatus(false, QStringLiteral("подключение…"));
    connectNow();
}

void OpenRgbClient::connectNow()
{
    if (m_stopped || !m_sock)
        return;
    m_ready = false;
    m_resizePending = false;
    m_buf.clear();
    m_phase = kPhaseConnecting;
    if (m_sock->state() != QAbstractSocket::UnconnectedState)
        m_sock->abort();
    m_sock->connectToHost(m_cfg.host, quint16(m_cfg.port));
}

void OpenRgbClient::drop(const QString &why)
{
    if (m_stopped || m_dropping)
        return;
    m_dropping = true;
    m_ready = false;
    m_resizePending = false;
    m_phase = kPhaseIdle;
    m_buf.clear();
    m_watch->stop();
    if (m_sock && m_sock->state() != QAbstractSocket::UnconnectedState)
        m_sock->abort();
    m_dropping = false;
    warnThrottled(why);
    setStatus(false, why);
    if (m_retry && !m_retry->isActive())
        m_retry->start(m_retryMs);
}

void OpenRgbClient::setStatus(bool ok, const QString &text)
{
    if (m_statusOk == ok && m_statusText == text)
        return;
    m_statusOk = ok;
    m_statusText = text;
    emit statusChanged(ok, text);
}

void OpenRgbClient::warnThrottled(const QString &why)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastWarnMs != 0 && (now - m_lastWarnMs) < 60000)
        return;
    m_lastWarnMs = now;
    qWarning().noquote() << "[OPENRGB]" << why;
}

void OpenRgbClient::onReadyRead()
{
    if (!m_sock)
        return;
    m_buf.append(m_sock->readAll());
    while (m_buf.size() >= 16) {
        if (m_buf.at(0) != 'O' || m_buf.at(1) != 'R' || m_buf.at(2) != 'G' || m_buf.at(3) != 'B') {
            const int at = m_buf.indexOf("ORGB", 1);
            if (at < 0) {
                m_buf.clear();
                return;
            }
            m_buf.remove(0, at);
            continue;
        }
        quint32 dev = 0;
        quint32 id = 0;
        quint32 size = 0;
        std::memcpy(&dev, m_buf.constData() + 4, 4);
        std::memcpy(&id, m_buf.constData() + 8, 4);
        std::memcpy(&size, m_buf.constData() + 12, 4);
        if (size > 2u * 1024u * 1024u) {
            drop(QStringLiteral("слишком большой пакет OpenRGB"));
            return;
        }
        if (m_buf.size() < 16 + int(size))
            return;
        const QByteArray payload = m_buf.mid(16, int(size));
        m_buf.remove(0, 16 + int(size));
        handlePacket(id, dev, payload);
        if (m_phase == kPhaseIdle)
            return;
    }
}

void OpenRgbClient::handlePacket(quint32 id, quint32 dev, const QByteArray &payload)
{
    if (id == kPktListUpdated && m_phase != kPhaseConnecting) {
        m_ready = false;
        m_hasSent = false;
        m_resizePending = false;
        beginScan();
        return;
    }

    if (m_phase == kPhaseWaitProto) {
        if (id != kPktVersion || payload.size() < 4)
            return;
        quint32 remote = 0;
        std::memcpy(&remote, payload.constData(), 4);
        m_proto = qMin(remote, 6u);
        if (m_proto < 1) {
            drop(QStringLiteral("OpenRGB без версии протокола"));
            return;
        }
        QByteArray name = QByteArrayLiteral("REACTOR SHELL");
        name.append('\0');
        QByteArray body;
        const quint16 n = quint16(name.size());
        body.append(char(n & 0xFF));
        body.append(char((n >> 8) & 0xFF));
        body.append(name);
        sendPacket(0, kPktClientName, body);
        beginScan();
        return;
    }

    if (m_phase == kPhaseWaitCount) {
        if (id != kPktCount || payload.size() < 4)
            return;
        quint32 count = 0;
        std::memcpy(&count, payload.constData(), 4);
        m_count = int(count);
        if (m_count <= 0 || m_count > 64) {
            drop(QStringLiteral("OpenRGB не видит устройств"));
            return;
        }
        m_cursor = 0;
        m_bestScore = -1;
        m_bestDev = -1;
        m_watch->start();
        requestDevice(0);
        return;
    }

    if (m_phase == kPhaseWaitDev) {
        if (id != kPktData)
            return;
        const int expect = m_resizePending ? m_bestDev : m_cursor;
        if (int(dev) != expect)
            return;
        const DevInfo info = parseDevice(payload, m_proto);
        if (m_resizePending) {
            m_resizePending = false;
            if (!info.ok) {
                drop(QStringLiteral("не разобрать плату после resize"));
                return;
            }
            const int zone = selectZone(info.zones);
            if (zone < 0) {
                drop(QStringLiteral("нет зоны ADDR_LED"));
                return;
            }
            m_bestZone = zone;
            m_bestLeds = info.zones.at(zone).leds;
            m_bestZoneName = info.zones.at(zone).name;
            if (m_bestLeds <= 0) {
                drop(QStringLiteral("в зоне 0 светодиодов — задайте число в OpenRGB или OpenRGB/led_count"));
                return;
            }
            commitSelection();
            return;
        }
        if (info.ok)
            considerDevice(info, m_cursor);
        ++m_cursor;
        if (m_cursor < m_count) {
            m_watch->start();
            requestDevice(m_cursor);
        } else {
            finishScan();
        }
    }
}

void OpenRgbClient::beginScan()
{
    m_phase = kPhaseWaitCount;
    m_ready = false;
    m_resizePending = false;
    m_bestScore = -1;
    m_bestDev = -1;
    m_bestZone = -1;
    m_bestLeds = 0;
    m_watch->start();
    if (!sendPacket(0, kPktCount, {}))
        drop(QStringLiteral("не удалось запросить список устройств"));
}

void OpenRgbClient::requestDevice(int index)
{
    m_phase = kPhaseWaitDev;
    if (!sendPacket(quint32(index), kPktData, {}))
        drop(QStringLiteral("не удалось прочитать устройство"));
}

int OpenRgbClient::scoreDevice(const DevInfo &dev) const
{
    const QString filter = m_cfg.deviceFilter.trimmed().toLower();
    const QString name = dev.name.toLower();
    const QString vendor = dev.vendor.toLower();
    const bool hit = filter.isEmpty() || name.contains(filter) || vendor.contains(filter);
    if (!hit)
        return -1;
    int score = 10;
    if (dev.type == 0)
        score += 20;
    return score;
}

int OpenRgbClient::selectZone(const QVector<ZoneInfo> &zones) const
{
    if (m_cfg.zoneIndex >= 0)
        return m_cfg.zoneIndex < zones.size() ? m_cfg.zoneIndex : -1;
    const QStringList keys = {
        QStringLiteral("addr"),
        QStringLiteral("addressable"),
        QStringLiteral("argb"),
        QStringLiteral("a-rgb")
    };
    for (int i = 0; i < zones.size(); ++i) {
        const QString n = zones.at(i).name.toLower();
        for (const QString &key : keys) {
            if (n.contains(key))
                return i;
        }
    }
    return zones.isEmpty() ? -1 : 0;
}

void OpenRgbClient::considerDevice(const DevInfo &dev, int index)
{
    const int score = scoreDevice(dev);
    if (score < 0)
        return;
    if (m_bestScore >= 0 && score <= m_bestScore)
        return;
    const int zone = selectZone(dev.zones);
    if (zone < 0)
        return;
    m_bestScore = score;
    m_bestDev = index;
    m_bestZone = zone;
    m_bestLeds = dev.zones.at(zone).leds;
    m_bestZoneName = dev.zones.at(zone).name;
    m_bestDevName = dev.name.trimmed().isEmpty() ? dev.vendor : dev.name.trimmed();
}

void OpenRgbClient::finishScan()
{
    if (m_bestDev < 0 || m_bestZone < 0) {
        const QString filter = m_cfg.deviceFilter.trimmed().isEmpty()
            ? QStringLiteral("материнская плата")
            : m_cfg.deviceFilter.trimmed();
        drop(QStringLiteral("не найдена зона подсветки (%1)").arg(filter));
        return;
    }
    if (m_bestLeds <= 0 && m_cfg.ledCount > 0) {
        QByteArray body(8, 0);
        const qint32 zone = m_bestZone;
        const qint32 size = m_cfg.ledCount;
        std::memcpy(body.data(), &zone, 4);
        std::memcpy(body.data() + 4, &size, 4);
        m_resizePending = true;
        m_phase = kPhaseWaitDev;
        m_watch->start();
        if (!sendPacket(quint32(m_bestDev), kPktResize, body)) {
            drop(QStringLiteral("не удалось задать число светодиодов"));
            return;
        }
        requestDevice(m_bestDev);
        return;
    }
    if (m_bestLeds <= 0) {
        drop(QStringLiteral("в зоне 0 светодиодов — задайте число в OpenRGB или OpenRGB/led_count"));
        return;
    }
    commitSelection();
}

void OpenRgbClient::commitSelection()
{
    m_dev = m_bestDev;
    m_zone = m_bestZone;
    m_leds = m_bestLeds;
    m_phase = kPhaseReady;
    m_ready = true;
    m_hasSent = false;
    m_watch->stop();
    m_lastWarnMs = 0;
    if (!sendPacket(quint32(m_dev), kPktCustom, {})) {
        drop(QStringLiteral("не удалось включить Direct"));
        return;
    }
    m_readyLabel = QStringLiteral("%1 · %2 · %3 LED")
                       .arg(m_bestDevName, m_bestZoneName)
                       .arg(m_leds);
    qInfo().noquote() << "[OPENRGB]" << m_readyLabel;
    setStatus(true, m_testActive ? QStringLiteral("тест: пурпурный") : m_readyLabel);
    flushSend();
}

void OpenRgbClient::submitColor(const QColor &color)
{
    if (m_stopped)
        return;
    m_live = color;
    m_hasLive = true;
    scheduleSend();
}

void OpenRgbClient::holdTestColor(const QColor &color, int durationMs)
{
    if (m_stopped)
        return;
    m_test = color;
    m_testActive = true;
    m_hasSent = false;
    m_testTimer->start(std::clamp(durationMs, 500, 15000));
    if (m_ready)
        setStatus(true, QStringLiteral("тест: пурпурный"));
    scheduleSend();
}

void OpenRgbClient::scheduleSend()
{
    if (!m_ready || m_stopped)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait = m_lastSendMs + m_minIntervalMs - now;
    if (wait > 0) {
        if (m_flush && !m_flush->isActive())
            m_flush->start(int(wait));
        return;
    }
    flushSend();
}

void OpenRgbClient::flushSend()
{
    if (!m_ready || m_stopped || m_leds <= 0)
        return;
    if (!m_testActive && !m_hasLive)
        return;
    const QColor want = m_testActive ? m_test : m_live;
    if (m_hasSent && want.red() == m_sentR && want.green() == m_sentG && want.blue() == m_sentB)
        return;
    if (!writeZone(want)) {
        drop(QStringLiteral("запись цвета не ушла"));
        return;
    }
    m_hasSent = true;
    m_sentR = want.red();
    m_sentG = want.green();
    m_sentB = want.blue();
    m_lastSendMs = QDateTime::currentMSecsSinceEpoch();
}

bool OpenRgbClient::writeZone(const QColor &color)
{
    QByteArray body;
    body.resize(6 + m_leds * 4);
    const qint32 zone = m_zone;
    const quint16 count = quint16(m_leds);
    std::memcpy(body.data(), &zone, 4);
    std::memcpy(body.data() + 4, &count, 2);
    const quint32 packed = quint32(color.red() & 255)
        | (quint32(color.green() & 255) << 8)
        | (quint32(color.blue() & 255) << 16);
    for (int i = 0; i < m_leds; ++i)
        std::memcpy(body.data() + 6 + i * 4, &packed, 4);
    return sendPacket(quint32(m_dev), kPktUpdateZone, body);
}

bool OpenRgbClient::sendPacket(quint32 dev, quint32 id, const QByteArray &payload)
{
    if (!m_sock || m_sock->state() != QAbstractSocket::ConnectedState)
        return false;
    QByteArray pkt(16, 0);
    pkt[0] = 'O';
    pkt[1] = 'R';
    pkt[2] = 'G';
    pkt[3] = 'B';
    std::memcpy(pkt.data() + 4, &dev, 4);
    std::memcpy(pkt.data() + 8, &id, 4);
    const quint32 size = quint32(payload.size());
    std::memcpy(pkt.data() + 12, &size, 4);
    pkt.append(payload);
    return m_sock->write(pkt) == pkt.size();
}

void OpenRgbClient::blackout()
{
    m_stopped = true;
    m_testActive = false;
    if (m_retry)
        m_retry->stop();
    if (m_flush)
        m_flush->stop();
    if (m_watch)
        m_watch->stop();
    if (m_ready && m_sock && m_sock->state() == QAbstractSocket::ConnectedState && m_leds > 0) {
        writeZone(QColor(0, 0, 0));
        m_sock->flush();
        m_sock->waitForBytesWritten(250);
    }
    m_ready = false;
    if (m_sock && m_sock->state() != QAbstractSocket::UnconnectedState)
        m_sock->disconnectFromHost();
}
