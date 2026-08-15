#ifndef PATHRESOLVER_H
#define PATHRESOLVER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

/**
 * Гибрид: шелл на C: (образ), данные на SSD (D: / метка GAMES).
 * C:/ShellVideo на writeback после reboot пустой — логи, оверлеи и
 * machine-cache лаунчеров живут в data_root.
 */
class PathResolver : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool cacheOk READ cacheOk NOTIFY cacheStatusChanged)
    Q_PROPERTY(bool cacheReady READ cacheOk NOTIFY cacheStatusChanged)
    Q_PROPERTY(double cacheFreeGb READ cacheFreeGb NOTIFY cacheStatusChanged)
    Q_PROPERTY(QString dataRoot READ dataRoot NOTIFY cacheStatusChanged)
    Q_PROPERTY(QString volumeLetter READ volumeLetter NOTIFY cacheStatusChanged)
    Q_PROPERTY(QString overlayCachePath READ overlayCachePath NOTIFY cacheStatusChanged)
    Q_PROPERTY(QString fallbackVideoUrl READ fallbackVideoUrl NOTIFY cacheStatusChanged)
    Q_PROPERTY(QString steamPath READ steamPath NOTIFY pathsChanged)
    Q_PROPERTY(QString gamesPath READ gamesPath NOTIFY pathsChanged)
    Q_PROPERTY(QString epicPath READ epicPath NOTIFY pathsChanged)
    Q_PROPERTY(QString eaPath READ eaPath NOTIFY pathsChanged)
    Q_PROPERTY(QString riotPath READ riotPath NOTIFY pathsChanged)
    Q_PROPERTY(QString debugLogPath READ debugLogPath NOTIFY cacheStatusChanged)

public:
    static PathResolver *instance();
    static QString findConfigIni();

    explicit PathResolver(QObject *parent = nullptr);

    bool cacheOk() const { return m_cacheOk; }
    double cacheFreeGb() const { return m_cacheFreeGb; }
    QString dataRoot() const { return m_dataRoot; }
    QString volumeLetter() const { return m_volumeLetter; }
    QString overlayCachePath() const;
    QString fallbackVideoUrl() const;
    QString steamPath() const { return m_steamPath; }
    QString gamesPath() const { return m_gamesPath; }
    QString epicPath() const { return m_epicPath; }
    QString eaPath() const { return m_eaPath; }
    QString riotPath() const { return m_riotPath; }
    QString debugLogPath() const;
    QString voiceDir() const;
    QString lobbyDir() const;

    Q_INVOKABLE QString fileUrl(const QString &absolutePath) const;
    Q_INVOKABLE QStringList expandLauncherCandidates(const QStringList &paths) const;
    Q_INVOKABLE bool waitForCache(int timeoutMs = 0);

    QString persistentFile(const QString &relative) const;
    void persistFile(const QString &volatilePath, const QString &relativeName);
    void restoreFile(const QString &volatilePath, const QString &relativeName);
    void persistLauncherCaches();
    void restoreLauncherCaches();
    void refresh();

signals:
    void cacheStatusChanged();
    void pathsChanged();

private:
    void loadConfig();
    void resolveDataRoot();
    bool tryUsePath(const QString &path, bool requireWritable);
    QString findVolumeByLabel(const QString &label) const;
    void ensureLayout();
    void updateFreeSpace();
    QString firstExisting(const QStringList &candidates, const QString &fallback) const;

    static PathResolver *s_instance;

    QTimer m_readyTimer;
    int m_readyAttempts = 0;
    int m_readyTimeoutSec = 90;

    QString m_configPath;
    QString m_configuredRoot;
    QString m_volumeLabel;
    QString m_dataRoot;
    QString m_volumeLetter;
    QString m_steamPath;
    QString m_gamesPath;
    QString m_epicPath;
    QString m_eaPath;
    QString m_riotPath;
    double m_cacheFreeGb = 0.0;
    bool m_cacheOk = false;
    bool m_restoredLaunchers = false;
};

#endif // PATHRESOLVER_H
