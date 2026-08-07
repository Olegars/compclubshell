#ifndef SESSIONALERTMANAGER_H
#define SESSIONALERTMANAGER_H

#include <QObject>
#include <QString>
#include <QTimer>

class SessionWarningToast;

class SessionAlertManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString timeRemaining READ timeRemaining NOTIFY timeRemainingChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged)

public:
    explicit SessionAlertManager(QObject *parent = nullptr);
    ~SessionAlertManager() override;

    QString timeRemaining() const { return m_timeRemaining; }
    bool sessionActive() const { return m_sessionActive; }

    /** Start / restart booking countdown from HH:MM:SS (or MM:SS). Resets warning thresholds. */
    Q_INVOKABLE void startSession(const QString &timeRemaining);
    /** Resync remaining from server without resetting warning flags already fired. */
    Q_INVOKABLE void syncTimeRemaining(const QString &timeRemaining);
    /** Stop countdown and hide toast (logout / session close). */
    Q_INVOKABLE void reset();
    /** Stub for «ПРОДЛИТЬ ВРЕМЯ» — toast + log, no API. */
    Q_INVOKABLE void requestExtendTime();

    /** Block SAPI/PowerShell TTS while voice-assistant reply is playing. */
    void setSpeechBlocked(bool blocked);
    bool speechBlocked() const { return m_speechBlocked; }

signals:
    void timeRemainingChanged();
    void sessionActiveChanged();
    void warningShown(int minutesLeft);
    void extendTimeRequested();
    /** Local countdown hit zero — shell should logout. */
    void sessionExpired();

private slots:
    void onTick();

private:
    static int parseTimeToSeconds(const QString &value);
    static QString formatSeconds(int totalSeconds);
    static QString minutesWord(int n);
    static QString warningPhrase(int minutes);

    void setTimeRemaining(const QString &value);
    void setSessionActive(bool active);
    void checkThresholdCrossings(int previousSeconds);
    void fireWarning(int minutes);
    void speakRussian(const QString &text);
    bool speakWithSapi(const QString &text);
    bool speakWithPowerShell(const QString &text);
    void showToast(const QString &text);

    QTimer m_tickTimer;
    QTimer m_toastTopmostTimer;
    SessionWarningToast *m_toast = nullptr;

    QString m_timeRemaining = QStringLiteral("00:00:00");
    int m_remainingSeconds = 0;
    bool m_sessionActive = false;
    bool m_speechBlocked = false;
    bool m_warned15 = false;
    bool m_warned10 = false;
    bool m_warned5 = false;
};

#endif // SESSIONALERTMANAGER_H
