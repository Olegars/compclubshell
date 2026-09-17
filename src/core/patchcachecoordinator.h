#ifndef PATCHCACHECOORDINATOR_H
#define PATCHCACHECOORDINATOR_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <memory>

#include "localhttpserver.h"

class NetworkManager;

/**
 * LAN P2P-кэшер патчей Steam/Epic: seed (обычно Super Client, иначе
 * временный сид на самом быстром D:) раздаёт уже скачанные билды по HTTP
 * в VLAN. Ночью fallback-сид качает апдейты в фоне без админа.
 * Не CDN и не torrent-клиент — оркестрация через heartbeat booking.
 */
class PatchCacheCoordinator : public QObject
{
    Q_OBJECT
public:
    explicit PatchCacheCoordinator(NetworkManager *net, QObject *parent = nullptr);
    ~PatchCacheCoordinator() override;

    int seedPort() const;
    bool seedActive() const;
    bool ingestActive() const;
    QString ingestResult() const { return m_ingestResult; }
    QString ingestMessage() const { return m_ingestMessage; }
    QString lanIp() const;

    void applySeedPolicy(const QJsonObject &seed);
    void applyPullCommand(const QJsonObject &pull);
    void applyIngestPolicy(const QJsonObject &ingest);
    void ackPull(qint64 commandId, const QString &result, const QString &message);

private:
    void loadConfig();
    void ensureSeedServer(bool want);
    QString resolveInstallRoot(const QString &platform, const QString &appId) const;
    QString steamInstallDir(const QString &appId) const;
    QJsonArray buildManifest(const QString &platform, const QString &appId) const;
    LocalHttpResponse handleSeedRequest(const LocalHttpRequest &req) const;
    void startPull(qint64 commandId, const QJsonArray &apps);
    bool pullOneApp(const QJsonObject &app, QString *err);
    bool httpGetJson(const QString &url, QJsonObject *out, QString *err);
    bool httpGetFile(const QString &url, const QString &dest, QString *err);
    QString steamExePath() const;
    QString steamcmdPath() const;
    bool processRunning(const QString &image) const;
    void startIngest();
    void stopIngest(const QString &reason);
    void startSteamSilent();
    void stopSteamIfOurs();
    void queueSteamcmdApps();
    void startNextSteamcmd();
    void setIngestStatus(const QString &result, const QString &message);

    NetworkManager *m_net = nullptr;
    std::unique_ptr<LocalHttpServer> m_server;
    std::unique_ptr<QProcess> m_steamcmd;
    bool m_enabled = true;
    bool m_ingestEnabled = true;
    int m_port = 8745;
    int m_maxFiles = 400;
    qint64 m_maxFileBytes = 512LL * 1024 * 1024;
    bool m_pullBusy = false;
    qint64 m_lastPullAckId = 0;
    bool m_ingestWant = false;
    bool m_ingestBusy = false;
    bool m_startedSteam = false;
    QString m_steamcmdBin;
    QStringList m_ingestQueue;
    int m_ingestOk = 0;
    int m_ingestTotal = 0;
    QString m_ingestResult;
    QString m_ingestMessage;
};

#endif
