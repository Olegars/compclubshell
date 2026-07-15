#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QDebug>
#include <QUrl>
#include <QJsonObject>
#include <QVariantMap>

class GameModel;
class StoreModel;

class NetworkManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)
    Q_PROPERTY(int computerId READ computerId NOTIFY computerIdChanged)
    Q_PROPERTY(int lastBookingId READ lastBookingId NOTIFY lastBookingIdChanged)
public:
    explicit NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent = nullptr);

    bool isPcRegistered() const;
    QString serverUrl() const;
    int computerId() const;
    int lastBookingId() const { return m_lastBookingId; }

    QNetworkAccessManager* networkAccessManager() const { return m_networkManager; }
    void setRootQmlObject(QObject* rootObj) { m_rootQml = rootObj; }

    Q_INVOKABLE QString getMachineHwid() const;
    Q_INVOKABLE void fetchTerminalConfig(const QString &hwid);
    Q_INVOKABLE void checkTerminalStatus();
    Q_INVOKABLE QString getCurrentPcName();
    Q_INVOKABLE void registerStation(const QString &zoneType, const QString &pcName);
    Q_INVOKABLE void logoutTerminal(int terminalId);
    Q_INVOKABLE QString getLocalPath(const QString &remotePath, const QString &target);
    Q_INVOKABLE int getLatency(const QString &host);
    Q_INVOKABLE QStringList getAvailableZones();
    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchProducts();
    Q_INVOKABLE void login(const QString &phone, const QString &pin, int terminalId);
    Q_INVOKABLE void fetchOverlays(int terminalId);
    Q_INVOKABLE void freeGameAccount(int terminalId, int gameId);

signals:
    void pcRegistrationChanged();
    void authRequired();
    void setupRequired();
    void fileDownloaded(const QString &remotePath, const QString &localPath, const QString &target);
    void loginSucceeded(const QString &userName, double balance, const QString &timeRemaining, const QString &phone);
    void loginFailed(const QString &message);
    void loginRequestFinished();
    void overlaysReady(const QVariantMap &data);
    void freeAccountFinished(bool success);
    void computerIdChanged();
    void lastBookingIdChanged();

private:
    static QString cleanDigits(const QString &value);

    QNetworkAccessManager *m_networkManager;
    bool m_isPcRegistered;
    QString m_serverUrl;
    QString m_configFilePath;
    QString m_cachePath;
    QString m_hwid;
    QString m_pcNameString;
    int m_computerId;
    int m_lastBookingId;
    QStringList m_activeDownloads;

    GameModel* m_gamesModel;
    StoreModel* m_storeModel;
    QObject* m_rootQml;
};

#endif // NETWORKMANAGER_H
