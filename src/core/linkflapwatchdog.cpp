#include "linkflapwatchdog.h"

#include "networkmanager.h"
#include "pathresolver.h"
#include "stationhealth.h"
#include "hwidprovider.h"

#include <QDateTime>
#include <QSettings>

LinkFlapWatchdog::LinkFlapWatchdog(NetworkManager *net, QObject *parent)
    : QObject(parent)
    , m_net(net)
{
    loadConfig();
    m_tick.setInterval(m_pollMs);
    connect(&m_tick, &QTimer::timeout, this, &LinkFlapWatchdog::tick);
    if (m_enabled)
        m_tick.start();
    QTimer::singleShot(8000, this, &LinkFlapWatchdog::tick);
}

void LinkFlapWatchdog::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("LinkFlap/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_pollMs = qBound(5000, s.value(QStringLiteral("LinkFlap/poll_ms"), 15000).toInt(), 120000);
    m_cooldownMs = qBound(10000, s.value(QStringLiteral("LinkFlap/cooldown_ms"), 45000).toInt(), 600000);
}

bool LinkFlapWatchdog::isGigabit(int mbps) const
{
    return mbps >= 900;
}

bool LinkFlapWatchdog::isDegraded(int mbps) const
{
    return mbps > 0 && mbps <= 100;
}

void LinkFlapWatchdog::consumeFlaps(int n)
{
    if (n <= 0)
        return;
    m_pendingFlaps = qMax(0, m_pendingFlaps - n);
}

void LinkFlapWatchdog::tick()
{
    if (!m_enabled || !m_net)
        return;
    if (!m_net->featureEnabled(QStringLiteral("link_flap")))
        return;

    const QString mac = HwidProvider::onboardMac();
    const StationHealth::NicInfo nic = StationHealth::nicInfo(mac);
    if (nic.linkMbps < 0)
        return;

    const int mbps = nic.linkMbps;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    bool flap = false;
    if (m_hadGigabit && isDegraded(mbps)) {
        flap = true;
    } else if (m_lastMbps >= 900 && isDegraded(mbps)) {
        flap = true;
    }

    // Рост InErrors при уже деградированном линке — тоже flap (дребезг).
    if (!flap && isDegraded(mbps) && m_lastInErrors > 0
        && nic.inErrors > m_lastInErrors + 8) {
        flap = true;
    }

    if (flap && (now - m_lastFlapMs) >= m_cooldownMs) {
        ++m_pendingFlaps;
        m_lastFlapMs = now;
        qWarning() << "[LINK-FLAP] " << m_lastMbps << "→" << mbps
                   << "Mbps errors" << nic.inErrors << "pc" << m_net->getCurrentPcName();
        QJsonObject payload;
        payload.insert(QStringLiteral("from_mbps"), m_lastMbps);
        payload.insert(QStringLiteral("to_mbps"), mbps);
        payload.insert(QStringLiteral("in_errors"), static_cast<qint64>(nic.inErrors));
        payload.insert(QStringLiteral("out_errors"), static_cast<qint64>(nic.outErrors));
        // Сервер копит счётчик за смену и при ≥2 создаёт инцидент.
        m_net->noteLinkFlap(payload);
    }

    if (isGigabit(mbps))
        m_hadGigabit = true;
    m_lastMbps = mbps;
    m_lastInErrors = nic.inErrors;
}
