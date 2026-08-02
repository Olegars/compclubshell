#include "voiceassistant.h"
#include "voicehotkeymonitor.h"
#include "networkmanager.h"
#include "processmanager.h"
#include "sessionalertmanager.h"
#include "audiomanager_win.h"

#include <QCoreApplication>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QMediaDevices>
#include <QDebug>

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

QString voiceTempDir()
{
    const QString dir = QStringLiteral("C:/ShellVideo/Voice");
    QDir().mkpath(dir);
    return dir;
}

} // namespace

VoiceAssistant::VoiceAssistant(NetworkManager *net,
                               ProcessManager *launcher,
                               SessionAlertManager *sessionAlert,
                               QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_launcher(launcher)
    , m_sessionAlert(sessionAlert)
{
    m_hotkey = new VoiceHotkeyMonitor(this);
    connect(m_hotkey, &VoiceHotkeyMonitor::holdStarted, this, &VoiceAssistant::onHoldStarted);
    connect(m_hotkey, &VoiceHotkeyMonitor::holdEnded, this, &VoiceAssistant::onHoldEnded);

    m_format.setSampleRate(16000);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);

    m_audioOut = new QAudioOutput(this);
    m_audioOut->setVolume(1.0f);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOut);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &VoiceAssistant::onPlayerStateChanged);

    m_errorClearTimer.setSingleShot(true);
    connect(&m_errorClearTimer, &QTimer::timeout, this, &VoiceAssistant::clearErrorState);

    if (m_net) {
        connect(m_net, &NetworkManager::aiAssistantSucceeded,
                this, &VoiceAssistant::onAiSuccess);
        connect(m_net, &NetworkManager::aiAssistantFailed,
                this, &VoiceAssistant::onAiFailed);
    }

    if (m_sessionAlert) {
        connect(m_sessionAlert, &SessionAlertManager::sessionActiveChanged, this, [this]() {
            setSessionActive(m_sessionAlert->sessionActive());
        });
        setSessionActive(m_sessionAlert->sessionActive());
    }

    loadConfig();
}

VoiceAssistant::~VoiceAssistant()
{
    cancelAndIdle(QStringLiteral("destroy"));
}

void VoiceAssistant::loadConfig()
{
    QSettings settings(resolveConfigPath(), QSettings::IniFormat);
    m_featureEnabled = settings.value(QStringLiteral("Voice/enabled"), true).toBool();
    const QString hotkeyName = settings.value(QStringLiteral("Voice/hotkey"),
                                              QStringLiteral("Grave")).toString();
    const int holdMs = settings.value(QStringLiteral("Voice/hold_ms"), 250).toInt();
    m_duckPercent = qBound(5, settings.value(QStringLiteral("Voice/duck_percent"), 20).toInt(), 80);
    m_minClipMs = qBound(200, settings.value(QStringLiteral("Voice/min_clip_ms"), 400).toInt(), 3000);

    m_hotkey->setVirtualKey(VoiceHotkeyMonitor::parseHotkeyName(hotkeyName));
    m_hotkey->setHoldMs(holdMs);
    emit enabledChanged();

    qWarning() << "[VOICE] config enabled=" << m_featureEnabled
               << "hotkey=" << hotkeyName
               << "holdMs=" << holdMs
               << "duck=" << m_duckPercent;
}

void VoiceAssistant::setSessionActive(bool active)
{
    if (m_sessionActive == active && m_hotkey->isEnabled() == (active && m_featureEnabled))
        return;
    m_sessionActive = active;
    const bool wantHook = m_sessionActive && m_featureEnabled;
    m_hotkey->setEnabled(wantHook);
    if (!wantHook)
        cancelAndIdle(QStringLiteral("session inactive"));
    qWarning() << "[VOICE] sessionActive=" << active << "hook=" << wantHook;
}

void VoiceAssistant::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void VoiceAssistant::duckAudio()
{
    if (m_ducked)
        return;
    win32_set_headphones_guard_paused(1);
    win32_duck_master(m_duckPercent);
    m_ducked = true;
}

void VoiceAssistant::restoreAudio()
{
    if (!m_ducked)
        return;
    win32_restore_master();
    win32_set_headphones_guard_paused(0);
    m_ducked = false;
}

void VoiceAssistant::abortNetwork()
{
    if (m_net)
        m_net->abortAiAssistant();
}

void VoiceAssistant::cancelAndIdle(const QString &reason)
{
    m_errorClearTimer.stop();
    stopCapture();
    abortNetwork();
    if (m_player)
        m_player->stop();
    if (!m_tempReplyPath.isEmpty()) {
        QFile::remove(m_tempReplyPath);
        m_tempReplyPath.clear();
    }
    if (!m_tempAskPath.isEmpty()) {
        QFile::remove(m_tempAskPath);
        m_tempAskPath.clear();
    }
    restoreAudio();
    if (m_sessionAlert)
        m_sessionAlert->setSpeechBlocked(false);
    if (m_state != QLatin1String("idle")) {
        if (!reason.isEmpty())
            qWarning() << "[VOICE] cancel:" << reason;
        setState(QStringLiteral("idle"));
    }
}

void VoiceAssistant::onHoldStarted()
{
    if (!m_sessionActive || !m_featureEnabled)
        return;
    if (m_state == QLatin1String("thinking") || m_state == QLatin1String("speaking"))
        return; // ignore while waiting / playing
    if (m_state == QLatin1String("listening"))
        return;
    beginListening();
}

void VoiceAssistant::onHoldEnded()
{
    if (m_state != QLatin1String("listening"))
        return;
    finishListeningAndSend();
}

void VoiceAssistant::beginListening()
{
    m_errorClearTimer.stop();
    if (m_player)
        m_player->stop();
    if (!startCapture()) {
        setState(QStringLiteral("error"));
        emit errorOccurred(QStringLiteral("Микрофон недоступен"));
        m_errorClearTimer.start(1500);
        return;
    }
    duckAudio();
    setState(QStringLiteral("listening"));
    qWarning() << "[VOICE] listening…";
}

bool VoiceAssistant::startCapture()
{
    stopCapture();
    m_pcmBuffer.clear();

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        qWarning() << "[VOICE] no default audio input";
        return false;
    }

    QAudioFormat fmt = m_format;
    if (!device.isFormatSupported(fmt)) {
        // Prefer 16-bit mono PCM for Whisper/WAV; fall back carefully.
        fmt = device.preferredFormat();
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(fmt)) {
            fmt.setSampleRate(device.preferredFormat().sampleRate());
            fmt.setSampleFormat(QAudioFormat::Int16);
            fmt.setChannelCount(1);
        }
        if (!device.isFormatSupported(fmt)) {
            qWarning() << "[VOICE] mic does not support Int16 PCM";
            return false;
        }
        m_format = fmt;
        qWarning() << "[VOICE] using mic format rate=" << fmt.sampleRate()
                   << "ch=" << fmt.channelCount();
    }

    m_audioSource = std::make_unique<QAudioSource>(device, m_format, this);
    m_audioDevice = m_audioSource->start();
    if (!m_audioDevice) {
        qWarning() << "[VOICE] QAudioSource start failed";
        m_audioSource.reset();
        return false;
    }

    connect(m_audioDevice, &QIODevice::readyRead, this, [this]() {
        if (m_audioDevice)
            m_pcmBuffer.append(m_audioDevice->readAll());
    });

    m_captureStartedMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void VoiceAssistant::stopCapture()
{
    if (m_audioSource) {
        m_audioSource->stop();
        m_audioSource.reset();
    }
    m_audioDevice = nullptr;
}

bool VoiceAssistant::writeWavFile(const QString &path, const QByteArray &pcm) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;

    const int sampleRate = m_format.sampleRate() > 0 ? m_format.sampleRate() : 16000;
    const int channels = m_format.channelCount() > 0 ? m_format.channelCount() : 1;
    const int bitsPerSample = 16;
    const quint32 byteRate = quint32(sampleRate * channels * bitsPerSample / 8);
    const quint16 blockAlign = quint16(channels * bitsPerSample / 8);
    const quint32 dataSize = quint32(pcm.size());
    const quint32 riffSize = 36 + dataSize;

    auto write32 = [&f](quint32 v) {
        char b[4] = { char(v), char(v >> 8), char(v >> 16), char(v >> 24) };
        f.write(b, 4);
    };
    auto write16 = [&f](quint16 v) {
        char b[2] = { char(v), char(v >> 8) };
        f.write(b, 2);
    };

    f.write("RIFF", 4);
    write32(riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write32(16);
    write16(1); // PCM
    write16(quint16(channels));
    write32(quint32(sampleRate));
    write32(byteRate);
    write16(blockAlign);
    write16(quint16(bitsPerSample));
    f.write("data", 4);
    write32(dataSize);
    f.write(pcm);
    f.close();
    return true;
}

void VoiceAssistant::finishListeningAndSend()
{
    if (m_audioDevice)
        m_pcmBuffer.append(m_audioDevice->readAll());
    stopCapture();

    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_captureStartedMs;
    if (elapsed < m_minClipMs || m_pcmBuffer.size() < 3200) {
        qWarning() << "[VOICE] clip too short, discard ms=" << elapsed << "bytes=" << m_pcmBuffer.size();
        restoreAudio();
        setState(QStringLiteral("idle"));
        return;
    }

    m_tempAskPath = voiceTempDir() + QStringLiteral("/ask-%1.wav")
                        .arg(QDateTime::currentMSecsSinceEpoch());
    if (!writeWavFile(m_tempAskPath, m_pcmBuffer)) {
        qWarning() << "[VOICE] failed to write wav";
        restoreAudio();
        setState(QStringLiteral("error"));
        emit errorOccurred(QStringLiteral("Не удалось сохранить запись"));
        m_errorClearTimer.start(1500);
        return;
    }
    m_pcmBuffer.clear();

    const int terminalId = m_net ? m_net->computerId() : 0;
    const int gameId = m_launcher ? m_launcher->currentGameId() : 0;
    const QString gameTitle = m_launcher ? m_launcher->gameTitle() : QString();

    setState(QStringLiteral("thinking"));
    qWarning() << "[VOICE] sending ask terminal=" << terminalId
               << "game=" << gameId << gameTitle
               << "file=" << m_tempAskPath;

    if (!m_net || terminalId <= 0) {
        onAiFailed(QStringLiteral("Терминал не зарегистрирован"));
        return;
    }

    m_net->askAiAssistant(terminalId, m_tempAskPath, gameId, gameTitle);
}

void VoiceAssistant::onAiSuccess(const QByteArray &audioBytes, const QString &mime,
                                 const QString &transcript, const QString &replyText)
{
    if (m_state != QLatin1String("thinking"))
        return;

    qWarning() << "[VOICE] reply ok transcript=" << transcript
               << "reply=" << replyText << "mime=" << mime
               << "bytes=" << audioBytes.size();

    if (!m_tempAskPath.isEmpty()) {
        QFile::remove(m_tempAskPath);
        m_tempAskPath.clear();
    }

    if (audioBytes.isEmpty()) {
        onAiFailed(QStringLiteral("Пустой ответ ассистента"));
        return;
    }

    playReply(audioBytes);
}

void VoiceAssistant::onAiFailed(const QString &message)
{
    if (m_state != QLatin1String("thinking") && m_state != QLatin1String("listening"))
        return;

    qWarning() << "[VOICE] failed:" << message;
    if (!m_tempAskPath.isEmpty()) {
        QFile::remove(m_tempAskPath);
        m_tempAskPath.clear();
    }
    restoreAudio();
    if (m_sessionAlert)
        m_sessionAlert->setSpeechBlocked(false);
    setState(QStringLiteral("error"));
    emit errorOccurred(message);
    m_errorClearTimer.start(2000);
}

void VoiceAssistant::playReply(const QByteArray &mp3Bytes)
{
    if (!m_tempReplyPath.isEmpty()) {
        QFile::remove(m_tempReplyPath);
        m_tempReplyPath.clear();
    }

    m_tempReplyPath = voiceTempDir() + QStringLiteral("/reply-%1.mp3")
                          .arg(QDateTime::currentMSecsSinceEpoch());
    QFile f(m_tempReplyPath);
    if (!f.open(QIODevice::WriteOnly) || f.write(mp3Bytes) != mp3Bytes.size()) {
        onAiFailed(QStringLiteral("Не удалось сохранить ответ"));
        return;
    }
    f.close();

    if (m_sessionAlert)
        m_sessionAlert->setSpeechBlocked(true);

    // Keep ducked while speaking so reply is audible.
    duckAudio();
    setState(QStringLiteral("speaking"));
    m_player->setSource(QUrl::fromLocalFile(m_tempReplyPath));
    m_player->play();
}

void VoiceAssistant::onPlayerStateChanged(QMediaPlayer::PlaybackState state)
{
    if (m_state != QLatin1String("speaking"))
        return;

    if (state == QMediaPlayer::StoppedState) {
        if (!m_tempReplyPath.isEmpty()) {
            QFile::remove(m_tempReplyPath);
            m_tempReplyPath.clear();
        }
        if (m_sessionAlert)
            m_sessionAlert->setSpeechBlocked(false);
        restoreAudio();
        setState(QStringLiteral("idle"));
        qWarning() << "[VOICE] playback finished";
    }
}

void VoiceAssistant::clearErrorState()
{
    if (m_state == QLatin1String("error"))
        setState(QStringLiteral("idle"));
}
