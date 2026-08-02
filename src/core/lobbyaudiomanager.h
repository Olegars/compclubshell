#ifndef LOBBYAUDIOMANAGER_H
#define LOBBYAUDIOMANAGER_H

#include <QObject>
#include <QString>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QTimer>

class NetworkManager;
class SessionAlertManager;

/**
 * Guest lobby music on speakers → fade-out on login → spoken AI greeting on speakers.
 * HeadphonesGuard is paused while room audio is active.
 */
class LobbyAudioManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    explicit LobbyAudioManager(NetworkManager *net,
                               SessionAlertManager *sessionAlert = nullptr,
                               QObject *parent = nullptr);
    ~LobbyAudioManager() override;

    QString state() const { return m_state; }

    /** Start/stop lobby music for unauthenticated shell. */
    Q_INVOKABLE void setGuestMode(bool guest);
    /** After successful login: fade music, then request/play greeting. */
    Q_INVOKABLE void onLoginSucceeded();
    /** Abort greeting / restore headphones policy. */
    Q_INVOKABLE void stopAll();

signals:
    void stateChanged();

private slots:
    void onFadeTick();
    void onGreetingReady(const QByteArray &audioBytes, const QString &mime,
                         const QString &replyText, bool isFirstVisit);
    void onGreetingFailed(const QString &message);
    void onPlayerStateChanged(QMediaPlayer::PlaybackState state);

private:
    void setState(const QString &state);
    void loadConfig();
    QString resolveMusicPath() const;
    bool ensureGeneratedLobbyWav(const QString &path) const;
    bool routeToSpeakers();
    void restoreHeadphonesPolicy();
    void startMusic();
    void beginFadeOutAndGreet();
    void playGreetingBytes(const QByteArray &mp3);
    void cleanupTempFiles();

    NetworkManager *m_net = nullptr;
    SessionAlertManager *m_sessionAlert = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOut = nullptr;
    QTimer m_fadeTimer;

    QString m_state = QStringLiteral("idle");
    bool m_guestMode = false;
    bool m_featureEnabled = true;
    bool m_speakersRouted = false;
    bool m_awaitingGreeting = false;
    bool m_playingGreeting = false;

    qreal m_musicVolume = 0.45;
    int m_fadeMs = 1400;
    qreal m_fadeFrom = 0.0;
    int m_fadeElapsed = 0;

    QString m_musicPath;
    QString m_speakersDeviceId;   // optional sticky WASAPI / Qt device id
    QString m_speakersNameHint;   // optional substring match
    QString m_tempGreetingPath;
};

#endif // LOBBYAUDIOMANAGER_H
