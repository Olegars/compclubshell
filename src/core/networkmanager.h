#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <QObject>
#include <QStringList>
#include <QSettings>
#include <QCoreApplication>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>

class GameModel;
class StoreModel;

class NetworkManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isPcRegistered READ isPcRegistered NOTIFY pcRegistrationChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)

public:
    explicit NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent = nullptr);

    bool isPcRegistered() const;
    QString serverUrl() const;

    void fetchTerminalConfig(const QString &hwid);

    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchProducts();
    Q_INVOKABLE QString getLocalPath(const QString &remoteUrl, const QString &blockId);

    Q_INVOKABLE QStringList getAvailableZones();
    Q_INVOKABLE QString getCurrentPcName();

    Q_INVOKABLE void registerStation(const QString &zoneType);
    Q_INVOKABLE void checkTerminalStatus();

signals:
    void pcRegistrationChanged();
    void setupRequired();
    void authRequired();

private:
    bool m_isPcRegistered;
    QString m_configFilePath;
    QString m_serverUrl;
    QString m_hwid;
    QString m_pcNameString; // Хранение имени ПК, полученного из базы данных

    GameModel* m_gamesModel;
    StoreModel* m_storeModel;

    QNetworkAccessManager* m_networkManager;

    void loadConfigStatus();
};

#endif // NETWORKMANAGER_H