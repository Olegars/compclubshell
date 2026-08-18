#ifndef VOICEASSISTANT_H
#define VOICEASSISTANT_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QFile>
#include <QTimer>
#include <memory>

class NetworkManager;
class ProcessManager;
class SessionAlertManager;
class VoiceHotkeyMonitor;
/**
 * Hold-to-talk voice AI: hotkey → mic → /api/shell/ai-assistant → MP3 reply + duck.
 */
class VoiceAssistant : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool active READ isUiActive NOTIFY stateChanged)
    Q_PROPERTY(bool enabled READ isFeatureEnabled NOTIFY enabledChanged)

public:
    explicit VoiceAssistant(NetworkManager *net,
                            ProcessManager *launcher,
                            SessionAlertManager *sessionAlert,
                            QObject *parent = nullptr);
    ~VoiceAssistant() override;

    QString state() const { return m_state; }
    QString lastError() const { return m_lastError; }
    bool isUiActive() const { return m_state != QLatin1String("idle"); }
    bool isFeatureEnabled() const { return m_featureEnabled; }

    Q_INVOKABLE void setSessionActive(bool active);

signals:
    void stateChanged();
    void lastErrorChanged();
    void enabledChanged();
    void errorOccurred(const QString &message);

private slots:
    void onHoldStarted();
    void onHoldEnded();
    void onAiSuccess(const QByteArray &audioBytes, const QString &mime,
                     const QString &transcript, const QString &replyText);
    void onAiFailed(const QString &message);
    void onPlayerStateChanged(QMediaPlayer::PlaybackState state);
    void onTtsPreview(const QByteArray &audioBytes, const QString &mime);
    void clearErrorState();

private:
    void loadConfig();
    void setState(const QString &state);
    void beginListening();
    void finishListeningAndSend();
    void cancelAndIdle(const QString &reason = QString());
    bool startCapture();
    void stopCapture();
    bool writeWavFile(const QString &path, const QByteArray &pcm) const;
    void duckAudio();
    void restoreAudio();
    void playReply(const QByteArray &mp3Bytes);
    void abortNetwork();
    void setLastError(const QString &message);

    NetworkManager *m_net = nullptr;
    ProcessManager *m_launcher = nullptr;
    SessionAlertManager *m_sessionAlert = nullptr;
    VoiceHotkeyMonitor *m_hotkey = nullptr;

    bool m_featureEnabled = true;
    bool m_sessionActive = false;
    int m_duckPercent = 20;
    int m_minClipMs = 400;

    QString m_state = QStringLiteral("idle");
    QString m_lastError;
    bool m_ducked = false;

    QAudioFormat m_format;
    std::unique_ptr<QAudioSource> m_audioSource;
    QIODevice *m_audioDevice = nullptr;
    QByteArray m_pcmBuffer;
    qint64 m_captureStartedMs = 0;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOut = nullptr;
    QString m_tempReplyPath;
    QString m_tempAskPath;

    QTimer m_errorClearTimer;
};

#endif // VOICEASSISTANT_H
