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
#include <QJsonArray>
#include <QVariantMap>
#include <QVariantList>
#include <QTimer>
#include <QVector>
#include <QStringList>

class GameModel;
class StoreModel;
class DmxController;
class ReactiveLighting;
class OpenRgbClient;
class QThread;
class CcbootSuperClient;
class LinkFlapWatchdog;
class PatchCacheCoordinator;
class HardwareHealthWatchdog;
class HidInputMonitor;

struct FanRelayEndpoint {
    QString host;
    QString driver;
    int port = 8080;
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
    Q_PROPERTY(bool lightAvailable READ lightAvailable NOTIFY lightStateChanged)
    Q_PROPERTY(QString lightColor READ lightColor NOTIFY lightStateChanged)
    Q_PROPERTY(int lightBrightness READ lightBrightness NOTIFY lightStateChanged)
    Q_PROPERTY(QString lightEffect READ lightEffect NOTIFY lightStateChanged)
    Q_PROPERTY(int lightManualLockSec READ lightManualLockSec NOTIFY lightStateChanged)
    Q_PROPERTY(bool lightInteractive READ lightInteractive NOTIFY lightStateChanged)
    Q_PROPERTY(QString lightInteractiveHint READ lightInteractiveHint NOTIFY lightStateChanged)
    Q_PROPERTY(bool openRgbEnabled READ openRgbEnabled NOTIFY openRgbChanged)
    Q_PROPERTY(bool openRgbConnected READ openRgbConnected NOTIFY openRgbChanged)
    Q_PROPERTY(QString openRgbStatus READ openRgbStatus NOTIFY openRgbChanged)
    Q_PROPERTY(double cpuTempC READ cpuTempC NOTIFY cpuTempChanged)
    Q_PROPERTY(double ssdTempC READ ssdTempC NOTIFY ssdTempChanged)
    Q_PROPERTY(QString zoneName READ zoneName NOTIFY zoneInfoChanged)
    Q_PROPERTY(QString zoneSlug READ zoneSlug NOTIFY zoneInfoChanged)
    Q_PROPERTY(QString zoneColor READ zoneColor NOTIFY zoneInfoChanged)
    Q_PROPERTY(QString clubName READ clubName NOTIFY clubNameChanged)
    Q_PROPERTY(bool maintenance READ maintenance NOTIFY maintenanceChanged)
    Q_PROPERTY(bool ttsEnabled READ ttsEnabled NOTIFY ttsVoicesChanged)
    Q_PROPERTY(QString ttsVoice READ ttsVoice NOTIFY ttsVoiceChanged)
    Q_PROPERTY(QVariantList ttsVoices READ ttsVoices NOTIFY ttsVoicesChanged)
    Q_PROPERTY(QStringList partySeatNames READ partySeatNames NOTIFY partyChanged)
    Q_PROPERTY(bool partyOrderAvailable READ partyOrderAvailable NOTIFY partyChanged)
    Q_PROPERTY(bool inMatch READ inMatch NOTIFY inMatchChanged)
    Q_PROPERTY(bool ghostCoachEnabled READ ghostCoachEnabled NOTIFY lanLiveChanged)
    Q_PROPERTY(bool partyEnergyAvailable READ partyEnergyAvailable NOTIFY lanLiveChanged)
    Q_PROPERTY(bool partyEnergyAutoFuel READ partyEnergyAutoFuel NOTIFY lanLiveChanged)
    Q_PROPERTY(bool partyEnergyIsCaptain READ partyEnergyIsCaptain NOTIFY lanLiveChanged)
    Q_PROPERTY(int partyEnergyMinutes READ partyEnergyMinutes NOTIFY lanLiveChanged)
    Q_PROPERTY(QVariantList bounties READ bounties NOTIFY lanLiveChanged)
    Q_PROPERTY(QVariantList bountyTargets READ bountyTargets NOTIFY lanLiveChanged)
    Q_PROPERTY(QVariantList bountyProducts READ bountyProducts NOTIFY lanLiveChanged)
    Q_PROPERTY(QString lanLiveToast READ lanLiveToast NOTIFY lanLiveChanged)
    Q_PROPERTY(QString playerName READ playerName NOTIFY playerNameChanged)
    Q_PROPERTY(QVariantMap throne READ throne NOTIFY throneChanged)
    Q_PROPERTY(QVariantMap lfg READ lfg NOTIFY lanLiveChanged)
    Q_PROPERTY(QVariantMap lootbox READ lootbox NOTIFY lootboxChanged)
    Q_PROPERTY(QVariantMap clanWar READ clanWar NOTIFY clanWarChanged)
    Q_PROPERTY(QVariantMap faceit READ faceit NOTIFY faceitChanged)
    Q_PROPERTY(QVariantMap faceitMatch READ faceitMatch NOTIFY faceitMatchChanged)
    Q_PROPERTY(QVariantMap arena READ arena NOTIFY arenaChanged)
    Q_PROPERTY(QVariantMap clubFeatures READ clubFeatures NOTIFY clubFeaturesChanged)
public:
    explicit NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent = nullptr);

    bool isPcRegistered() const;
    QString serverUrl() const;
    ReactiveLighting *reactiveLighting() const { return m_gsi; }

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
    bool lightAvailable() const { return m_lightAvailable; }
    QString lightColor() const { return m_lightColor; }
    int lightBrightness() const { return m_lightBrightness; }
    QString lightEffect() const { return m_lightEffect; }
    int lightManualLockSec() const { return m_lightManualLockSec; }
    bool lightInteractive() const { return m_lightInteractive; }
    QString lightInteractiveHint() const { return m_lightInteractiveHint; }
    bool openRgbEnabled() const { return m_openRgbEnabled; }
    bool openRgbConnected() const { return m_openRgbConnected; }
    QString openRgbStatus() const { return m_openRgbStatus; }
    double cpuTempC() const { return m_cpuTempC; }
    double ssdTempC() const { return m_ssdTempC; }
    QString zoneName() const { return m_zoneName; }
    QString zoneSlug() const { return m_zoneSlug; }
    QString zoneColor() const { return m_zoneColor; }
    QString clubName() const { return m_clubName; }
    bool ttsEnabled() const { return m_ttsEnabled; }
    QString ttsVoice() const { return m_ttsVoice; }
    QVariantList ttsVoices() const { return m_ttsVoices; }
    QStringList partySeatNames() const { return m_partySeatNames; }
    bool partyOrderAvailable() const { return m_partySeatNames.size() > 1; }
    bool inMatch() const { return m_inMatch; }
    bool ghostCoachEnabled() const { return m_ghostCoachEnabled; }
    bool partyEnergyAvailable() const { return m_partyEnergyAvailable; }
    bool partyEnergyAutoFuel() const { return m_partyEnergyAutoFuel; }
    bool partyEnergyIsCaptain() const { return m_partyEnergyIsCaptain; }
    int partyEnergyMinutes() const { return m_partyEnergyMinutes; }
    QVariantList bounties() const { return m_bounties; }
    QVariantList bountyTargets() const { return m_bountyTargets; }
    QVariantList bountyProducts() const { return m_bountyProducts; }
    QString lanLiveToast() const { return m_lanLiveToast; }
    QString playerName() const { return m_playerName; }
    QVariantMap throne() const { return m_throne; }
    QVariantMap lfg() const { return m_lfg; }
    QVariantMap lootbox() const { return m_lootbox; }
    QVariantMap clanWar() const { return m_clanWar; }
    QVariantMap faceit() const { return m_faceit; }
    QVariantMap faceitMatch() const { return m_faceitMatch; }
    QVariantMap arena() const { return m_arena; }
    QVariantMap clubFeatures() const { return m_clubFeatures; }
    Q_INVOKABLE bool featureEnabled(const QString &key) const;
    bool maintenance() const { return m_maintenance; }
    Q_INVOKABLE void setMaintenance(bool on);
    /** Guest logged in (UI or backend user id). */
    bool isGuestSessionActive() const;
    /** ПК включён по расписанию (warmup), гостя нет. */
    bool isWarmupIdle() const;
    bool hasPersonalFanRelay() const;
    void setPersonalFanSpeedDirect(int speed);
    void setFanProbeLock(bool on);
    void reportShellIncident(const QString &type, const QString &severity,
                             const QString &description, const QJsonObject &payload = QJsonObject());
    void setIntegrityTelemetry(const QString &status, const QString &hash,
                               const QString &message, const QStringList &driftPaths);
    void setGpuTelemetry(int limitW, const QString &mode);
    void ackResyncCommand(qint64 commandId, const QString &result, const QString &message);
    void ackRollbackCommand(qint64 commandId, const QString &result, const QString &message);
    void postGoldenRevision(const QJsonObject &payload);
    void fetchGoldenRevision(qint64 revisionId);
    void setCrashTelemetry(bool detected, const QString &reason, const QString &detail);
    void setLinkFlapWatchdog(LinkFlapWatchdog *watchdog);
    void setHardwareHealthWatchdog(HardwareHealthWatchdog *watchdog);
    void setHidInputMonitor(HidInputMonitor *hid);
    void armShiftAudit();
    void setPatchCache(PatchCacheCoordinator *cache);
    void noteLinkFlap(const QJsonObject &payload);
    void ackPatchPull(qint64 commandId, const QString &result, const QString &message);
    QString primaryLanIp() const;

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
    /** Абсолютный URL ролика относительно api_ip — для стрима, пока кэш качается. */
    Q_INVOKABLE QString resolveOverlayUrl(const QString &remotePath) const;
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
    /** Guest arrived early: release pre-session shop order into the admin queue. */
    Q_INVOKABLE void releaseScheduledOrder(int terminalId = 0);
    Q_INVOKABLE void login(const QString &phone, const QString &pin, int terminalId, bool acceptSeatChange = false);
    /** QR login challenge for the login screen (poll until consumed). */
    Q_INVOKABLE void requestQrChallenge(int terminalId = 0);
    Q_INVOKABLE void stopQrLoginPoll();
    /** Poll spendable balance for the active shell session (no-op when logged out). */
    Q_INVOKABLE void refreshBalance();
    /** Create YooKassa embedded widget top-up; emits topUpReady(widgetUrl, ...). */
    Q_INVOKABLE void createTopUp(double amount, bool sendReceipt = false);
    /** Pull payment status from YooKassa and credit wallet if paid (same as «Вернуться»). */
    Q_INVOKABLE void syncTopUpPayment(const QString &paymentId);
    Q_INVOKABLE void fetchOverlays(int terminalId);
    Q_INVOKABLE void fetchClanWar(int terminalId = 0);
    Q_INVOKABLE void freeGameAccount(int terminalId, int gameId);
    Q_INVOKABLE void recordGameLaunch(int gameId);
    Q_INVOKABLE void uploadClip(const QString &filePath, int durationSec,
                                const QString &shareToken = QString(),
                                const QString &aspect = QString(),
                                const QString &source = QString());
    Q_INVOKABLE void sendSos(const QString &reasonCode, const QString &reasonLabel);
    /** Ручной запуск приложения TV Shell. На ПК сервер событие не пишет. */
    Q_INVOKABLE void recordTvAppLaunch(const QString &title);
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
    Q_INVOKABLE void fetchTtsVoices();
    Q_INVOKABLE void setTtsVoice(const QString &voice);
    Q_INVOKABLE void fetchLanLive();
    Q_INVOKABLE void createBounty(int targetComputerId, const QString &kind, const QString &game,
                                 const QString &weapon, const QString &stakeType,
                                 double stakeAmount, int productId, const QString &title);
    Q_INVOKABLE void cancelBounty(int bountyId);
    Q_INVOKABLE void setPartyAutoFuel(bool on);
    Q_INVOKABLE void contributePartyEnergy(int minutes, const QString &source);
    Q_INVOKABLE void setGhostCoachEnabled(bool on);
    Q_INVOKABLE void enqueueLfg(const QString &game, const QString &rank);
    Q_INVOKABLE void cancelLfg();
    Q_INVOKABLE void sitLfg();
    Q_INVOKABLE void openLootbox(int dropId);
    Q_INVOKABLE void createArenaChallenge(const QString &game, const QString &mode,
                                          double entryFee, const QString &scope,
                                          int targetComputerId, const QString &kind = QString(),
                                          int maxPlayers = 0);
    Q_INVOKABLE void acceptArena(const QString &uuid);
    Q_INVOKABLE void declineArena(const QString &uuid);
    Q_INVOKABLE void cancelArena(const QString &uuid);
    Q_INVOKABLE void proposeArenaRaise(const QString &uuid, double entryFee);
    Q_INVOKABLE void voteArenaRaise(const QString &uuid, bool agree);
    Q_INVOKABLE void startArena(const QString &uuid);
    Q_INVOKABLE void bindFanPair(int boardId, int channel, int channel2);
    Q_INVOKABLE void unbindFan(int fanId);
    /** Pulse high ~2.5s then night on LAN relay (NetMod TCP port or W5100 path-port). */
    Q_INVOKABLE void testFanPair(const QString &host, int modulePort, int channel, int channel2,
                                 const QString &driver = QString());
    Q_INVOKABLE void fetchLightState();
    Q_INVOKABLE void setLightColor(const QString &color);
    Q_INVOKABLE void setLightBrightness(int brightness);
    Q_INVOKABLE void setLightInteractive(bool on);
    /** Static magenta for ~4s so a tech can see the ARGB hub is in M/B sync. */
    Q_INVOKABLE void testOpenRgbSync();
    /** Heartbeat питания: last_seen + MAC → power_desired / session_active. */
    Q_INVOKABLE void startPowerHeartbeat();
    Q_INVOKABLE void stopPowerHeartbeat();
    Q_INVOKABLE void sendPowerHeartbeat();
    void setDisklessController(CcbootSuperClient *controller);
    bool isProduction() const { return m_production; }
    /** aboutToQuit: fan OFF+/99 ack, затем power_state=off. */
    Q_INVOKABLE void notifyPowerOffline();
    /** Синхронно погасить вентилятор и заактить состояние (logout / shutdown). */
    Q_INVOKABLE void ensureFanOffBeforeExit();
    /** Погасить свет комнаты, если это последняя сессия. */
    Q_INVOKABLE void ensureLightOffBeforeExit();
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
    /** PIN на чужом ПК: canSwitch=true → Да/Нет, иначе только «понятно». */
    void seatChangeRequired(const QString &message, bool canSwitch);
    void loginRequestFinished();
    void qrChallengeReady(const QString &token, const QString &qrPayload, const QString &expiresAt);
    void qrChallengeFailed(const QString &message);
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
    void clipUploadSucceeded(const QString &shareUrl);
    void clipUploadFailed(const QString &message);
    void quickAppsChanged();
    void sosSent(bool success);
    void fanStateChanged();
    void fanDebugChanged();
    void fanDiscoverChanged();
    void fanBindFinished(bool ok, const QString &message);
    void fanTestFinished(bool ok, const QString &message);
    void lightStateChanged();
    void openRgbChanged();
    void cpuTempChanged();
    void ssdTempChanged();
    void zoneInfoChanged();
    void clubNameChanged();
    void maintenanceChanged();
    /** Backend asks shell to reboot or shutdown after session / idle policy. */
    void powerActionRequested(const QString &action);
    /** Scheduler closed the booking while shell still showed a logged-in user. */
    void sessionForceEnded();
    /** Admin queued silent D: re-sync (heartbeat). */
    void resyncCommandReceived(qint64 commandId, const QString &action);
    /** Admin queued rollback to a verified golden-image revision. */
    void rollbackCommandReceived(qint64 commandId, qint64 revisionId, const QString &action);
    void goldenRevisionFetched(qint64 revisionId, const QJsonObject &payload);
    void goldenRevisionFetchFailed(qint64 revisionId, const QString &message);
    /** Cloud asks shell to pull game patch over LAN from seed peer. */
    void patchPullReceived(qint64 commandId, const QJsonObject &pull);
    void aiAssistantSucceeded(const QByteArray &audioBytes, const QString &mime,
                              const QString &transcript, const QString &replyText);
    void aiAssistantFailed(const QString &message);
    void voiceGreetingSucceeded(const QByteArray &audioBytes, const QString &mime,
                                const QString &replyText, bool isFirstVisit);
    void voiceGreetingFailed(const QString &message);
    void ttsVoiceChanged();
    void ttsVoicesChanged();
    void partyChanged();
    void inMatchChanged();
    void lanLiveChanged();
    void clanWarChanged();
    void faceitChanged();
    void faceitMatchChanged();
    void arenaChanged();
    void arenaIncoming(const QVariantMap &challenge);
    void arenaVictory(const QVariantMap &result);
    void bountySettled(const QString &message);
    void ghostWhisper(const QString &text);
    void killHighlight();
    void throneChanged();
    void playerNameChanged();
    void throneCrowned(const QString &line);
    void lootboxChanged();
    void lootboxDropped(const QVariantMap &box);
    void lootboxOpened(const QVariantMap &box);
    void clubFeaturesChanged();
    void rageSmashAlert(const QVariantMap &payload);
    void lfgSitSucceeded(const QString &pin, const QString &pcName, const QString &message);
    void ttsPreviewSucceeded(const QByteArray &audioBytes, const QString &mime);

private:
    static QString cleanDigits(const QString &value);
    static double jsonToDouble(const QJsonValue &value, double defaultValue = 0.0);
    static double userBalanceFromJson(const QJsonObject &user);
    void applyGamesPayload(const QJsonDocument &doc);
    void applyOrderStatusFromJson(const QJsonObject &rootObj);
    int resolveTerminalId(int terminalId) const;
    void applyFanStateFromJson(const QJsonObject &fanObj);
    void applyTtsVoicesFromJson(const QJsonObject &root);
    void startSessionFans(const QJsonObject &fanObj);
    void startSessionLights(const QJsonObject &lightObj);
    void applyLightStateFromJson(const QJsonObject &lightObj);
    void applyDesiredToDmx(bool force);
    void setupOpenRgb(const QSettings &settings);
    void blackoutOpenRgb();
    void acknowledgeLightApplied(const QString &color, int brightness, const QString &effect,
                                  const QString &error);
    void postLightScene(const QJsonObject &body);
    void setLightManualLockSec(int sec);
    void postThermal(double cpuC, double ssdC);
    void acknowledgeFanApplied(int appliedPower, const QString &error, const QString &source);
    int computeLocalDesiredPower(const QJsonObject &fanObj) const;
    void applyDesiredToRelay(int desiredPower, const QString &source);
    bool hasRelayConfig() const;
    void setFanDebug(const QString &line);
    void setFanManualLockSec(int sec);
    QString primaryMacAddress() const;
    bool isLocalSessionActive() const;
    bool isSetupScreenOpen() const;
    void handlePowerPolicy(const QString &desired, const QString &action, bool sessionActive);
    void attachStationHealth(QJsonObject &json);
    void handleDisklessCommand(const QJsonObject &obj);
    void handleResyncCommand(const QJsonObject &obj);
    void handleRollbackCommand(const QJsonObject &obj);
    void handlePatchCommands(const QJsonObject &root);
    void publishFiscalReceipt(const QString &url, double amount, bool isStub, const QString &description);
    void pollTopUpReceipt(const QString &paymentId, double fallbackAmount, int attempt);
    void applyQrLoginSuccess(const QJsonObject &response);
    void pollQrStatusOnce();
    void applyClubName(const QString &raw);
    void applyPlayerTtsVoice(const QString &voice);
    void applyPartyFromJson(const QJsonObject &root);
    void applyLanLiveFromJson(const QJsonObject &root);
    void applyThroneFromJson(const QJsonObject &root);
    void applyLootboxFromJson(const QJsonObject &root);
    void applyClanWarFromJson(const QJsonObject &root);
    void applyFaceitFromJson(const QJsonObject &root);
    void applyFaceitMatchFromJson(const QJsonObject &root);
    void applyArenaFromJson(const QJsonObject &root);
    void postArenaAction(const QString &path, const QJsonObject &extra = QJsonObject());
    void applyClubFeaturesFromJson(const QJsonObject &root);
    void postGsiEvent(const QJsonObject &payload);
    void setInMatch(bool on);
    void syncGsiListen();

    QNetworkAccessManager *m_networkManager;
    QTimer *m_climateTimer = nullptr;
    QTimer *m_fanLockTimer = nullptr;
    QTimer *m_powerHeartbeatTimer = nullptr;
    QTimer *m_qrPollTimer = nullptr;
    QString m_qrToken;
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
    bool m_maintenance = false;
    bool m_production = false;
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
    bool m_lightAvailable = false;
    QString m_lightColor = QStringLiteral("white");
    int m_lightBrightness = 0;
    QString m_lightEffect = QStringLiteral("none");
    int m_lightManualLockSec = 0;
    int m_lightRainbowPeriodMs = 8000;
    bool m_lightRequestInFlight = false;
    bool m_lightAckInFlight = false;
    bool m_skipLightApply = false;
    bool m_lightInteractive = false;
    QString m_lightInteractiveHint;
    qint64 m_lastPlayEventAt = 0;
    ReactiveLighting *m_gsi = nullptr;
    QTimer *m_lightLockTimer = nullptr;
    DmxController *m_dmx = nullptr;
    OpenRgbClient *m_openRgb = nullptr;
    QThread *m_openRgbThread = nullptr;
    bool m_openRgbEnabled = false;
    bool m_openRgbConnected = false;
    QString m_openRgbStatus = QStringLiteral("выключено (OpenRGB/enabled)");
    double m_cpuTempC = -1.0;
    double m_ssdTempC = -1.0;
    QString m_zoneName;
    QString m_zoneSlug;
    QString m_zoneColor;
    QString m_clubName;
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
    QString m_fanRelayDriver;
    int m_fanRelayPort = 8080;
    int m_fanRelayChannel = 0;
    int m_fanRelayChannel2 = 0;
    QVector<FanRelayEndpoint> m_fanRelays;
    int m_fanAppliedPower = 1;
    int m_fanDesiredPower = 1;
    int m_fanDefaultOnPower = 3;
    QNetworkReply *m_aiAssistantReply = nullptr;
    QNetworkReply *m_voiceGreetingReply = nullptr;
    QNetworkReply *m_ttsVoicesReply = nullptr;
    QNetworkReply *m_ttsVoiceSetReply = nullptr;
    bool m_ttsEnabled = false;
    QString m_ttsVoice;
    QVariantList m_ttsVoices;
    QStringList m_partySeatNames;
    bool m_inMatch = false;
    bool m_ghostCoachEnabled = true;
    bool m_partyEnergyAvailable = false;
    bool m_partyEnergyAutoFuel = false;
    bool m_partyEnergyIsCaptain = false;
    int m_partyEnergyMinutes = 0;
    QVariantList m_bounties;
    QVariantList m_bountyTargets;
    QVariantList m_bountyProducts;
    QString m_lanLiveToast;
    QString m_playerName;
    QVariantMap m_throne;
    QVariantMap m_lfg;
    QVariantMap m_lootbox;
    int m_lootboxAnnouncedId = 0;
    QVariantMap m_clanWar;
    QVariantMap m_faceit;
    QVariantMap m_faceitMatch;
    QVariantMap m_arena;
    QString m_arenaIncomingUuid;
    QVariantMap m_clubFeatures;
    bool m_gsiPostInFlight = false;
    QJsonObject m_pendingGsi;

    GameModel* m_gamesModel;
    GameModel* m_featuredGamesModel = nullptr;
    StoreModel* m_storeModel;
    QObject* m_rootQml;
    CcbootSuperClient *m_diskless = nullptr;
    qint64 m_lastDisklessAckId = 0;
    QString m_lastDisklessResult;
    QString m_lastDisklessMessage;
    int m_healthTick = 0;
    QString m_cachedGamesHash;
    QJsonArray m_cachedGamesJson;
    int m_cachedSteamCount = 0;
    int m_cachedEpicCount = 0;
    bool m_fanProbeLock = false;
    QString m_lastPowerDesired;
    bool m_lastSessionActiveFromHeartbeat = false;
    QString m_integrityStatus;
    QString m_integrityHash;
    QString m_integrityMessage;
    QStringList m_integrityDrift;
    int m_gpuPowerLimitW = 0;
    QString m_gpuMode;
    qint64 m_lastResyncAckId = 0;
    QString m_lastResyncResult;
    QString m_lastResyncMessage;
    qint64 m_lastRollbackAckId = 0;
    QString m_lastRollbackResult;
    QString m_lastRollbackMessage;
    bool m_crashDetected = false;
    QString m_crashReason;
    QString m_crashDetail;
    LinkFlapWatchdog *m_linkFlap = nullptr;
    HardwareHealthWatchdog *m_hwHealth = nullptr;
    HidInputMonitor *m_hidAudit = nullptr;
    bool m_shiftAuditArmed = false;
    bool m_shiftAuditReady = false;
    bool m_shiftAuditSent = false;
    PatchCacheCoordinator *m_patchCache = nullptr;
    int m_pendingLinkFlaps = 0;
    QJsonObject m_lastLinkFlapPayload;
    qint64 m_lastPatchPullAckId = 0;
    QString m_lastPatchPullResult;
    QString m_lastPatchPullMessage;
};

#endif // NETWORKMANAGER_H
