#include "lobbyaudiomanager.h"
#include "networkmanager.h"
#include "pathresolver.h"
#include "sessionalertmanager.h"
#include "audiomanager_win.h"

#include <QCoreApplication>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QUrl>
#include <QDebug>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QtMath>
#include <cstring>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace {

QString resolveConfigPath()
{
    const QString base = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        base + QStringLiteral("/config.ini"),
        base + QStringLiteral("/../config.ini"),
        base + QStringLiteral("/../../config.ini"),
    };
    for (const QString &c : candidates) {
        if (QFile::exists(c))
            return c;
    }
    return candidates.first();
}

} // namespace

LobbyAudioManager::LobbyAudioManager(NetworkManager *net,
                                     SessionAlertManager *sessionAlert,
                                     QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_sessionAlert(sessionAlert)
{
    m_audioOut = new QAudioOutput(this);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOut);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &LobbyAudioManager::onPlayerStateChanged);

    m_fadeTimer.setInterval(40);
    connect(&m_fadeTimer, &QTimer::timeout, this, &LobbyAudioManager::onFadeTick);

    if (m_net) {
        connect(m_net, &NetworkManager::voiceGreetingSucceeded,
                this, &LobbyAudioManager::onGreetingReady);
        connect(m_net, &NetworkManager::voiceGreetingFailed,
                this, &LobbyAudioManager::onGreetingFailed);
    }

    loadConfig();
}

LobbyAudioManager::~LobbyAudioManager()
{
    stopAll();
}

void LobbyAudioManager::loadConfig()
{
    QSettings settings(resolveConfigPath(), QSettings::IniFormat);
    m_featureEnabled = settings.value(QStringLiteral("Voice/lobby_enabled"), true).toBool();
    m_musicVolume = qBound(0.05,
                          settings.value(QStringLiteral("Voice/lobby_volume"), 0.45).toDouble(),
                          1.0);
    m_fadeMs = qBound(400, settings.value(QStringLiteral("Voice/lobby_fade_ms"), 1400).toInt(), 5000);
    m_musicPath = settings.value(QStringLiteral("Voice/lobby_music")).toString().trimmed();
    m_speakersDeviceId = settings.value(QStringLiteral("Voice/lobby_speakers_id")).toString().trimmed();
    m_speakersNameHint = settings.value(QStringLiteral("Voice/lobby_speakers_name")).toString().trimmed();
    qWarning() << "[LOBBY] enabled=" << m_featureEnabled
               << "volume=" << m_musicVolume
               << "fadeMs=" << m_fadeMs
               << "music=" << m_musicPath
               << "speakersId=" << m_speakersDeviceId
               << "speakersName=" << m_speakersNameHint;
}

void LobbyAudioManager::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

QString LobbyAudioManager::resolveMusicPath() const
{
    const QString dataRoot = PathResolver::instance()
            ? PathResolver::instance()->lobbyDir()
            : QStringLiteral("C:/ShellVideo");
    const QStringList candidates = {
        m_musicPath,
        QCoreApplication::applicationDirPath() + QStringLiteral("/sounds/lobby.mp3"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/sounds/lobby.wav"),
        dataRoot + QStringLiteral("/lobby.mp3"),
        dataRoot + QStringLiteral("/lobby.wav"),
    };
    for (const QString &c : candidates) {
        if (!c.isEmpty() && QFile::exists(c))
            return c;
    }
    return dataRoot + QStringLiteral("/lobby-ambient.wav");
}

bool LobbyAudioManager::ensureGeneratedLobbyWav(const QString &path) const
{
    if (QFile::exists(path))
        return true;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;

    // Soft 20s ambient pad (two low sines), looped by QMediaPlayer.
    const int sampleRate = 22050;
    const int channels = 1;
    const int seconds = 20;
    const int samples = sampleRate * seconds;
    QByteArray pcm;
    pcm.resize(samples * 2);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < samples; ++i) {
        const double t = double(i) / sampleRate;
        const double env = 0.35 + 0.15 * qSin(2.0 * M_PI * 0.05 * t);
        const double v = 0.22 * qSin(2.0 * M_PI * 110.0 * t) * env
                       + 0.14 * qSin(2.0 * M_PI * 164.8 * t) * env
                       + 0.08 * qSin(2.0 * M_PI * 220.0 * t) * (0.5 + 0.5 * qSin(2.0 * M_PI * 0.08 * t));
        out[i] = qint16(qBound(-32000.0, v * 32767.0, 32000.0));
    }

    const quint32 dataSize = quint32(pcm.size());
    const quint32 riffSize = 36 + dataSize;
    const quint16 bits = 16;
    const quint16 blockAlign = quint16(channels * bits / 8);
    const quint32 byteRate = quint32(sampleRate * blockAlign);

    auto w32 = [&f](quint32 v) {
        char b[4] = { char(v), char(v >> 8), char(v >> 16), char(v >> 24) };
        f.write(b, 4);
    };
    auto w16 = [&f](quint16 v) {
        char b[2] = { char(v), char(v >> 8) };
        f.write(b, 2);
    };

    f.write("RIFF", 4);
    w32(riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(16);
    w16(1);
    w16(quint16(channels));
    w32(quint32(sampleRate));
    w32(byteRate);
    w16(blockAlign);
    w16(bits);
    f.write("data", 4);
    w32(dataSize);
    f.write(pcm);
    f.close();
    qWarning() << "[LOBBY] generated ambient wav:" << path;
    return true;
}

bool LobbyAudioManager::routeToSpeakers()
{
    // Pause guard so it does not churn endpoints while lobby/greeting plays.
    // Do NOT change Windows default — route via explicit QAudioDevice instead.
    win32_set_headphones_guard_paused(1);
    m_speakersRouted = true;

    QString wantId = m_speakersDeviceId;
    if (wantId.isEmpty())
        wantId = win32_find_speakers_device_id();

    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    QAudioDevice chosen;

    auto idMatch = [](const QAudioDevice &dev, const QString &id) -> bool {
        if (id.isEmpty())
            return false;
        const QString qid = QString::fromUtf8(dev.id());
        return qid.compare(id, Qt::CaseInsensitive) == 0
            || qid.contains(id, Qt::CaseInsensitive)
            || id.contains(qid, Qt::CaseInsensitive);
    };

    if (!wantId.isEmpty()) {
        for (const QAudioDevice &dev : outputs) {
            if (idMatch(dev, wantId)) {
                chosen = dev;
                break;
            }
        }
    }

    if (chosen.isNull() && !m_speakersNameHint.isEmpty()) {
        for (const QAudioDevice &dev : outputs) {
            if (dev.description().contains(m_speakersNameHint, Qt::CaseInsensitive)) {
                chosen = dev;
                break;
            }
        }
    }

    // Last resort: pick a non-headphone-looking output from Qt list.
    if (chosen.isNull()) {
        for (const QAudioDevice &dev : outputs) {
            const QString n = dev.description().toLower();
            if (n.contains(QStringLiteral("headphone"))
                || n.contains(QStringLiteral("headset"))
                || n.contains(QStringLiteral("наушник"))
                || n.contains(QStringLiteral("гарнитур")))
                continue;
            if (n.contains(QStringLiteral("speaker"))
                || n.contains(QStringLiteral("колонк"))
                || n.contains(QStringLiteral("realtek"))) {
                chosen = dev;
                break;
            }
        }
    }

    if (chosen.isNull()) {
        qWarning() << "[LOBBY] speakers QAudioDevice not found — using default output";
        return false;
    }

    m_audioOut->setDevice(chosen);
    qWarning() << "[LOBBY] routed to speakers device:" << chosen.description()
               << "id=" << QString::fromUtf8(chosen.id());
    return true;
}

void LobbyAudioManager::restoreHeadphonesPolicy()
{
    win32_set_headphones_guard_paused(0);
    m_speakersRouted = false;
    // Return QAudioOutput to system default (headphones after guard resumes).
    if (m_audioOut)
        m_audioOut->setDevice(QMediaDevices::defaultAudioOutput());
}

void LobbyAudioManager::cleanupTempFiles()
{
    if (!m_tempGreetingPath.isEmpty()) {
        QFile::remove(m_tempGreetingPath);
        m_tempGreetingPath.clear();
    }
}

void LobbyAudioManager::setGuestMode(bool guest)
{
    m_guestMode = guest;
    if (!m_featureEnabled)
        return;

    if (guest) {
        m_awaitingGreeting = false;
        m_playingGreeting = false;
        if (m_net)
            m_net->abortVoiceGreeting();
        // Не дёргать startMusic повторно (Main.onCompleted + onAuthRequired).
        if (m_state == QLatin1String("music")
            && m_player
            && m_player->playbackState() == QMediaPlayer::PlayingState) {
            return;
        }
        startMusic();
    } else if (m_state == QLatin1String("music")) {
        // Login path uses onLoginSucceeded (fade). Plain stop if leaving guest otherwise.
        m_player->stop();
        setState(QStringLiteral("idle"));
    }
}

void LobbyAudioManager::startMusic()
{
    if (!m_featureEnabled)
        return;

    // Уже играет — не делать повторный COM-scan speakers + reopen MediaPlayer.
    if (m_state == QLatin1String("music")
        && m_player
        && m_player->playbackState() == QMediaPlayer::PlayingState) {
        return;
    }

    QString path = resolveMusicPath();
    if (!QFile::exists(path)) {
        if (!ensureGeneratedLobbyWav(path)) {
            qWarning() << "[LOBBY] cannot prepare music file";
            return;
        }
    }

    routeToSpeakers();
    m_audioOut->setVolume(m_musicVolume);
    m_player->setLoops(QMediaPlayer::Infinite);
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
    setState(QStringLiteral("music"));
    qWarning() << "[LOBBY] music start:" << path;
}

void LobbyAudioManager::onLoginSucceeded()
{
    if (!m_featureEnabled) {
        restoreHeadphonesPolicy();
        return;
    }
    beginFadeOutAndGreet();
}

void LobbyAudioManager::beginFadeOutAndGreet()
{
    m_guestMode = false;
    m_awaitingGreeting = true;

    // Kick off greeting request immediately while music fades.
    if (m_net) {
        const int terminalId = m_net->computerId();
        const int bookingId = m_net->lastBookingId();
        if (terminalId > 0)
            m_net->requestVoiceGreeting(terminalId, bookingId);
        else
            m_awaitingGreeting = false;
    }

    if (m_state != QLatin1String("music") || m_player->playbackState() != QMediaPlayer::PlayingState) {
        m_player->stop();
        setState(m_awaitingGreeting ? QStringLiteral("greeting_wait") : QStringLiteral("idle"));
        if (!m_awaitingGreeting)
            restoreHeadphonesPolicy();
        return;
    }

    m_fadeFrom = m_audioOut->volume();
    m_fadeElapsed = 0;
    m_fadeTimer.start();
    setState(QStringLiteral("fading"));
}

void LobbyAudioManager::onFadeTick()
{
    m_fadeElapsed += m_fadeTimer.interval();
    const qreal t = qBound(0.0, qreal(m_fadeElapsed) / qreal(m_fadeMs), 1.0);
    m_audioOut->setVolume(m_fadeFrom * (1.0 - t));
    if (t < 1.0)
        return;

    m_fadeTimer.stop();
    m_player->stop();
    m_player->setLoops(1);
    setState(m_awaitingGreeting ? QStringLiteral("greeting_wait") : QStringLiteral("idle"));
    if (!m_awaitingGreeting)
        restoreHeadphonesPolicy();
    qWarning() << "[LOBBY] music faded out";
}

void LobbyAudioManager::onGreetingReady(const QByteArray &audioBytes, const QString &mime,
                                        const QString &replyText, bool isFirstVisit)
{
    Q_UNUSED(mime)
    if (!m_awaitingGreeting && m_state != QLatin1String("fading")
        && m_state != QLatin1String("greeting_wait")) {
        return;
    }
    m_awaitingGreeting = false;
    qWarning() << "[LOBBY] greeting ready firstVisit=" << isFirstVisit
               << "text=" << replyText;

    // If still fading, wait until fade completes then play — store bytes via temp now.
    playGreetingBytes(audioBytes);
}

void LobbyAudioManager::playGreetingBytes(const QByteArray &mp3)
{
    if (m_fadeTimer.isActive()) {
        // Finish fade instantly, then play.
        m_fadeTimer.stop();
        m_player->stop();
        m_player->setLoops(1);
    }

    cleanupTempFiles();
    const QString voiceDir = PathResolver::instance()
            ? PathResolver::instance()->voiceDir()
            : QStringLiteral("C:/ShellVideo/Voice");
    QDir().mkpath(voiceDir);
    m_tempGreetingPath = voiceDir + QStringLiteral("/greeting-%1.mp3")
                             .arg(QDateTime::currentMSecsSinceEpoch());
    QFile f(m_tempGreetingPath);
    if (!f.open(QIODevice::WriteOnly) || f.write(mp3) != mp3.size()) {
        qWarning() << "[LOBBY] failed to write greeting mp3";
        restoreHeadphonesPolicy();
        setState(QStringLiteral("idle"));
        return;
    }
    f.close();

    routeToSpeakers();
    m_playingGreeting = true;
    if (m_sessionAlert)
        m_sessionAlert->setSpeechBlocked(true);
    m_audioOut->setVolume(1.0);
    m_player->setLoops(1);
    m_player->setSource(QUrl::fromLocalFile(m_tempGreetingPath));
    m_player->play();
    setState(QStringLiteral("greeting"));
}

void LobbyAudioManager::onGreetingFailed(const QString &message)
{
    qWarning() << "[LOBBY] greeting failed:" << message;
    m_awaitingGreeting = false;
    if (m_state == QLatin1String("fading"))
        return; // fade will restore policy
    if (!m_playingGreeting) {
        restoreHeadphonesPolicy();
        setState(QStringLiteral("idle"));
    }
}

void LobbyAudioManager::onPlayerStateChanged(QMediaPlayer::PlaybackState state)
{
    if (m_playingGreeting && state == QMediaPlayer::StoppedState) {
        m_playingGreeting = false;
        if (m_sessionAlert)
            m_sessionAlert->setSpeechBlocked(false);
        cleanupTempFiles();
        restoreHeadphonesPolicy();
        setState(QStringLiteral("idle"));
        qWarning() << "[LOBBY] greeting finished — headphones policy restored";
    }
}

void LobbyAudioManager::stopAll()
{
    m_fadeTimer.stop();
    m_awaitingGreeting = false;
    m_playingGreeting = false;
    m_guestMode = false;
    if (m_sessionAlert)
        m_sessionAlert->setSpeechBlocked(false);
    if (m_net)
        m_net->abortVoiceGreeting();
    if (m_player)
        m_player->stop();
    cleanupTempFiles();
    restoreHeadphonesPolicy();
    setState(QStringLiteral("idle"));
}
