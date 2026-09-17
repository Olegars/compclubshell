#ifndef INSTANTREPLAY_H
#define INSTANTREPLAY_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

class NetworkManager;
class PathResolver;

/**
 * Rolling 60s buffer on D:/ShellData/replay (not C: writeback).
 * ffmpeg + h264_nvenc when present; F8 / CS2 kill dumps a 9:16 reel.
 */
class InstantReplay : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY statusChanged)
    Q_PROPERTY(bool capturing READ capturing NOTIFY statusChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY statusChanged)
    Q_PROPERTY(QString hotkeyName READ hotkeyName NOTIFY statusChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY statusChanged)

public:
    explicit InstantReplay(NetworkManager *net, PathResolver *paths, QObject *parent = nullptr);
    ~InstantReplay() override;

    bool available() const { return m_available && m_clubEnabled; }
    bool capturing() const { return m_capturing; }
    bool saving() const { return m_saving; }
    QString hotkeyName() const { return m_hotkeyName; }
    QString lastMessage() const { return m_lastMessage; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void saveClip();
    Q_INVOKABLE void flushAndLogout(int terminalId);
    Q_INVOKABLE void handleHotkeyTap();
    int virtualKey() const { return m_vk; }
    void applyClubConfig();

signals:
    void statusChanged();
    void clipSaved(const QString &shareUrl);
    void clipFailed(const QString &message);

private slots:
    void onCaptureFinished(int exitCode, QProcess::ExitStatus status);
    void onConcatFinished(int exitCode, QProcess::ExitStatus status);
    void onVerticalFinished(int exitCode, QProcess::ExitStatus status);
    void onKillHighlight();

private:
    void loadConfig();
    void resolveFfmpeg();
    void detectNvenc();
    QString bufferDir() const;
    void startCapture(bool allowSoftware);
    void finishLogout();
    void setMessage(const QString &msg);
    void installHook();
    void uninstallHook();
    void saveClipWithSource(const QString &source);
    void startVerticalExport();
    void uploadReadyFile();
    QString ffmpegText(const QString &raw) const;
    QString fontFile() const;
    QString downloadQrPng(const QString &payload, const QString &dest) const;

    NetworkManager *m_net = nullptr;
    PathResolver *m_paths = nullptr;
    QProcess *m_capture = nullptr;
    QProcess *m_concat = nullptr;
    QProcess *m_vertical = nullptr;
    bool m_enabled = true;
    bool m_clubEnabled = true;
    bool m_available = false;
    bool m_capturing = false;
    bool m_saving = false;
    bool m_saveOnLogout = true;
    bool m_useNvenc = false;
    bool m_verticalEnabled = true;
    bool m_autoOnKill = true;
    int m_seconds = 60;
    int m_killSeconds = 12;
    int m_exportDuration = 60;
    int m_killCooldownSec = 75;
    int m_pendingLogoutId = 0;
    qint64 m_lastKillSaveMs = 0;
    QString m_ffmpeg;
    QString m_hotkeyName = QStringLiteral("F8");
    QString m_lastMessage;
    QString m_exportPath;
    QString m_verticalPath;
    QString m_logoPath;
    QString m_shareToken;
    QString m_pendingSource = QStringLiteral("manual");
    int m_vk = 0x77; // F8
    QTimer m_logoutWatchdog;

#ifdef Q_OS_WIN
    HHOOK m_hook = nullptr;
#endif
};

#endif
