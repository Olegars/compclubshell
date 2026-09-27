#ifndef GOLDENIMAGEDRIFTWATCHDOG_H
#define GOLDENIMAGEDRIFTWATCHDOG_H

#include <QObject>
#include <QHash>
#include <QStringList>
#include <atomic>

class NetworkManager;
class ProcessManager;

/**
 * Golden Image Drift Watchdog: SHA256 критических файлов D:,
 * сравнение с эталоном бездиска, тихий re-sync по heartbeat.
 */
class GoldenImageDriftWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit GoldenImageDriftWatchdog(NetworkManager *net,
                                      ProcessManager *launcher,
                                      QObject *parent = nullptr);

    void snapshotAtSessionStart();
    void onGameSessionFinished(const QString &reason);

private:
    void loadConfig();
    void startSnapshotAsync(bool loadTemplate);
    void applySnapshot(const QHash<QString, QString> &hashes,
                       const QHash<QString, QString> &templateHashes,
                       bool loadedTemplate);
    QStringList criticalPaths() const;
    QHash<QString, QString> hashFiles(const QStringList &paths) const;
    QString aggregateHash(const QHash<QString, QString> &hashes) const;
    QHash<QString, QString> loadTemplateHashes() const;
    QStringList diffPaths(const QHash<QString, QString> &local,
                          const QHash<QString, QString> &templateHashes) const;
    void reportDrift(const QStringList &paths, const QString &trigger);
    bool shouldCheckAfterFailure(const QString &reason) const;
    bool copyFromTemplate(const QString &relPath) const;
    void runResync(const QStringList &paths, qint64 commandId);

    NetworkManager *m_net = nullptr;
    ProcessManager *m_launcher = nullptr;

    bool m_enabled = true;
    QString m_templateRoot;
    QString m_copyTool;
    int m_maxFiles = 64;
    int m_maxResyncFiles = 12;

    QHash<QString, QString> m_sessionHashes;
    QHash<QString, QString> m_templateHashes;
    bool m_templateLoaded = false;
    bool m_resyncBusy = false;
    std::atomic<bool> m_snapshotBusy { false };
    qint64 m_pendingResyncId = 0;
};

#endif // GOLDENIMAGEDRIFTWATCHDOG_H
