#ifndef RAGESMASHWATCHDOG_H
#define RAGESMASHWATCHDOG_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QWindow>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

class NetworkManager;
class HidInputMonitor;

/**
 * Детекция удара по столу: IMU игровой мыши (HID Sensor / гироскоп),
 * резкий сноп клавиш (10+ за 100 мс) и/или ударный рывок курсора —
 * вместе с падением K/D по GSI. Тикет hardware_abuse + оверлей «напиток».
 */
class RageSmashWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit RageSmashWatchdog(NetworkManager *net,
                               HidInputMonitor *hid,
                               QObject *parent = nullptr);
    ~RageSmashWatchdog() override;

#ifdef Q_OS_WIN
    void handleMouse(WPARAM wParam, const MSLLHOOKSTRUCT *info);
    void handleKey(WPARAM wParam, const KBDLLHOOKSTRUCT *info);
    void handleRawInput(LPARAM lParam);
#endif

    void onGsiEvent(const QJsonObject &payload);

private:
    class ImuSink;

    void loadConfig();
    bool clubEnabled() const;
    void startWatch();
    void stopWatch();
    void resetState();
    void noteKeyDown(quint32 vk, qint64 nowMs);
    void noteMouseDelta(int dx, int dy, qint64 nowMs);
    void noteImuSample(double mag, qint64 nowMs);
    bool kdDropped() const;
    void maybeFire(const QString &source, const QJsonObject &extra);
    void postIncident(const QString &source, const QJsonObject &extra);
    void installHooks();
    void uninstallHooks();
    void installRawInput();
    void uninstallRawInput();

    NetworkManager *m_net = nullptr;
    HidInputMonitor *m_hid = nullptr;
    ImuSink *m_imuSink = nullptr;

    bool m_enabled = true;
    bool m_watching = false;
    int m_mashKeys = 10;
    int m_mashWindowMs = 100;
    int m_mouseShockPx = 9000;
    int m_mouseShockMs = 80;
    double m_imuSpike = 2.4;
    int m_deathNeed = 2;
    int m_kdWindowMs = 45000;
    int m_cooldownMs = 180000;
    qint64 m_lastReportMs = 0;

    QList<QPair<qint64, quint32>> m_keyDowns;
    QHash<quint32, bool> m_keyHeld;
    QList<QPair<qint64, int>> m_mouseMoves;
    QList<QPair<qint64, QString>> m_fragEvents;
    int m_sessionKills = 0;
    int m_sessionDeaths = 0;
    double m_kdPeak = 0;
    double m_imuBaseline = 0;
    qint64 m_lastImuMs = 0;

#ifdef Q_OS_WIN
    HHOOK m_mouseHook = nullptr;
    HHOOK m_keyHook = nullptr;
#endif
};

#endif
