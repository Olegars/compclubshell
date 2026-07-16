#ifndef IPLATFORMAUTH_H
#define IPLATFORMAUTH_H

#include <QObject>
#include <QJsonObject>
#include <QProcess>
#include <QString>

class NetworkManager;

// Общий контракт авторизации лаунчера (Steam / Epic / Direct …).
// ProcessManager держит сессию окна игры; платформа — кэш, старт, scout, backup.
class IPlatformAuth : public QObject
{
    Q_OBJECT
public:
    explicit IPlatformAuth(QObject *parent = nullptr) : QObject(parent) {}
    ~IPlatformAuth() override = default;

    virtual QString platformId() const = 0;

    // Убить процессы лаунчера перед стартом (неблокирующе).
    virtual void killLauncher() = 0;

    // Применить кэш с сервера. true = тихий вход, scout не обязателен.
    virtual bool applyCache(const QJsonObject &authData) = 0;

    // Заполнить QProcess и запустить лаунчер/игру.
    virtual void startLauncher(QProcess *process,
                               const QJsonObject &authData,
                               const QString &appIdHint) = 0;

    virtual void startScout(const QString &login, const QString &password) = 0;
    virtual void stopScout() = 0;

    virtual bool needsCacheBackup() const = 0;
    virtual void setNeedsCacheBackup(bool need) = 0;
    virtual bool didInteractiveLogin() const = 0;

    // false — пока идёт интерактивный логин (не принимать UnrealWindow/игру раньше времени)
    virtual bool allowsGameDetect() const { return true; }

    virtual void backupCache(NetworkManager *net,
                             int terminalId,
                             const QString &login) = 0;

    // Имя процесса лаунчера для раннего exit-watch (например steam.exe).
    virtual QString launcherProcessName() const = 0;
};

#endif // IPLATFORMAUTH_H
