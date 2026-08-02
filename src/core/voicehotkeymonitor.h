#ifndef VOICEHOTKEYMONITOR_H
#define VOICEHOTKEYMONITOR_H

#include <QObject>
#include <QTimer>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

/**
 * Global hold-to-talk via WH_KEYBOARD_LL.
 * Short taps pass through; after holdMs the key is swallowed until release.
 */
class VoiceHotkeyMonitor : public QObject
{
    Q_OBJECT
public:
    explicit VoiceHotkeyMonitor(QObject *parent = nullptr);
    ~VoiceHotkeyMonitor() override;

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void setVirtualKey(int vk);
    int virtualKey() const { return m_vk; }

    void setHoldMs(int ms);
    int holdMs() const { return m_holdMs; }

    /** Parse config names: Grave, F1..F12, CapsLock, etc. */
    static int parseHotkeyName(const QString &name);

    // Called from LL-hook (same thread as Qt event loop when installed from main).
    void handleKeyDown();
    void handleKeyUp();
    bool shouldSwallow() const { return m_holdConfirmed; }

signals:
    void holdStarted();
    void holdEnded();

private slots:
    void onHoldTimeout();

private:
    void installHook();
    void uninstallHook();

    bool m_enabled = false;
    int m_vk = 0xC0; // VK_OEM_3 Grave
    int m_holdMs = 250;
    bool m_physicallyDown = false;
    bool m_holdConfirmed = false;
    QTimer m_holdTimer;

#ifdef Q_OS_WIN
    HHOOK m_hook = nullptr;
#endif
};

#endif // VOICEHOTKEYMONITOR_H
