#ifndef EPICAUTH_H
#define EPICAUTH_H

#include "iplatformauth.h"
#include <QTimer>

class EpicAuth : public IPlatformAuth
{
    Q_OBJECT
public:
    explicit EpicAuth(QObject *parent = nullptr);
    ~EpicAuth() override;

    QString platformId() const override { return QStringLiteral("epic"); }

    void killLauncher() override;
    bool applyCache(const QJsonObject &authData) override;
    void startLauncher(QProcess *process,
                       const QJsonObject &authData,
                       const QString &appIdHint) override;
    void startScout(const QString &login, const QString &password) override;
    void stopScout() override;

    bool needsCacheBackup() const override { return m_needBackup; }
    void setNeedsCacheBackup(bool need) override { m_needBackup = need; }
    bool didInteractiveLogin() const override { return m_emailSent; }
    bool allowsGameDetect() const override { return m_allowsGameDetect; }

    void backupCache(NetworkManager *net,
                     int terminalId,
                     const QString &login,
                     int accountId = 0,
                     int gameId = 0) override;

    QString launcherProcessName() const override { return QStringLiteral("EpicGamesLauncher.exe"); }

private:
    enum class Phase {
        WaitEmailDialog,
        EmailSubmitted,      // keyboard email+Continue in flight
        WaitPasswordDialog,
        PasswordSubmitted,
        Done
    };

    void silentKill(const QString &image);
    void injectEmail(quintptr hwnd, const QString &email);
    void injectPassword(quintptr hwnd, const QString &password);
    void relaunchGameUri();
    void scheduleSilentRelaunch();
    void pollSilentReadyThenRelaunch();
    void fallbackSilentToScout();
    void bumpLaunchGen();

    QTimer *m_scoutTimer = nullptr;
    Phase m_phase = Phase::WaitEmailDialog;
    int m_ticks = 0;
    int m_phaseTick = 0;
    int m_launchGen = 0;
    int m_silentRelaunchCount = 0;
    int m_silentUriFirstTick = 0;
    int m_silentConfidentTicks = 0;
    bool m_silentSawLibrary = false;
    bool m_needBackup = false;
    bool m_emailSent = false;
    bool m_passwordSent = false;
    quintptr m_loginHwnd = 0;
    bool m_allowsGameDetect = true;
    bool m_expectInteractive = true;
    QString m_launchUri;
    QString m_launcherExe;
    QString m_pendingLogin;
    QString m_pendingPassword;
};

#endif // EPICAUTH_H
