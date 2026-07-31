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
#include <QVariantList>

class GameModel;
class StoreModel;

class NetworkManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)
    Q_PROPERTY(int computerId READ computerId NOTIFY computerIdChanged)
    Q_PROPERTY(int lastBookingId READ lastBookingId NOTIFY lastBookingIdChanged)
    Q_PROPERTY(int userId READ userId NOTIFY userIdChanged)
    Q_PROPERTY(QString featuredLabel READ featuredLabel NOTIFY featuredChanged)
    Q_PROPERTY(QString featuredMode READ featuredMode NOTIFY featuredChanged)
public:
    explicit NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent = nullptr);

    bool isPcRegistered() const;
    QString serverUrl() const;

    /**
     * Собирает базовый адрес бэкенда из Network/api_ip и Network/api_port.
     * В api_ip допустимы схема, порт и слэш ("https://0451.space/"), поэтому
     * простая склейка давала "http://https://0451.space:443".
     */
    static QString buildServerUrl(const QString &rawHost, const QString &rawPort);
    int computerId() const;
    int lastBookingId() const { return m_lastBookingId; }
    int userId() const { return m_userId; }
    QString featuredLabel() const { return m_featuredLabel; }
    QString featuredMode() const { return m_featuredMode; }

    QNetworkAccessManager* networkAccessManager() const { return m_networkManager; }
    void setRootQmlObject(QObject* rootObj) { m_rootQml = rootObj; }

    /** Second model for «Вы часто играете» / «Популярно в клубе». */
    void setFeaturedGamesModel(GameModel *model) { m_featuredGamesModel = model; }

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
    /** Poll shell order status for terminal (and optional order_id). Updates hasActiveOrder on Main.qml. */
    Q_INVOKABLE void checkOrderStatus(int terminalId = 0, int orderId = 0);
    Q_INVOKABLE void login(const QString &phone, const QString &pin, int terminalId);
    /** Poll spendable balance for the active shell session (no-op when logged out). */
    Q_INVOKABLE void refreshBalance();
    /** Create YooKassa embedded widget top-up; emits topUpReady(widgetUrl, ...). */
    Q_INVOKABLE void createTopUp(double amount);
    /** Pull payment status from YooKassa and credit wallet if paid (same as «Вернуться»). */
    Q_INVOKABLE void syncTopUpPayment(const QString &paymentId);
    Q_INVOKABLE void fetchOverlays(int terminalId);
    Q_INVOKABLE void freeGameAccount(int terminalId, int gameId);
    Q_INVOKABLE void recordGameLaunch(int gameId);
    Q_INVOKABLE void sendSos(const QString &reasonCode, const QString &reasonLabel);
    Q_INVOKABLE void clearSessionUser();
    /** Clear games catalog search filter (TextField cleared via Launcher signal). */
    void clearGamesSearch();

signals:
    void pcRegistrationChanged();
    void authRequired();
    void setupRequired();
    void fileDownloaded(const QString &remotePath, const QString &localPath, const QString &target);
    void loginSucceeded(const QString &userName, double balance, const QString &timeRemaining, const QString &phone);
    void loginFailed(const QString &message);
    void loginRequestFinished();
    /** Emitted only when polled balance differs from the last known value. */
    void balanceUpdated(double balance);
    void topUpReady(const QString &widgetUrl, const QString &paymentId, double amount);
    void topUpFailed(const QString &message);
    void overlaysReady(const QVariantMap &data);
    void freeAccountFinished(bool success);
    void computerIdChanged();
    void lastBookingIdChanged();
    void userIdChanged();
    void featuredChanged();
    void gamesLoaded();
    void sosSent(bool success);

private:
    static QString cleanDigits(const QString &value);
    static double jsonToDouble(const QJsonValue &value, double defaultValue = 0.0);
    static double userBalanceFromJson(const QJsonObject &user);
    void applyGamesPayload(const QJsonDocument &doc);
    void applyOrderStatusFromJson(const QJsonObject &rootObj);
    int resolveTerminalId(int terminalId) const;

    QNetworkAccessManager *m_networkManager;
    bool m_isPcRegistered;
    QString m_serverUrl;
    QString m_configFilePath;
    QString m_cachePath;
    QString m_hwid;
    QString m_pcNameString;
    int m_computerId;
    int m_lastBookingId;
    int m_userId = 0;
    double m_lastKnownBalance = -1.0;
    bool m_balanceRefreshInFlight = false;
    QString m_featuredLabel;
    QString m_featuredMode;
    QStringList m_activeDownloads;

    GameModel* m_gamesModel;
    GameModel* m_featuredGamesModel = nullptr;
    StoreModel* m_storeModel;
    QObject* m_rootQml;
};

#endif // NETWORKMANAGER_H
