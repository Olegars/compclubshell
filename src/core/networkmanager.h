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
#include <QTimer>

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
    Q_PROPERTY(QVariantList quickApps READ quickApps NOTIFY quickAppsChanged)
    Q_PROPERTY(bool fanAvailable READ fanAvailable NOTIFY fanStateChanged)
    Q_PROPERTY(bool fanOn READ fanOn NOTIFY fanStateChanged)
    Q_PROPERTY(QString fanMode READ fanMode NOTIFY fanStateChanged)
    Q_PROPERTY(double cpuTempC READ cpuTempC NOTIFY cpuTempChanged)
    Q_PROPERTY(QString zoneName READ zoneName NOTIFY zoneInfoChanged)
    Q_PROPERTY(QString zoneSlug READ zoneSlug NOTIFY zoneInfoChanged)
    Q_PROPERTY(QString zoneColor READ zoneColor NOTIFY zoneInfoChanged)
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
    QVariantList quickApps() const { return m_quickApps; }
    bool fanAvailable() const { return m_fanAvailable; }
    bool fanOn() const { return m_fanOn; }
    QString fanMode() const { return m_fanMode; }
    double cpuTempC() const { return m_cpuTempC; }
    QString zoneName() const { return m_zoneName; }
    QString zoneSlug() const { return m_zoneSlug; }
    QString zoneColor() const { return m_zoneColor; }

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
    /** true если локальный mp4 достаточно лёгкий для UI-потока (~логин/телефон). */
    Q_INVOKABLE bool isLocalMediaLight(const QString &qmlOrLocalPath,
                                       qint64 maxBytes = 8 * 1024 * 1024) const;
    Q_INVOKABLE int getLatency(const QString &host);
    Q_INVOKABLE QStringList getAvailableZones();
    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchQuickApps();
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
    /** Старт опроса температуры + состояния вентилятора (после логина). */
    Q_INVOKABLE void startClimateControl();
    /** Остановка опроса (логаут / пауза). */
    Q_INVOKABLE void stopClimateControl();
    /** Ручное управление: on | off | auto. */
    Q_INVOKABLE void setFan(const QString &action);
    Q_INVOKABLE void fetchFanState();
    Q_INVOKABLE void reportThermalNow();
    /** Heartbeat питания: last_seen + MAC → power_desired / session_active. */
    Q_INVOKABLE void startPowerHeartbeat();
    Q_INVOKABLE void stopPowerHeartbeat();
    Q_INVOKABLE void sendPowerHeartbeat();
    /** aboutToQuit: сразу сказать бэкенду power_state=off. */
    Q_INVOKABLE void notifyPowerOffline();
    /** Clear games catalog search filter (TextField cleared via Launcher signal). */
    void clearGamesSearch();

    /** Hold-to-talk voice AI: multipart upload → JSON with audio_base64 MP3. */
    void askAiAssistant(int terminalId, const QString &audioPath,
                        int gameId = 0, const QString &gameTitle = QString());
    void abortAiAssistant();

    /** Personalized spoken greeting after login (DeepSeek + TTS). */
    void requestVoiceGreeting(int terminalId, int bookingId = 0);
    void abortVoiceGreeting();

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
    void quickAppsChanged();
    void sosSent(bool success);
    void fanStateChanged();
    void cpuTempChanged();
    void zoneInfoChanged();
    /** Backend asks shell to reboot or shutdown after session / idle policy. */
    void powerActionRequested(const QString &action);
    /** Scheduler closed the booking while shell still showed a logged-in user. */
    void sessionForceEnded();
    void aiAssistantSucceeded(const QByteArray &audioBytes, const QString &mime,
                              const QString &transcript, const QString &replyText);
    void aiAssistantFailed(const QString &message);
    void voiceGreetingSucceeded(const QByteArray &audioBytes, const QString &mime,
                                const QString &replyText, bool isFirstVisit);
    void voiceGreetingFailed(const QString &message);

private:
    static QString cleanDigits(const QString &value);
    static double jsonToDouble(const QJsonValue &value, double defaultValue = 0.0);
    static double userBalanceFromJson(const QJsonObject &user);
    void applyGamesPayload(const QJsonDocument &doc);
    void applyOrderStatusFromJson(const QJsonObject &rootObj);
    int resolveTerminalId(int terminalId) const;
    void applyFanStateFromJson(const QJsonObject &fanObj);
    void postThermal(double cpuC);
    QString primaryMacAddress() const;
    bool isLocalSessionActive() const;
    void handlePowerPolicy(const QString &desired, const QString &action, bool sessionActive);

    QNetworkAccessManager *m_networkManager;
    QTimer *m_climateTimer = nullptr;
    QTimer *m_powerHeartbeatTimer = nullptr;
    bool m_isPcRegistered;
    QString m_serverUrl;
    QString m_configFilePath;
    QString m_cachePath;
    QString m_hwid;
    QString m_pcNameString;
    QString m_cachedMac;
    int m_computerId;
    int m_lastBookingId;
    int m_userId = 0;
    double m_lastKnownBalance = -1.0;
    bool m_balanceRefreshInFlight = false;
    bool m_powerHeartbeatInFlight = false;
    bool m_sawActiveSession = false;
    bool m_idleShutdownRequested = false;
    QString m_featuredLabel;
    QString m_featuredMode;
    QStringList m_activeDownloads;
    QVariantList m_quickApps;
    bool m_overlaysFetchInFlight = false;
    int m_overlaysQueuedTerminalId = -1;
    bool m_fanAvailable = false;
    bool m_fanOn = false;
    QString m_fanMode;
    double m_cpuTempC = -1.0;
    QString m_zoneName;
    QString m_zoneSlug;
    QString m_zoneColor;
    bool m_climateActive = false;
    bool m_fanRequestInFlight = false;
    bool m_thermalRequestInFlight = false;
    QNetworkReply *m_aiAssistantReply = nullptr;
    QNetworkReply *m_voiceGreetingReply = nullptr;

    GameModel* m_gamesModel;
    GameModel* m_featuredGamesModel = nullptr;
    StoreModel* m_storeModel;
    QObject* m_rootQml;
};

#endif // NETWORKMANAGER_H
