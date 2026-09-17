#include "hardwarehealthwatchdog.h"

#include "hidinputmonitor.h"
#include "networkmanager.h"
#include "pathresolver.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QSettings>

#ifdef Q_OS_WIN

namespace {

HardwareHealthWatchdog *g_hwHealth = nullptr;

LRESULT CALLBACK hwHealthMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_hwHealth) {
        const MSLLHOOKSTRUCT *info = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
        if (info)
            g_hwHealth->handleMouse(wParam, info);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK hwHealthKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_hwHealth) {
        const KBDLLHOOKSTRUCT *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (info)
            g_hwHealth->handleKey(wParam, info);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

#endif

HardwareHealthWatchdog::HardwareHealthWatchdog(NetworkManager *net,
                                               HidInputMonitor *hid,
                                               QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_hid(hid)
{
    loadConfig();
    m_stuckTick.setInterval(1000);
    connect(&m_stuckTick, &QTimer::timeout, this, &HardwareHealthWatchdog::onStuckTick);
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

HardwareHealthWatchdog::~HardwareHealthWatchdog()
{
    stopWatch();
}

void HardwareHealthWatchdog::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("HardwareHealth/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_bounceGapMs = qBound(15, s.value(QStringLiteral("HardwareHealth/bounce_gap_ms"), 40).toInt(), 80);
    m_bounceHits = qBound(4, s.value(QStringLiteral("HardwareHealth/bounce_hits"), 10).toInt(), 40);
    m_bounceWindowMs = qBound(10000, s.value(QStringLiteral("HardwareHealth/bounce_window_ms"), 60000).toInt(), 300000);
    m_bounceSpreadMs = qBound(3000, s.value(QStringLiteral("HardwareHealth/bounce_spread_ms"), 15000).toInt(), 180000);
    m_chatterGapMs = qBound(15, s.value(QStringLiteral("HardwareHealth/chatter_gap_ms"), 35).toInt(), 80);
    m_chatterHits = qBound(4, s.value(QStringLiteral("HardwareHealth/chatter_hits"), 8).toInt(), 40);
    m_stuckDownMs = qBound(4000, s.value(QStringLiteral("HardwareHealth/stuck_down_ms"), 12000).toInt(), 60000);
    m_stuckOtherPresses = qBound(2, s.value(QStringLiteral("HardwareHealth/stuck_other_presses"), 4).toInt(), 20);
    m_cooldownMs = qBound(30000, s.value(QStringLiteral("HardwareHealth/cooldown_ms"), 600000).toInt(), 3600000);
}

bool HardwareHealthWatchdog::clubEnabled() const
{
    if (!m_enabled)
        return false;
    return !m_net || m_net->featureEnabled(QStringLiteral("hardware_health"));
}

void HardwareHealthWatchdog::startWatch()
{
    if (!clubEnabled() || m_watching)
        return;
    m_watching = true;
    resetState();
    installHooks();
    m_stuckTick.start();
    qDebug() << "[HW-HEALTH] watch started";
}

void HardwareHealthWatchdog::stopWatch()
{
    if (!m_watching) {
        uninstallHooks();
        return;
    }
    m_watching = false;
    m_stuckTick.stop();
    uninstallHooks();
    resetState();
    qDebug() << "[HW-HEALTH] watch stopped";
}

void HardwareHealthWatchdog::resetState()
{
    for (auto &btn : m_buttons)
        btn = ButtonState{};
    m_keys.clear();
}

bool HardwareHealthWatchdog::isHoldOkScan(quint32 scan)
{
    // Физические клавиши, которые в играх держат секундами (не путать с залипанием).
    switch (scan) {
    case 0x1D: // LCtrl
    case 0x9D: // RCtrl (make code)
    case 0x11D: // RCtrl (LL extended)
    case 0x2A: // LShift
    case 0x36: // RShift
    case 0x38: // LAlt
    case 0xB8: // RAlt (make code)
    case 0x138: // RAlt (LL extended)
    case 0x15B: // LWin (extended)
    case 0x15C: // RWin
    case 0x11: // W
    case 0x1E: // A
    case 0x1F: // S
    case 0x20: // D
    case 0x39: // Space
    case 0x148: // Up
    case 0x150: // Down
    case 0x14B: // Left
    case 0x14D: // Right
        return true;
    default:
        return false;
    }
}

QString HardwareHealthWatchdog::buttonName(int button)
{
    switch (button) {
    case 1: return QStringLiteral("right");
    case 2: return QStringLiteral("middle");
    default: return QStringLiteral("left");
    }
}

bool HardwareHealthWatchdog::windowTripped(QList<qint64> &times, qint64 nowMs,
                                           int hits, int windowMs, int spreadMs) const
{
    while (!times.isEmpty() && nowMs - times.first() > windowMs)
        times.removeFirst();
    if (times.size() < hits)
        return false;
    return (times.last() - times.first()) >= spreadMs;
}

void HardwareHealthWatchdog::noteMouseBounce(int button, qint64 nowMs)
{
    if (button < 0 || button > 2)
        return;
    ButtonState &st = m_buttons[button];
    st.bounceAt.append(nowMs);
    if (!windowTripped(st.bounceAt, nowMs, m_bounceHits, m_bounceWindowMs, m_bounceSpreadMs))
        return;

    QJsonObject extra;
    extra.insert(QStringLiteral("button"), buttonName(button));
    extra.insert(QStringLiteral("hits"), st.bounceAt.size());
    extra.insert(QStringLiteral("window_ms"), m_bounceWindowMs);
    extra.insert(QStringLiteral("spread_ms"), int(st.bounceAt.last() - st.bounceAt.first()));
    extra.insert(QStringLiteral("gap_ms"), m_bounceGapMs);
    queueIncident(QStringLiteral("mouse"), QStringLiteral("bounce"), extra);
    st.bounceAt.clear();
}

void HardwareHealthWatchdog::noteKeyChatter(quint32 scan, quint32 vk, qint64 nowMs)
{
    KeyState &st = m_keys[scan];
    st.chatterAt.append(nowMs);
    if (!windowTripped(st.chatterAt, nowMs, m_chatterHits, m_bounceWindowMs, m_bounceSpreadMs))
        return;

    QJsonObject extra;
    extra.insert(QStringLiteral("scan_code"), int(scan));
    extra.insert(QStringLiteral("vk"), int(vk ? vk : st.vk));
    extra.insert(QStringLiteral("hits"), st.chatterAt.size());
    extra.insert(QStringLiteral("window_ms"), m_bounceWindowMs);
    extra.insert(QStringLiteral("spread_ms"), int(st.chatterAt.last() - st.chatterAt.first()));
    queueIncident(QStringLiteral("keyboard"), QStringLiteral("chatter"), extra);
    st.chatterAt.clear();
}

void HardwareHealthWatchdog::noteKeyStuck(quint32 scan, quint32 vk, qint64 nowMs, qint64 holdMs)
{
    QJsonObject extra;
    extra.insert(QStringLiteral("scan_code"), int(scan));
    extra.insert(QStringLiteral("vk"), int(vk));
    extra.insert(QStringLiteral("hold_ms"), int(holdMs));
    extra.insert(QStringLiteral("other_presses"), m_keys.value(scan).otherPresses);
    extra.insert(QStringLiteral("detected_at"), nowMs);
    queueIncident(QStringLiteral("keyboard"), QStringLiteral("stuck_key"), extra);
    if (m_keys.contains(scan))
        m_keys[scan].otherPresses = 0;
}

void HardwareHealthWatchdog::queueIncident(const QString &kind, const QString &reason,
                                           const QJsonObject &extra)
{
    const QJsonObject copy = extra;
    QMetaObject::invokeMethod(this, [this, kind, reason, copy]() {
        postIncident(kind, reason, copy);
    }, Qt::QueuedConnection);
}

void HardwareHealthWatchdog::postIncident(const QString &kind, const QString &reason,
                                          const QJsonObject &extra)
{
    if (!m_net || !m_watching || !clubEnabled())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastReportMs > 0 && now - m_lastReportMs < m_cooldownMs)
        return;
    m_lastReportMs = now;

    const QString pc = m_net->getCurrentPcName();
    const QString desc = QStringLiteral("Проверить свитч/микрик на %1").arg(pc);

    QJsonObject payload = extra;
    payload.insert(QStringLiteral("kind"), kind);
    payload.insert(QStringLiteral("reason"), reason);
    payload.insert(QStringLiteral("pc"), pc);

    qWarning() << "[HW-HEALTH] anomaly" << kind << reason << "pc" << pc;
    m_net->reportShellIncident(QStringLiteral("hardware_switch_fault"),
                               QStringLiteral("high"),
                               desc,
                               payload);
}

void HardwareHealthWatchdog::onStuckTick()
{
    if (!m_watching)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
        KeyState &st = it.value();
        if (!st.down || isHoldOkScan(it.key()))
            continue;
        if (st.otherPresses < m_stuckOtherPresses)
            continue;
        const qint64 holdMs = now - st.downAt;
        if (holdMs < m_stuckDownMs)
            continue;
        noteKeyStuck(it.key(), st.vk, now, holdMs);
        st.down = false;
    }
}

#ifdef Q_OS_WIN

void HardwareHealthWatchdog::handleMouse(WPARAM wParam, const MSLLHOOKSTRUCT *info)
{
    if (!m_watching || !info)
        return;
    if (info->flags & LLMHF_INJECTED)
        return;

    int button = -1;
    bool down = false;
    switch (wParam) {
    case WM_LBUTTONDOWN: button = 0; down = true; break;
    case WM_LBUTTONUP:   button = 0; down = false; break;
    case WM_RBUTTONDOWN: button = 1; down = true; break;
    case WM_RBUTTONUP:   button = 1; down = false; break;
    case WM_MBUTTONDOWN: button = 2; down = true; break;
    case WM_MBUTTONUP:   button = 2; down = false; break;
    default:
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ButtonState &st = m_buttons[button];
    if (down) {
        const bool bounceFromDown = st.lastDownMs > 0 && (now - st.lastDownMs) <= m_bounceGapMs;
        const bool bounceFromUp = st.lastUpMs > 0 && (now - st.lastUpMs) <= m_bounceGapMs;
        st.lastDownMs = now;
        if (bounceFromDown || bounceFromUp)
            noteMouseBounce(button, now);
    } else {
        st.lastUpMs = now;
    }
}

void HardwareHealthWatchdog::handleKey(WPARAM wParam, const KBDLLHOOKSTRUCT *info)
{
    if (!m_watching || !info)
        return;
    if (info->flags & LLKHF_INJECTED)
        return;

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp)
        return;

    quint32 scan = info->scanCode & 0xFF;
    if (info->flags & LLKHF_EXTENDED)
        scan |= 0x100;
    if (scan == 0)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    KeyState &st = m_keys[scan];
    st.vk = info->vkCode;

    if (isDown) {
        if (st.down)
            return; // typematic, не дребезг
        const bool chatter = st.lastUpMs > 0 && (now - st.lastUpMs) <= m_chatterGapMs;
        st.down = true;
        st.downAt = now;
        st.otherPresses = 0;
        if (chatter)
            noteKeyChatter(scan, info->vkCode, now);
        return;
    }

    if (!st.down)
        return;
    st.down = false;
    st.lastUpMs = now;

    for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
        if (it.key() == scan || !it.value().down)
            continue;
        ++it.value().otherPresses;
    }
}

void HardwareHealthWatchdog::installHooks()
{
    if (m_mouseHook || m_keyHook)
        return;
    g_hwHealth = this;
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, hwHealthMouseProc, inst, 0);
    m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, hwHealthKeyboardProc, inst, 0);
    if (!m_mouseHook || !m_keyHook) {
        qWarning() << "[HW-HEALTH] SetWindowsHookEx failed mouse=" << (m_mouseHook != nullptr)
                   << "key=" << (m_keyHook != nullptr) << "err" << GetLastError();
        uninstallHooks();
    }
}

void HardwareHealthWatchdog::uninstallHooks()
{
    if (m_mouseHook) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
    if (m_keyHook) {
        UnhookWindowsHookEx(m_keyHook);
        m_keyHook = nullptr;
    }
    if (g_hwHealth == this)
        g_hwHealth = nullptr;
}

#else // !Q_OS_WIN

void HardwareHealthWatchdog::installHooks() {}
void HardwareHealthWatchdog::uninstallHooks() {}

#endif
