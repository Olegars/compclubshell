#include "voicehotkeymonitor.h"

#include <QDebug>

#ifdef Q_OS_WIN

namespace {

VoiceHotkeyMonitor *g_monitor = nullptr;

LRESULT CALLBACK voiceLowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_monitor && g_monitor->isEnabled()) {
        const KBDLLHOOKSTRUCT *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (info && int(info->vkCode) == g_monitor->virtualKey()
            && (info->flags & LLKHF_INJECTED) == 0) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                g_monitor->handleKeyDown();
                if (g_monitor->shouldSwallow())
                    return 1;
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                const bool swallow = g_monitor->shouldSwallow();
                g_monitor->handleKeyUp();
                if (swallow)
                    return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

void VoiceHotkeyMonitor::handleKeyDown()
{
    if (m_physicallyDown)
        return; // auto-repeat
    m_physicallyDown = true;
    m_holdConfirmed = false;
    m_holdTimer.start(m_holdMs);
}

void VoiceHotkeyMonitor::handleKeyUp()
{
    const bool wasConfirmed = m_holdConfirmed;
    m_holdTimer.stop();
    m_physicallyDown = false;
    m_holdConfirmed = false;
    if (wasConfirmed)
        emit holdEnded();
}

void VoiceHotkeyMonitor::onHoldTimeout()
{
    if (!m_physicallyDown || m_holdConfirmed)
        return;
    m_holdConfirmed = true;
    emit holdStarted();
}

void VoiceHotkeyMonitor::installHook()
{
    if (m_hook)
        return;
    g_monitor = this;
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, voiceLowLevelKeyboardProc,
                               GetModuleHandleW(nullptr), 0);
    if (!m_hook) {
        qWarning() << "[VOICE] SetWindowsHookEx WH_KEYBOARD_LL failed:" << GetLastError();
        g_monitor = nullptr;
    } else {
        qWarning() << "[VOICE] hotkey hook installed vk=" << Qt::hex << m_vk
                   << "holdMs=" << Qt::dec << m_holdMs;
    }
}

void VoiceHotkeyMonitor::uninstallHook()
{
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (g_monitor == this)
        g_monitor = nullptr;
    m_holdTimer.stop();
    m_physicallyDown = false;
    m_holdConfirmed = false;
}

#else // !Q_OS_WIN

void VoiceHotkeyMonitor::handleKeyDown() {}
void VoiceHotkeyMonitor::handleKeyUp() {}
void VoiceHotkeyMonitor::onHoldTimeout() {}
void VoiceHotkeyMonitor::installHook() {}
void VoiceHotkeyMonitor::uninstallHook() {}

#endif

VoiceHotkeyMonitor::VoiceHotkeyMonitor(QObject *parent)
    : QObject(parent)
{
    m_holdTimer.setSingleShot(true);
    connect(&m_holdTimer, &QTimer::timeout, this, &VoiceHotkeyMonitor::onHoldTimeout);
#ifdef Q_OS_WIN
    m_vk = VK_OEM_3;
#endif
}

VoiceHotkeyMonitor::~VoiceHotkeyMonitor()
{
    setEnabled(false);
}

void VoiceHotkeyMonitor::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (m_enabled)
        installHook();
    else
        uninstallHook();
}

void VoiceHotkeyMonitor::setVirtualKey(int vk)
{
    if (vk <= 0)
        return;
    m_vk = vk;
}

void VoiceHotkeyMonitor::setHoldMs(int ms)
{
    m_holdMs = qBound(50, ms, 2000);
}

int VoiceHotkeyMonitor::parseHotkeyName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n.isEmpty() || n == QLatin1String("grave") || n == QLatin1String("`")
        || n == QLatin1String("tilde") || n == QLatin1String("oem_3")) {
#ifdef Q_OS_WIN
        return VK_OEM_3;
#else
        return 0xC0;
#endif
    }
    if (n == QLatin1String("capslock") || n == QLatin1String("caps")) {
#ifdef Q_OS_WIN
        return VK_CAPITAL;
#else
        return 0x14;
#endif
    }
    if (n.startsWith(QLatin1Char('f')) && n.size() <= 3) {
        bool ok = false;
        const int fn = n.mid(1).toInt(&ok);
        if (ok && fn >= 1 && fn <= 12)
            return 0x70 + (fn - 1); // VK_F1
    }
#ifdef Q_OS_WIN
    if (n == QLatin1String("f13")) return VK_F13;
    if (n == QLatin1String("f14")) return VK_F14;
    if (n == QLatin1String("f15")) return VK_F15;
#endif
    qWarning() << "[VOICE] unknown hotkey name, using Grave:" << name;
#ifdef Q_OS_WIN
    return VK_OEM_3;
#else
    return 0xC0;
#endif
}
