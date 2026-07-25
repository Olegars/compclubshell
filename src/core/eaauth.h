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
                     const QString &login,
                     int accountId = 0,
                     int gameId = 0) override;

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
    void fireGameUri(bool protocolRetry = false);
    void finishScoutSuccess(const QString &why);
    void scheduleLibraryReadyLaunch(const QString &why);
    void stopLibraryReadyWatch();
    bool isEaLibraryReadyUi() const;
    bool isClubGameLikelyRunning() const;
    void fireOrigin2Protocol(const QString &uri, bool isRetry);
    void warnTitleOfferMismatch() const;
    static QString normalizeEaGameUri(const QString &args);
    static QString eaLauncherBeside(const QString &eaDesktopExe);
    static QString resolveDirectGameExe(const QString &args);
    static bool directExeMatchesGame(const QString &gameExe,
                                     const QString &launchArgs,
                                     const QString &gameTitle);
    static QStringList directGameExtraArgs(const QString &gameExe);
    void startPersonalLoginWatch();

    void resetLogWatch();
    void pollEaLogs();
    void ingestEaLogChunk(const QByteArray &chunk);
    void applyEaLogLine(const QString &line);
    static QString findEaDesktopLog();
    static const char *logAuthStateName(LogAuthState s);

    // Overlay TOPMOST: never during SendInput; settle after Enter; throttle while idle.
    void keepOverlayUp(bool force);
    void beginInjectBurst();
    void endInjectBurstRestoreOverlay(int delayMs = 400);

    QTimer *m_scoutTimer = nullptr;
    QTimer *m_libraryReadyTimer = nullptr;
    Phase m_phase = Phase::WaitLoginWindow;
    LogAuthState m_logAuth = LogAuthState::Unknown;
    int m_ticks = 0;
    int m_phaseTick = 0;
    int m_loginRetries = 0;
    int m_libraryReadyTicks = 0;
    int m_libraryReadySinceTick = -1;
    bool m_needBackup = false;
    bool m_emailSent = false;
    bool m_passwordSent = false;
    bool m_errorBackClicked = false;
    bool m_allowsGameDetect = true;
    bool m_expectInteractive = true;
    bool m_personalLaunch = false; // «Свой аккаунт»: wipe + EADesktop, без scout/URI/DIRECT
    bool m_gameUriDeferred = false;
    bool m_logReadyForActions = false;
    bool m_sawAwaitingAuth = false;
    bool m_personalEarlyAuthWarned = false;
    bool m_origin2Fired = false;
    bool m_origin2Retried = false;
    bool m_sendInputBusy = false;
    qint64 m_overlayHoldOffUntilMs = 0;
    qint64 m_lastOverlayRaiseMs = 0;
    qint64 m_logOffset = 0;
    qint64 m_origin2FiredAtMs = 0;
    QString m_logPath;
    QByteArray m_logCarry; // незавершённая строка между poll
    QString m_launcherExe;
    QString m_launchArgs;
    QString m_gameTitle;
};

#endif // EAAUTH_H
