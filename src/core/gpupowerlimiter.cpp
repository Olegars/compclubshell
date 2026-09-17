#include "gpupowerlimiter.h"

#include "networkmanager.h"
#include "pathresolver.h"

#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

GpuPowerLimiter::GpuPowerLimiter(NetworkManager *net, QObject *parent)
    : QObject(parent)
    , m_net(net)
{
    loadConfig();
    m_tick.setInterval(20000);
    connect(&m_tick, &QTimer::timeout, this, &GpuPowerLimiter::tick);
    if (m_enabled)
        m_tick.start();
    QTimer::singleShot(8000, this, &GpuPowerLimiter::tick);

    if (m_net) {
        connect(m_net, &NetworkManager::loginSucceeded, this, [this]() { updateMode(false); });
        connect(m_net, &NetworkManager::sessionForceEnded, this, [this]() { tick(); });
    }
}

void GpuPowerLimiter::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("GpuPower/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_idleLimitW = qBound(30, s.value(QStringLiteral("GpuPower/idle_limit_w"), 45).toInt(), 80);
    m_defaultLimitW = qBound(0, s.value(QStringLiteral("GpuPower/default_limit_w"), 0).toInt(), 1000);
    m_nvidiaSmi = s.value(QStringLiteral("GpuPower/nvidia_smi")).toString().trimmed();
    if (m_nvidiaSmi.isEmpty())
        m_nvidiaSmi = QStringLiteral("nvidia-smi");
}

bool GpuPowerLimiter::nvidiaPresent() const
{
    QProcess p;
    p.setProgram(m_nvidiaSmi);
    p.setArguments({QStringLiteral("-L")});
    p.start(QIODevice::ReadOnly);
    if (!p.waitForFinished(2500))
        return false;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 && !p.readAllStandardOutput().trimmed().isEmpty();
}

bool GpuPowerLimiter::applyLimit(int watts)
{
    if (watts <= 0)
        return clearLimit();
    QProcess p;
    p.setProgram(m_nvidiaSmi);
    p.setArguments({QStringLiteral("-pl"), QString::number(watts)});
    p.start(QIODevice::ReadOnly);
    if (!p.waitForFinished(4000))
        return false;
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (ok) {
        m_limited = true;
        m_appliedW = watts;
        qWarning() << "[GPU-ECO] power limit" << watts << "W";
    }
    return ok;
}

bool GpuPowerLimiter::clearLimit()
{
    if (!m_limited && m_appliedW <= 0)
        return true;
    const int target = m_defaultLimitW > 0 ? m_defaultLimitW : 999;
    QProcess p;
    p.setProgram(m_nvidiaSmi);
    p.setArguments({QStringLiteral("-pl"), QString::number(target)});
    p.start(QIODevice::ReadOnly);
    if (!p.waitForFinished(4000))
        return false;
    m_limited = false;
    m_appliedW = 0;
    qWarning() << "[GPU-ECO] power limit cleared →" << target << "W";
    return p.exitStatus() == QProcess::NormalExit;
}

void GpuPowerLimiter::updateMode(bool idleWarmup)
{
    if (!m_enabled || !m_net || !nvidiaPresent())
        return;

    if (idleWarmup && !m_net->isGuestSessionActive() && !m_net->maintenance()) {
        if (!m_limited || m_appliedW != m_idleLimitW) {
            applyLimit(m_idleLimitW);
            m_net->setGpuTelemetry(m_idleLimitW, QStringLiteral("idle"));
        }
        m_lastIdleWarmup = true;
        return;
    }

    if (m_limited || m_lastIdleWarmup) {
        clearLimit();
        m_net->setGpuTelemetry(0, m_net->isGuestSessionActive() ? QStringLiteral("session") : QStringLiteral("unknown"));
    }
    m_lastIdleWarmup = false;
}

void GpuPowerLimiter::tick()
{
    if (!m_enabled || !m_net)
        return;
    const bool idleWarmup = m_net->isWarmupIdle();
    updateMode(idleWarmup);
}
