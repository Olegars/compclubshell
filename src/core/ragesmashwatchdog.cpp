#include "ragesmashwatchdog.h"

#include "hidinputmonitor.h"
#include "networkmanager.h"
#include "pathresolver.h"
#include "reactivelighting.h"
#include "valvegsi.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QSettings>
#include <QWindow>
#include <cmath>

#ifdef Q_OS_WIN

namespace {

RageSmashWatchdog *g_rageSmash = nullptr;

LRESULT CALLBACK rageMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_rageSmash) {
        const MSLLHOOKSTRUCT *info = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
        if (info)
            g_rageSmash->handleMouse(wParam, info);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK rageKeyProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_rageSmash) {
        const KBDLLHOOKSTRUCT *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (info)
            g_rageSmash->handleKey(wParam, info);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

class RageSmashWatchdog::ImuSink : public QWindow
{
public:
    explicit ImuSink(RageSmashWatchdog *owner)
        : m_owner(owner)
    {
        setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus
                 | Qt::WindowTransparentForInput);
        setGeometry(-32000, -32000, 8, 8);
        create();
    }

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(eventType);
        auto *msg = static_cast<MSG *>(message);
        if (msg && msg->message == WM_INPUT && m_owner) {
            m_owner->handleRawInput(msg->lParam);
            return false;
        }
        return QWindow::nativeEvent(eventType, message, result);
    }

private:
    RageSmashWatchdog *m_owner = nullptr;
};

#endif

RageSmashWatchdog::RageSmashWatchdog(NetworkManager *net,
                                     HidInputMonitor *hid,
                                     QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_hid(hid)
{
    loadConfig();
    if (m_net) {
        ReactiveLighting *light = m_net->reactiveLighting();
        if (light && light->valveGsi()) {
            connect(light->valveGsi(), &ValveGsi::gameEvent,
                    this, &RageSmashWatchdog::onGsiEvent);
        }
    }
    if (m_hid) {
        connect(m_hid, &HidInputMonitor::watchingChanged, this, [this]() {
            if (m_hid && m_hid->watching())
                startWatch();
            else
                stopWatch();
        });
        if (m_hid->watching())
            startWatch();
    }
    if (m_net) {
        connect(m_net, &NetworkManager::clubFeaturesChanged, this, [this]() {
            if (clubEnabled() && m_hid && m_hid->watching())
                startWatch();
            else
                stopWatch();
        });
    }
}

RageSmashWatchdog::~RageSmashWatchdog()
{
    stopWatch();
}

void RageSmashWatchdog::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("RageSmash/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_mashKeys = qBound(6, s.value(QStringLiteral("RageSmash/mash_keys"), 10).toInt(), 24);
    m_mashWindowMs = qBound(40, s.value(QStringLiteral("RageSmash/mash_window_ms"), 100).toInt(), 250);
    m_mouseShockPx = qBound(2500, s.value(QStringLiteral("RageSmash/mouse_shock_px"), 9000).toInt(), 40000);
    m_mouseShockMs = qBound(30, s.value(QStringLiteral("RageSmash/mouse_shock_ms"), 80).toInt(), 250);
    m_imuSpike = qBound(1.4,
                        s.value(QStringLiteral("RageSmash/imu_spike"), 2.4).toDouble(),
                        8.0);
    m_deathNeed = qBound(1, s.value(QStringLiteral("RageSmash/death_need"), 2).toInt(), 6);
    m_kdWindowMs = qBound(8000, s.value(QStringLiteral("RageSmash/kd_window_ms"), 45000).toInt(), 180000);
    m_cooldownMs = qBound(30000, s.value(QStringLiteral("RageSmash/cooldown_ms"), 180000).toInt(), 1800000);
}

bool RageSmashWatchdog::clubEnabled() const
{
    if (!m_enabled)
        return false;
    return !m_net || m_net->featureEnabled(QStringLiteral("rage_smash"));
}

void RageSmashWatchdog::startWatch()
{
    if (!clubEnabled() || m_watching)
        return;
    m_watching = true;
    resetState();
    installHooks();
    installRawInput();
    qDebug() << "[RAGE-SMASH] watch started";
}

void RageSmashWatchdog::stopWatch()
{
    uninstallRawInput();
    uninstallHooks();
    if (!m_watching)
        return;
    m_watching = false;
    resetState();
    qDebug() << "[RAGE-SMASH] watch stopped";
}

void RageSmashWatchdog::resetState()
{
    m_keyDowns.clear();
    m_keyHeld.clear();
    m_mouseMoves.clear();
    m_fragEvents.clear();
    m_sessionKills = 0;
    m_sessionDeaths = 0;
    m_kdPeak = 0;
    m_imuBaseline = 0;
    m_lastImuMs = 0;
}

void RageSmashWatchdog::onGsiEvent(const QJsonObject &payload)
{
    const QString event = payload.value(QStringLiteral("event")).toString().toLower();
    if (event != QLatin1String("kill") && event != QLatin1String("death"))
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_fragEvents.append({now, event});
    while (!m_fragEvents.isEmpty() && now - m_fragEvents.first().first > m_kdWindowMs)
        m_fragEvents.removeFirst();
    if (event == QLatin1String("kill"))
        ++m_sessionKills;
    else
        ++m_sessionDeaths;
    const double kd = double(m_sessionKills) / double(qMax(1, m_sessionDeaths));
    if (kd > m_kdPeak)
        m_kdPeak = kd;
}

bool RageSmashWatchdog::kdDropped() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int kills = 0;
    int deaths = 0;
    for (const auto &ev : m_fragEvents) {
        if (now - ev.first > m_kdWindowMs)
            continue;
        if (ev.second == QLatin1String("death"))
            ++deaths;
        else if (ev.second == QLatin1String("kill"))
            ++kills;
    }
    if (deaths >= m_deathNeed && deaths > kills)
        return true;
    if (m_sessionDeaths >= m_deathNeed && m_kdPeak > 0.2) {
        const double nowKd = double(m_sessionKills) / double(qMax(1, m_sessionDeaths));
        if (nowKd < m_kdPeak * 0.7)
            return true;
    }
    return false;
}

void RageSmashWatchdog::noteKeyDown(quint32 vk, qint64 nowMs)
{
    m_keyDowns.append({nowMs, vk});
    while (!m_keyDowns.isEmpty() && nowMs - m_keyDowns.first().first > m_mashWindowMs)
        m_keyDowns.removeFirst();

    QHash<quint32, int> uniq;
    for (const auto &row : m_keyDowns)
        uniq.insert(row.second, 1);
    if (uniq.size() < m_mashKeys)
        return;

    QJsonObject extra;
    extra.insert(QStringLiteral("keys"), uniq.size());
    extra.insert(QStringLiteral("window_ms"), m_mashWindowMs);
    maybeFire(QStringLiteral("keymash"), extra);
}

void RageSmashWatchdog::noteMouseDelta(int dx, int dy, qint64 nowMs)
{
    const int dist = qAbs(dx) + qAbs(dy);
    if (dist < 2)
        return;
    m_mouseMoves.append({nowMs, dist});
    while (!m_mouseMoves.isEmpty() && nowMs - m_mouseMoves.first().first > m_mouseShockMs)
        m_mouseMoves.removeFirst();
    int sum = 0;
    for (const auto &row : m_mouseMoves)
        sum += row.second;
    if (sum < m_mouseShockPx)
        return;

    QJsonObject extra;
    extra.insert(QStringLiteral("mouse_px"), sum);
    extra.insert(QStringLiteral("window_ms"), m_mouseShockMs);
    maybeFire(QStringLiteral("mouse_shock"), extra);
}

void RageSmashWatchdog::noteImuSample(double mag, qint64 nowMs)
{
    if (mag <= 0.01)
        return;
    if (m_imuBaseline <= 0.01)
        m_imuBaseline = mag;
    else
        m_imuBaseline = m_imuBaseline * 0.92 + mag * 0.08;
    m_lastImuMs = nowMs;
    if (m_imuBaseline < 0.05)
        return;
    const double ratio = mag / m_imuBaseline;
    if (ratio < m_imuSpike || mag < m_imuBaseline + 0.35)
        return;

    QJsonObject extra;
    extra.insert(QStringLiteral("g"), mag);
    extra.insert(QStringLiteral("imu_ratio"), ratio);
    maybeFire(QStringLiteral("imu"), extra);
}

void RageSmashWatchdog::maybeFire(const QString &source, const QJsonObject &extra)
{
    if (!m_net || !m_watching || !clubEnabled())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastReportMs > 0 && now - m_lastReportMs < m_cooldownMs)
        return;

    const bool imu = (source == QLatin1String("imu"));
    const bool dropped = kdDropped();
    if (!imu && !dropped)
        return;

    m_lastReportMs = now;

    QJsonObject extraCopy = extra;
    QMetaObject::invokeMethod(this, [this, source, extraCopy]() {
        postIncident(source, extraCopy);
    }, Qt::QueuedConnection);
}

void RageSmashWatchdog::postIncident(const QString &source, const QJsonObject &extra)
{
    if (!m_net || !m_watching)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int killsW = 0;
    int deathsW = 0;
    for (const auto &ev : m_fragEvents) {
        if (now - ev.first > m_kdWindowMs)
            continue;
        if (ev.second == QLatin1String("death"))
            ++deathsW;
        else if (ev.second == QLatin1String("kill"))
            ++killsW;
    }
    const double kdNow = double(m_sessionKills) / double(qMax(1, m_sessionDeaths));

    QJsonObject payload = extra;
    payload.insert(QStringLiteral("source"), source);
    payload.insert(QStringLiteral("kills"), m_sessionKills);
    payload.insert(QStringLiteral("deaths"), m_sessionDeaths);
    payload.insert(QStringLiteral("kills_window"), killsW);
    payload.insert(QStringLiteral("deaths_window"), deathsW);
    payload.insert(QStringLiteral("kd_before"), m_kdPeak);
    payload.insert(QStringLiteral("kd_after"), kdNow);
    payload.insert(QStringLiteral("kd_dropped"), kdDropped());
    if (m_net->lastBookingId() > 0)
        payload.insert(QStringLiteral("booking_id"), m_net->lastBookingId());

    const QString pc = m_net->getCurrentPcName();
    const QString desc = QStringLiteral("Удар по столу на %1").arg(pc);
    qWarning() << "[RAGE-SMASH]" << source << "pc" << pc << "kd" << kdDropped();
    m_net->reportShellIncident(QStringLiteral("hardware_abuse"),
                               QStringLiteral("high"),
                               desc,
                               payload);
}

#ifdef Q_OS_WIN

void RageSmashWatchdog::handleMouse(WPARAM wParam, const MSLLHOOKSTRUCT *info)
{
    if (!m_watching || !info)
        return;
    if (info->flags & LLMHF_INJECTED)
        return;
    if (wParam != WM_MOUSEMOVE)
        return;
    static POINT last{};
    static bool have = false;
    if (!have) {
        last = info->pt;
        have = true;
        return;
    }
    const int dx = int(info->pt.x - last.x);
    const int dy = int(info->pt.y - last.y);
    last = info->pt;
    noteMouseDelta(dx, dy, QDateTime::currentMSecsSinceEpoch());
}

void RageSmashWatchdog::handleKey(WPARAM wParam, const KBDLLHOOKSTRUCT *info)
{
    if (!m_watching || !info)
        return;
    if (info->flags & LLKHF_INJECTED)
        return;
    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp)
        return;
    const quint32 vk = info->vkCode;
    if (vk == 0)
        return;
    if (isUp) {
        m_keyHeld.remove(vk);
        return;
    }
    if (m_keyHeld.value(vk, false))
        return;
    m_keyHeld.insert(vk, true);
    noteKeyDown(vk, QDateTime::currentMSecsSinceEpoch());
}

void RageSmashWatchdog::handleRawInput(LPARAM lParam)
{
    if (!m_watching)
        return;
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || size < sizeof(RAWINPUTHEADER))
        return;
    QByteArray buf(int(size), 0);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buf.data(), &size,
                        sizeof(RAWINPUTHEADER)) == UINT(-1))
        return;
    const auto *raw = reinterpret_cast<RAWINPUT *>(buf.data());
    if (!raw || raw->header.dwType != RIM_TYPEHID)
        return;

    const UINT packet = raw->data.hid.dwSizeHid;
    const UINT count = raw->data.hid.dwCount;
    if (packet < 6 || count < 1)
        return;
    const BYTE *bytes = raw->data.hid.bRawData;
    auto readAxis = [](const BYTE *p, int off) -> int {
        return int(qint16(quint16(p[off]) | (quint16(p[off + 1]) << 8)));
    };
    double best = 0;
    for (UINT i = 0; i < count; ++i) {
        const BYTE *p = bytes + i * packet;
        int start = 0;
        if (packet >= 7)
            start = 1; // report id
        if (start + 6 > int(packet))
            start = 0;
        if (start + 6 > int(packet))
            continue;
        const double x = readAxis(p, start);
        const double y = readAxis(p, start + 2);
        const double z = readAxis(p, start + 4);
        const double mag = std::sqrt(x * x + y * y + z * z) / 8192.0;
        if (mag > best)
            best = mag;
    }
    if (best > 0.02)
        noteImuSample(best, QDateTime::currentMSecsSinceEpoch());
}

void RageSmashWatchdog::installHooks()
{
    if (m_mouseHook || m_keyHook)
        return;
    g_rageSmash = this;
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, rageMouseProc, inst, 0);
    m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, rageKeyProc, inst, 0);
    if (!m_mouseHook || !m_keyHook) {
        qWarning() << "[RAGE-SMASH] SetWindowsHookEx failed err" << GetLastError();
        uninstallHooks();
    }
}

void RageSmashWatchdog::uninstallHooks()
{
    if (m_mouseHook) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
    if (m_keyHook) {
        UnhookWindowsHookEx(m_keyHook);
        m_keyHook = nullptr;
    }
    if (g_rageSmash == this)
        g_rageSmash = nullptr;
}

void RageSmashWatchdog::installRawInput()
{
    if (m_imuSink)
        return;
    m_imuSink = new ImuSink(this);
    HWND hwnd = reinterpret_cast<HWND>(m_imuSink->winId());
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x20; // HID Sensor — акселерометр/гироскоп игровых мышей
    rid.usUsage = 0;
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_PAGEONLY;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)))
        qWarning() << "[RAGE-SMASH] RegisterRawInputDevices failed" << GetLastError();
    m_imuSink->show();
}

void RageSmashWatchdog::uninstallRawInput()
{
    if (!m_imuSink)
        return;
    HWND hwnd = reinterpret_cast<HWND>(m_imuSink->winId());
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x20;
    rid.usUsage = 0;
    rid.dwFlags = RIDEV_REMOVE | RIDEV_PAGEONLY;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
    m_imuSink->deleteLater();
    m_imuSink = nullptr;
}

#else

void RageSmashWatchdog::installHooks() {}
void RageSmashWatchdog::uninstallHooks() {}
void RageSmashWatchdog::installRawInput() {}
void RageSmashWatchdog::uninstallRawInput() {}

#endif
