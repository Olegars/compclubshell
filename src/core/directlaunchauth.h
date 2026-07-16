#ifndef DIRECTLAUNCHAUTH_H
#define DIRECTLAUNCHAUTH_H

#include "iplatformauth.h"

// Запуск exe/ярлыка без интерактивного логина (Epic/Riot/EA пока так;
// позже заменится на EpicAuth со scout при необходимости).
class DirectLaunchAuth : public IPlatformAuth
{
    Q_OBJECT
public:
    explicit DirectLaunchAuth(const QString &platformId, QObject *parent = nullptr);

    QString platformId() const override { return m_platformId; }

    void killLauncher() override;
    bool applyCache(const QJsonObject &authData) override;
    void startLauncher(QProcess *process,
                       const QJsonObject &authData,
                       const QString &appIdHint) override;
    void startScout(const QString &login, const QString &password) override;
    void stopScout() override;

    bool needsCacheBackup() const override { return false; }
    void setNeedsCacheBackup(bool) override {}
    bool didInteractiveLogin() const override { return false; }

    void backupCache(NetworkManager *, int, const QString &) override {}

    QString launcherProcessName() const override { return m_launcherImage; }

private:
    QString m_platformId;
    QString m_launcherImage;
};

#endif // DIRECTLAUNCHAUTH_H
