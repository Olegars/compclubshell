#ifndef ROLLBACKMARKER_H
#define ROLLBACKMARKER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class NetworkManager;
class CcbootSuperClient;
class PathResolver;

/**
 * Rollback Markers: снимок хэшей Steam/Epic/конфигов после save Super Client,
 * детект BSOD, откат файлов по heartbeat на проверенную ревизию.
 */
class RollbackMarkerWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit RollbackMarkerWatchdog(NetworkManager *net,
                                    CcbootSuperClient *ccboot,
                                    QObject *parent = nullptr);

    void markCleanShutdown();

private:
    void loadConfig();
    bool clubEnabled() const;
    void detectCrash();
    QString classifyCrash() const;
    void snapshotAndUpload(const QString &diskMode, const QString &note);
    QJsonArray collectFiles() const;
    QJsonObject fileEntry(const QString &abs, const QString &rel, const QString &kind) const;
    void appendDirMasked(QJsonArray *out, const QString &dir, const QString &mask,
                         const QString &relPrefix, const QString &kind) const;
    void onRollbackCommand(qint64 commandId, qint64 revisionId);
    void applyFiles(const QJsonArray &files, qint64 commandId);
    QString destFor(const QJsonObject &file) const;
    QString markerPath() const;
    QString pendingPath() const;
    QString everBootedPath() const;
    void writePending(const QString &diskMode) const;
    QString takePending() const;

    NetworkManager *m_net = nullptr;
    CcbootSuperClient *m_ccboot = nullptr;
    bool m_localEnabled = true;
    bool m_busy = false;
    qint64 m_pendingCommandId = 0;
    QString m_crashReason;
    QString m_crashDetail;
};

#endif
