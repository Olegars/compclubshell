#ifndef EAAUTH_H
#define EAAUTH_H

#include "iplatformauth.h"
#include <QByteArray>
#include <QTimer>

// EA App (EADesktop.exe): machine-cache + scout.
// Interactive: log-tail DesktopFSM + Tab только на login dialog; игра — origin2://.
class EaAuth : public IPlatformAuth
{
    Q_OBJECT
public:
    explicit EaAuth(QObject *parent = nullptr);
    ~EaAuth() override;

    QString platformId() const override { return QStringLiteral("ea"); }

    void killLauncher() override;
    bool applyCache(const QJsonObject &authData) override;
    void startLauncher(QProcess *process,
                       const QJsonObject &authData,
                       const QString &appIdHint) override;
    void startScout(const QString &login, const QString &password) override;
    void stopScout() override;

    bool needsCacheBackup() const override { return m_needBackup; }
    void setNeedsCacheBackup(bool need) override { m_needBackup = need; }
    bool didInteractiveLogin() const override { return m_passwordSent; }
    bool allowsGameDetect() const override { return m_allowsGameDetect; }

    void backupCache(NetworkManager *net,
                     int terminalId,
                     const QString &login) override;

    QString launcherProcessName() const override { return QStringLiteral("EADesktop.exe"); }

private:
    enum class Phase {
        WaitLoginWindow,
        EmailSubmitted,
        WaitPassword,
        PasswordSubmitted,
        WaitLoginGone,
        Done
    };

    // DesktopFSM из %LOCALAPPDATA%\...\EA Desktop\Logs\EADesktop.log
    enum class LogAuthState {
        Unknown,
        AwaitingAuth,
        Authenticated
    };

    void silentKill(const QString &image);
    void injectEmail(quintptr hwnd, const QString &email);
    void injectPassword(quintptr hwnd, const QString &password);
    void clickBackOnError(quintptr hwnd);
    void relaunchGameArgs();
    void fireGameUri();
    void finishScoutSuccess(const QString &why);
    static QString normalizeEaGameUri(const QString &args);
    static QString eaLauncherBeside(const QString &eaDesktopExe);
    static QString resolveDirectGameExe(const QString &args);
    static QStringList directGameExtraArgs(const QString &gameExe);

    void resetLogWatch();
    void pollEaLogs();
    void ingestEaLogChunk(const QByteArray &chunk);
    void applyEaLogLine(const QString &line);
    static QString findEaDesktopLog();
    static const char *logAuthStateName(LogAuthState s);

    QTimer *m_scoutTimer = nullptr;
    Phase m_phase = Phase::WaitLoginWindow;
    LogAuthState m_logAuth = LogAuthState::Unknown;
    int m_ticks = 0;
    int m_phaseTick = 0;
    int m_loginRetries = 0;
    bool m_needBackup = false;
    bool m_emailSent = false;
    bool m_passwordSent = false;
    bool m_errorBackClicked = false;
    bool m_allowsGameDetect = true;
    bool m_expectInteractive = true;
    bool m_gameUriDeferred = false;
    bool m_logReadyForActions = false;
    bool m_sawAwaitingAuth = false;
    qint64 m_logOffset = 0;
    QString m_logPath;
    QByteArray m_logCarry; // незавершённая строка между poll
    QString m_launcherExe;
    QString m_launchArgs;
};

#endif // EAAUTH_H
