#ifndef RIOTAUTH_H
#define RIOTAUTH_H

#include "iplatformauth.h"
#include <QTimer>

class QNetworkAccessManager;

// Riot Client: DirectLaunch + Tab-scout логин/пароль (одна форма).
class RiotAuth : public IPlatformAuth
{
    Q_OBJECT
public:
    explicit RiotAuth(QObject *parent = nullptr);
    ~RiotAuth() override;

    QString platformId() const override { return QStringLiteral("riot"); }

    void killLauncher() override;
    bool applyCache(const QJsonObject &authData) override;
    void startLauncher(QProcess *process,
                       const QJsonObject &authData,
                       const QString &appIdHint) override;
    void startScout(const QString &login, const QString &password) override;
    void stopScout() override;

    bool needsCacheBackup() const override { return false; }
    void setNeedsCacheBackup(bool) override {}
    bool didInteractiveLogin() const override { return m_credentialsSent; }
    bool allowsGameDetect() const override { return m_allowsGameDetect; }

    void backupCache(NetworkManager *, int, const QString &) override {}

    QString launcherProcessName() const override
    {
        return QStringLiteral("RiotClientServices.exe");
    }

private:
    enum class Phase {
        WaitLoginWindow,
        WaitStableSize,
        TypeUsername,
        TabToPassword,
        TypePassword,
        TabToSubmit,
        PressEnter,
        Done
    };

    void silentKill(const QString &image);
    void clearLocalSession();
    void scheduleProductLaunch();
    void pollSessionThenLaunchProduct();
    void shellExecuteProductOnce(const char *why);
    void shellExecuteRiotProtocol(const char *why);
    void dismissRiotModalsSoft(const char *why);
    void launchGameExeDirect(const char *why);
    bool clickPlayButton(const char *why);
    void dismissClientOverlay(const char *why);
    void dismissAlreadyRunningDialog(const char *why);
    void dismissAccessDeniedDialog(const char *why);
    void skipLolTutorialViaLcu(const char *why);
    void openLeagueLobbyViaLcu(const char *why);
    void ensureSingleLeagueClient();
    void notifyProductReady();
    void launchProductViaApi(const char *why);
    void finishLaunchNudge(const char *why);
    void discoverRiotLaunchPaths(const char *why);
    void postRiotApi(const QString &path, const QByteArray &body, const char *why);
    bool readRiotLockfile(int *port, QString *password, QString *protocol) const;
    bool readLeagueLockfile(int *port, QString *password, QString *protocol) const;
    bool isGameProcessRunning() const;
    bool parseProductArgs(QString *productId, QString *patchline) const;
    QString sessionSettingsPath() const;
    QString resolveGameExePath() const;
    QNetworkAccessManager *ensureNam();
    void parkRiotOffscreen(quintptr hwnd, const char *why);
    void restoreRiotOnscreen(const char *why);
    void keepShellOverlayUp();
    void keepOverlayOverRiot(quintptr hwnd = 0);

    QTimer *m_scoutTimer = nullptr;
    QTimer *m_sessionPollTimer = nullptr;
    Phase m_phase = Phase::WaitLoginWindow;
    int m_ticks = 0;
    int m_phaseTick = 0;
    int m_stableCount = 0;
    int m_lastW = 0;
    int m_lastH = 0;
    int m_sessionPollTicks = 0;
    int m_rsoRetryCount = 0;
    quintptr m_loginHwnd = 0;
    // Сохранённый rect Riot Client до park за экран
    int m_riotSavedX = 0;
    int m_riotSavedY = 0;
    int m_riotSavedW = 0;
    int m_riotSavedH = 0;
    quintptr m_riotParkedHwnd = 0;
    bool m_riotParkedOffscreen = false;
    bool m_credentialsSent = false;
    bool m_allowsGameDetect = true;
    bool m_expectInteractive = true;
    bool m_productLaunchScheduled = false;
    bool m_productLaunchOk = false;
    bool m_playClickDone = false;
    bool m_playClickStop = false; // уже кликнули Play / игра жива — больше не жать
    bool m_overlayDismissScheduled = false;
    bool m_overlayDismissed = false;
    bool m_lcuTutorialSkipDone = false;
    bool m_leagueHeaderPlayDone = false;
    int m_headerPlayAttempts = 0;
    bool m_leagueLobbyPosted = false;
    int m_lobbyPostAttempts = 0;
    bool m_launchAborted = false; // stopScout/kill — отменить отложенные Play/LCU
    QString m_launcherExe;
    QString m_productArgs;
    QString m_gameTitle;
    QString m_login;
    QString m_password;
    QNetworkAccessManager *m_nam = nullptr;
};

#endif // RIOTAUTH_H
