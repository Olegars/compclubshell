#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QDebug>

class GameModel;
class StoreModel;

class NetworkManager : public QObject
{
    Q_OBJECT

public:
    explicit NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent = nullptr);

    bool isPcRegistered() const;

    // Регистрация корневого QML объекта для QMetaObject::invokeMethod
    void setRootQmlObject(QObject* root) { m_rootQml = root; }

    // === СИСТЕМНЫЕ МЕТОДЫ, ДОСТУПНЫЕ ДЛЯ ВЫЗОВА ИЗ QML ЧЕРЕЗ Q_INVOKABLE ===
    Q_INVOKABLE QString serverUrl() const; // ИСПРАВЛЕНО: Теперь метод виден в JS как функция!
    Q_INVOKABLE void fetchTerminalConfig(const QString &hwid);
    Q_INVOKABLE void checkTerminalStatus();
    Q_INVOKABLE void registerStation(const QString &zoneType);
    Q_INVOKABLE QString getLocalPath(const QString &remotePath, const QString &target = "");
    Q_INVOKABLE int getLatency(const QString &host);
    Q_INVOKABLE QString getCurrentPcName();
    Q_INVOKABLE QStringList getAvailableZones();

    // Заглушки синхронизации игровых сессий
    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchProducts();

signals:
    void pcRegistrationChanged();
    void authRequired();
    void setupRequired();

    // Сигнал кэширования для оверлеев
    void fileDownloaded(const QString &remoteUrl, const QString &localPath, const QString &target);

private:
    QNetworkAccessManager *m_networkManager;
    QString m_configFilePath;
    QString m_serverUrl;
    QString m_hwid;
    QString m_pcNameString;
    bool m_isPcRegistered;

    QString m_cachePath;
    QStringList m_activeDownloads; // Защита от параллельного скачивания

    QObject* m_rootQml;
    GameModel* m_gamesModel;
    StoreModel* m_storeModel;
};

#endif // NETWORKMANAGER_H