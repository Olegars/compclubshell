#ifndef STEAMAUTH_H
#define STEAMAUTH_H

#include "iplatformauth.h"
#include <QTimer>

class SteamAuth : public IPlatformAuth
{
    Q_OBJECT
public:
    explicit SteamAuth(QObject *parent = nullptr);
    ~SteamAuth() override;

    QString platformId() const override { return QStringLiteral("steam"); }

    void killLauncher() override;
    bool applyCache(const QJsonObject &authData) override;
    void startLauncher(QProcess *process,
                       const QJsonObject &authData,
                       const QString &appIdHint) override;
    void startScout(const QString &login, const QString &password) override;
    void stopScout() override;

    bool needsCacheBackup() const override { return m_needBackup; }
    void setNeedsCacheBackup(bool need) override { m_needBackup = need; }
    bool didInteractiveLogin() const override { return m_scoutInjected; }

    void backupCache(NetworkManager *net,
                     int terminalId,
                     const QString &login,
                     int accountId = 0,
                     int gameId = 0) override;

    QString launcherProcessName() const override { return QStringLiteral("steam.exe"); }

    static QString resolveAppId(const QJsonObject &authData, const QString &appIdHint);
    static QString steamInstallPath();

private:
    QString localAppDataSteamVdfPath() const;
    QString buildLoginUsersVdf(const QString &steamId,
                               const QString &login,
                               const QString &persona,
                               const QString &existing) const;

    // Personal: kill steam family + wait until gone (file locks).
    void killSteamAndWait(int timeoutMs = 8000);
    // Personal: hard logout — registry / VDF / ssfn / local.vdf connect-cache.
    void wipePersonalSession();
    static QString sanitizeLoginUsersForLogout(const QString &vdf);
    static QString sanitizeConfigVdfForLogout(const QString &vdf);

    QTimer *m_authScoutTimer = nullptr;
    bool m_needBackup = false;
    bool m_personalLaunch = false;
    int m_scoutTicks = 0;
    int m_scoutInjectTick = 0;
    bool m_scoutInjected = false;
    bool m_scoutAccountConfirmed = false;
};

#endif // STEAMAUTH_H
