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
#include <QVector>

class GameModel;
class StoreModel;

struct FanRelayEndpoint {
    QString host;
    int port = 30000;
    int channel = 0;
    int channel2 = 0;
    int fanId = 0;
};

class NetworkManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)
    Q_PROPERTY(int computerId READ computerId NOTIFY computerIdChanged)
    Q_PROPERTY(int lastBookingId READ lastBookingId NOTIFY lastBookingIdChanged)
    Q_PROPERTY(int userId READ userId NOTIFY userIdChanged)
    Q_PROPERTY(QString pendingReceiptUrl READ pendingReceiptUrl NOTIFY pendingReceiptChanged)
    Q_PROPERTY(double pendingReceiptAmount READ pendingReceiptAmount NOTIFY pendingReceiptChanged)
    Q_PROPERTY(bool pendingReceiptStub READ pendingReceiptStub NOTIFY pendingReceiptChanged)
    Q_PROPERTY(QString pendingReceiptDescription READ pendingReceiptDescription NOTIFY pendingReceiptChanged)
    Q_PROPERTY(QString featuredLabel READ featuredLabel NOTIFY featuredChanged)
    Q_PROPERTY(QString featuredMode READ featuredMode NOTIFY featuredChanged)
    Q_PROPERTY(QVariantList quickApps READ quickApps NOTIFY quickAppsChanged)
    Q_PROPERTY(bool fanAvailable READ fanAvailable NOTIFY fanStateChanged)
    Q_PROPERTY(bool fanOn READ fanOn NOTIFY fanStateChanged)
    Q_PROPERTY(QString fanMode READ fanMode NOTIFY fanStateChanged)
    Q_PROPERTY(int fanSpeed READ fanSpeed NOTIFY fanStateChanged)
    Q_PROPERTY(int fanManualLockSec READ fanManualLockSec NOTIFY fanStateChanged)
    Q_PROPERTY(QString fanDebug READ fanDebug NOTIFY fanDebugChanged)
    Q_PROPERTY(QVariantList fanDiscoverBoards READ fanDiscoverBoards NOTIFY fanDiscoverChanged)
    Q_PROPERTY(QVariantList fanDiscoverBound READ fanDiscoverBound NOTIFY fanDiscoverChanged)
    Q_PROPERTY(QString fanDiscoverStatus READ fanDiscoverStatus NOTIFY fanDiscoverChanged)
    Q_PROPERTY(QString fanDiscoverSpaceName READ fanDiscoverSpaceName NOTIFY fanDiscoverChanged)
    Q_PROPERTY(bool fanDiscoverBusy READ fanDiscoverBusy NOTIFY fanDiscoverChanged)
    Q_PROPERTY(int fanDiscoverSlotsUsed READ fanDiscoverSlotsUsed NOTIFY fanDiscoverChanged)
    Q_PROPERTY(int fanDiscoverSlotsMax READ fanDiscoverSlotsMax NOTIFY fanDiscoverChanged)
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
    QString pendingReceiptUrl() const { return m_pendingReceiptUrl; }
    double pendingReceiptAmount() const { return m_pendingReceiptAmount; }
    bool pendingReceiptStub() const { return m_pendingReceiptStub; }
    QString pendingReceiptDescription() const { return m_pendingReceiptDescription; }
    Q_INVOKABLE void clearPendingReceipt();
    QString featuredLabel() const { return m_featuredLabel; }
    QString featuredMode() const { return m_featuredMode; }
    QVariantList quickApps() const { return m_quickApps; }
    bool fanAvailable() const { return m_fanAvailable; }
    bool fanOn() const { return m_fanOn; }
    QString fanMode() const { return m_fanMode; }
    int fanSpeed() const { return m_fanDesiredPower; }
    int fanManualLockSec() const { return m_fanManualLockSec; }
    QString fanDebug() const { return m_fanDebug; }
    QVariantList fanDiscoverBoards() const { return m_fanDiscoverBoards; }
    QVariantList fanDiscoverBound() const { return m_fanDiscoverBound; }
    QString fanDiscoverStatus() const { return m_fanDiscoverStatus; }
    QString fanDiscoverSpaceName() const { return m_fanDiscoverSpaceName; }
    bool fanDiscoverBusy() const { return m_fanDiscoverBusy || m_fanTestInFlight; }
    int fanDiscoverSlotsUsed() const { return m_fanDiscoverSlotsUsed; }
    int fanDiscoverSlotsMax() const { return m_fanDiscoverSlotsMax; }
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
    Q_INVOKABLE void createTopUp(double amount, bool sendReceipt = false);
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
    /** Ручное управление: on | off | auto | 50 | 75 | 100. */
    Q_INVOKABLE void setFan(const QString &action);
    Q_INVOKABLE void fetchFanState();
    Q_INVOKABLE void reportThermalNow();
    Q_INVOKABLE void fetchFanDiscover();
    Q_INVOKABLE void bindFanPair(int boardId, int channel, int channel2);
    Q_INVOKABLE void unbindFan(int fanId);
    /** Pulse high ~2.5s then night on LAN W5100 (path-port). */
    Q_INVOKABLE void testFanPair(const QString &host, int modulePort, int channel, int channel2);
    /** Heartbeat питания: last_seen + MAC → power_desired / session_active. */
    Q_INVOKABLE void startPowerHeartbeat();
    Q_INVOKABLE void stopPowerHeartbeat();
    Q_INVOKABLE void sendPowerHeartbeat();
    /** aboutToQuit: fan OFF+/99 ack, затем power_state=off. */
    Q_INVOKABLE void notifyPowerOffline();
    /** Синхронно погасить вентилятор и заактить состояние (logout / shutdown). */
    Q_INVOKABLE void ensureFanOffBeforeExit();
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
    /** Чек полного расчёта после входа на бронь (URL + сумма для QR-попапа). */
    void fiscalReceiptReady(const QString &url, double amount, bool isStub, const QString &description);
    void pendingReceiptChanged();
    /** Emitted only when polled balance differs from the last known value. */
    void balanceUpdated(double balance);
    /** Polled session clock from /api/shell/balance (HH:MM:SS + active flag). */
    void sessionTimeUpdated(const QString &timeRemaining, bool sessionActive);
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
    void fanDebugChanged();
    void fanDiscoverChanged();
    void fanBindFinished(bool ok, const QString &message);
    void fanTestFinished(bool ok, const QString &message);
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
    void acknowledgeFanApplied(int appliedPower, const QString &error, const QString &source);
    int computeLocalDesiredPower(const QJsonObject &fanObj) const;
    void applyDesiredToRelay(int desiredPower, const QString &source);
    bool hasRelayConfig() const;
    void setFanDebug(const QString &line);
    void setFanManualLockSec(int sec);
    QString primaryMacAddress() const;
    bool isLocalSessionActive() const;
    void handlePowerPolicy(const QString &desired, const QString &action, bool sessionActive);
    void publishFiscalReceipt(const QString &url, double amount, bool isStub, const QString &description);
    void pollTopUpReceipt(const QString &paymentId, double fallbackAmount, int attempt);

    QNetworkAccessManager *m_networkManager;
    QTimer *m_climateTimer = nullptr;
    QTimer *m_fanLockTimer = nullptr;
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
    QString m_pendingReceiptUrl;
    double m_pendingReceiptAmount = 0.0;
    bool m_pendingReceiptStub = false;
    QString m_pendingReceiptDescription;
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
    int m_fanManualLockSec = 0;
    QString m_fanDebug;
    QVariantList m_fanDiscoverBoards;
    QVariantList m_fanDiscoverBound;
    QString m_fanDiscoverStatus;
    QString m_fanDiscoverSpaceName;
    bool m_fanDiscoverBusy = false;
    bool m_fanTestInFlight = false;
    int m_fanDiscoverSlotsUsed = 0;
    int m_fanDiscoverSlotsMax = 2;
    double m_cpuTempC = -1.0;
    QString m_zoneName;
    QString m_zoneSlug;
    QString m_zoneColor;
    bool m_climateActive = false;
    bool m_fanRequestInFlight = false;
    bool m_thermalRequestInFlight = false;
    bool m_fanAckInFlight = false;
    bool m_postBootCooldown = false;
    /** false on lobby/auth: relays only for thermal; true after login. */
    bool m_userSessionActive = false;
    bool m_fanApplyInFlight = false;
    bool m_skipRelayApply = false;
    bool m_forceRelayApply = false;
    qint64 m_fanRelayUnreachableUntilMs = 0;
    QString m_fanRelayHost;
    int m_fanRelayPort = 30000;
    int m_fanRelayChannel = 0;
    int m_fanRelayChannel2 = 0;
    QVector<FanRelayEndpoint> m_fanRelays;
    int m_fanAppliedPower = 1;
    int m_fanDesiredPower = 1;
    int m_fanDefaultOnPower = 3;
    QNetworkReply *m_aiAssistantReply = nullptr;
    QNetworkReply *m_voiceGreetingReply = nullptr;

    GameModel* m_gamesModel;
    GameModel* m_featuredGamesModel = nullptr;
    StoreModel* m_storeModel;
    QObject* m_rootQml;
};

#endif // NETWORKMANAGER_H
