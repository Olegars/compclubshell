#ifndef HARDWAREHEALTHWATCHDOG_H
#define HARDWAREHEALTHWATCHDOG_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

class NetworkManager;
class HidInputMonitor;

/**
 * Логический слой поверх HID-сессии: дребезг микрика мыши (фантомный
 * дабл-клик) и залипание/дребезг конкретного скан-кода клавиши.
 * Тикет в админку: «Проверить свитч/микрик на ПК-ХХ».
 */
class HardwareHealthWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit HardwareHealthWatchdog(NetworkManager *net,
                                    HidInputMonitor *hid,
                                    QObject *parent = nullptr);
    ~HardwareHealthWatchdog() override;

    bool faultLatched() const { return m_faultLatched; }

#ifdef Q_OS_WIN
    void handleMouse(WPARAM wParam, const MSLLHOOKSTRUCT *info);
    void handleKey(WPARAM wParam, const KBDLLHOOKSTRUCT *info);
#endif

private slots:
    void onStuckTick();

private:
    struct ButtonState {
        qint64 lastDownMs = 0;
        qint64 lastUpMs = 0;
        QList<qint64> bounceAt;
    };

    struct KeyState {
        bool down = false;
        quint32 vk = 0;
        qint64 downAt = 0;
        qint64 lastUpMs = 0;
        int otherPresses = 0;
        QList<qint64> chatterAt;
    };

    void loadConfig();
    bool clubEnabled() const;
    void startWatch();
    void stopWatch();
    void resetState();
    void noteMouseBounce(int button, qint64 nowMs);
    void noteKeyChatter(quint32 scan, quint32 vk, qint64 nowMs);
    void noteKeyStuck(quint32 scan, quint32 vk, qint64 nowMs, qint64 holdMs);
    bool windowTripped(QList<qint64> &times, qint64 nowMs, int hits, int windowMs, int spreadMs) const;
    void queueIncident(const QString &kind, const QString &reason, const QJsonObject &extra);
    void postIncident(const QString &kind, const QString &reason, const QJsonObject &extra);
    static bool isHoldOkScan(quint32 scan);
    static QString buttonName(int button);

    void installHooks();
    void uninstallHooks();

    NetworkManager *m_net = nullptr;
    HidInputMonitor *m_hid = nullptr;
    QTimer m_stuckTick;

    bool m_enabled = true;
    bool m_watching = false;
    int m_bounceGapMs = 40;
    int m_bounceHits = 10;
    int m_bounceWindowMs = 60000;
    int m_bounceSpreadMs = 15000;
    int m_chatterGapMs = 35;
    int m_chatterHits = 8;
    int m_stuckDownMs = 12000;
    int m_stuckOtherPresses = 4;
    int m_cooldownMs = 600000;
    qint64 m_lastReportMs = 0;
    bool m_faultLatched = false;

    ButtonState m_buttons[3];
    QHash<quint32, KeyState> m_keys;

#ifdef Q_OS_WIN
    HHOOK m_mouseHook = nullptr;
    HHOOK m_keyHook = nullptr;
#endif
};

#endif
