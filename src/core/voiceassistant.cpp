#include "voiceassistant.h"
#include "voicehotkeymonitor.h"
#include "networkmanager.h"
#include "pathresolver.h"
#include "processmanager.h"
#include "sessionalertmanager.h"
#include "audiomanager_win.h"

#include <QCoreApplication>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QDebug>
#include <QTimer>

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
    const QString dir = PathResolver::instance()
            ? PathResolver::instance()->voiceDir()
            : QStringLiteral("C:/ShellVideo/Voice");
    QDir().mkpath(dir);
    return dir;
}

int micScore(const QString &name)
{
    const QString n = name.toLower();
    if (n.contains(QStringLiteral("stereo mix"))
            || n.contains(QStringLiteral("стереомикшер"))
            || n.contains(QStringLiteral("what u hear"))
            || n.contains(QStringLiteral("loopback"))
            || n.contains(QStringLiteral("wave out")))
        return -200;
    if (n.contains(QStringLiteral("hdmi")) || n.contains(QStringLiteral("display")))
        return -80;
    int score = 0;
    if (n.contains(QStringLiteral("headset"))
            || n.contains(QStringLiteral("headphone"))
            || n.contains(QStringLiteral("наушник"))
            || n.contains(QStringLiteral("гарнитур")))
        score += 80;
    if (n.contains(QStringLiteral("mic"))
            || n.contains(QStringLiteral("микро")))
        score += 40;
    if (n.contains(QStringLiteral("usb")) || n.contains(QStringLiteral("realtek")))
        score += 10;
    return score;
}

QAudioDevice pickCaptureDevice()
{
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    QAudioDevice best = QMediaDevices::defaultAudioInput();
    int bestScore = best.isNull() ? -1000 : micScore(best.description());
    for (const QAudioDevice &dev : inputs) {
        const int score = micScore(dev.description());
        if (score > bestScore) {
            best = dev;
            bestScore = score;
        }
    }
    return best;
}

qint16 pcmPeakAbs(const QByteArray &pcm)
{
    const int n = pcm.size() / 2;
    const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
    qint16 peak = 0;
    for (int i = 0; i < n; ++i) {
        const qint16 a = qAbs(s[i]);
        if (a > peak)
            peak = a;
    }
    return peak;
}

QByteArray pcmToInt16(const QByteArray &raw, QAudioFormat::SampleFormat sampleFormat)
{
    if (sampleFormat == QAudioFormat::Int16)
        return raw;

    if (sampleFormat == QAudioFormat::Float) {
        const int n = raw.size() / int(sizeof(float));
        const auto *f = reinterpret_cast<const float *>(raw.constData());
        QByteArray out;
        out.resize(n * 2);
        auto *s = reinterpret_cast<qint16 *>(out.data());
        for (int i = 0; i < n; ++i) {
            const float x = qBound(-1.0f, f[i], 1.0f);
            s[i] = qint16(x * 32767.0f);
        }
        return out;
    }

    if (sampleFormat == QAudioFormat::Int32) {
        const int n = raw.size() / int(sizeof(qint32));
        const auto *src = reinterpret_cast<const qint32 *>(raw.constData());
        QByteArray out;
        out.resize(n * 2);
        auto *s = reinterpret_cast<qint16 *>(out.data());
        for (int i = 0; i < n; ++i)
            s[i] = qint16(src[i] >> 16);
        return out;
    }

    if (sampleFormat == QAudioFormat::UInt8) {
        const int n = raw.size();
        const auto *u = reinterpret_cast<const quint8 *>(raw.constData());
        QByteArray out;
        out.resize(n * 2);
        auto *s = reinterpret_cast<qint16 *>(out.data());
        for (int i = 0; i < n; ++i)
            s[i] = qint16((int(u[i]) - 128) * 256);
        return out;
    }

    return raw;
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
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &err) {
                if (m_state != QLatin1String("speaking"))
                    return;
                const QString msg = err.trimmed().isEmpty()
                        ? QStringLiteral("Плеер не смог озвучить ответ")
                        : err;
                qWarning() << "[VOICE] player error:" << msg;
                onAiFailed(msg);
            });

    m_errorClearTimer.setSingleShot(true);
    connect(&m_errorClearTimer, &QTimer::timeout, this, &VoiceAssistant::clearErrorState);

    if (m_net) {
        connect(m_net, &NetworkManager::aiAssistantSucceeded,
                this, &VoiceAssistant::onAiSuccess);
        connect(m_net, &NetworkManager::aiAssistantFailed,
                this, &VoiceAssistant::onAiFailed);
        connect(m_net, &NetworkManager::ttsPreviewSucceeded,
                this, &VoiceAssistant::onTtsPreview);
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

void VoiceAssistant::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
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
    setLastError(QString());
    if (!startCapture()) {
        setLastError(QStringLiteral("Микрофон недоступен"));
        setState(QStringLiteral("error"));
        emit errorOccurred(QStringLiteral("Микрофон недоступен"));
        m_errorClearTimer.start(8000);
        return;
    }
    setState(QStringLiteral("listening"));
    qWarning() << "[VOICE] listening…";
}

bool VoiceAssistant::startCapture()
{
    stopCapture();
    m_pcmBuffer.clear();

    const QAudioDevice device = pickCaptureDevice();
    if (device.isNull()) {
        qWarning() << "[VOICE] no default audio input";
        return false;
    }

    QAudioFormat fmt = device.preferredFormat();
    if (fmt.channelCount() < 1)
        fmt.setChannelCount(1);
    if (fmt.sampleRate() < 8000)
        fmt.setSampleRate(16000);
    if (fmt.sampleFormat() == QAudioFormat::Unknown)
        fmt.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(fmt)) {
        fmt = m_format;
        if (!device.isFormatSupported(fmt)) {
            qWarning() << "[VOICE] mic format not supported";
            return false;
        }
    }

    m_audioSource = std::make_unique<QAudioSource>(device, fmt, this);
    m_audioSource->setVolume(1.0f);
    m_audioDevice = m_audioSource->start();
    if (!m_audioDevice) {
        qWarning() << "[VOICE] QAudioSource start failed";
        m_audioSource.reset();
        return false;
    }

    m_format = m_audioSource->format();
    qWarning() << "[VOICE] capture device=" << device.description()
               << "rate=" << m_format.sampleRate()
               << "ch=" << m_format.channelCount()
               << "sample=" << int(m_format.sampleFormat());

    connect(m_audioDevice, &QIODevice::readyRead, this, [this]() {
        if (m_audioDevice)
            m_pcmBuffer.append(m_audioDevice->readAll());
    });

    auto *pull = new QTimer(m_audioSource.get());
    pull->setInterval(50);
    connect(pull, &QTimer::timeout, this, [this]() {
        if (m_audioDevice)
            m_pcmBuffer.append(m_audioDevice->readAll());
    });
    pull->start();

    m_captureStartedMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void VoiceAssistant::stopCapture()
{
    if (m_audioSource) {
        m_audioSource->stop();
        if (m_audioDevice)
            m_pcmBuffer.append(m_audioDevice->readAll());
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
    stopCapture();

    const QByteArray pcm = pcmToInt16(m_pcmBuffer, m_format.sampleFormat());
    m_pcmBuffer.clear();

    const int rate = qMax(1, m_format.sampleRate());
    const int channels = qMax(1, m_format.channelCount());
    const int frames = pcm.size() / (2 * channels);
    const int durationMs = int((qint64(frames) * 1000) / rate);
    const qint16 peak = pcmPeakAbs(pcm);
    qWarning() << "[VOICE] clip ms=" << durationMs
               << "bytes=" << pcm.size()
               << "peak=" << int(peak)
               << "rate=" << rate
               << "ch=" << channels
               << "sample=" << int(m_format.sampleFormat());

    if (durationMs < m_minClipMs || frames < 1600) {
        qWarning() << "[VOICE] clip too short, discard";
        restoreAudio();
        setState(QStringLiteral("idle"));
        return;
    }
    if (peak < 80) {
        qWarning() << "[VOICE] clip is digital silence";
        restoreAudio();
        setState(QStringLiteral("error"));
        const QString msg = QStringLiteral("Микрофон молчит. Выберите гарнитуру как устройство ввода.");
        emit errorOccurred(msg);
        setLastError(msg);
        m_errorClearTimer.start(8000);
        return;
    }

    m_tempAskPath = voiceTempDir() + QStringLiteral("/ask-%1.wav")
                        .arg(QDateTime::currentMSecsSinceEpoch());
    if (!writeWavFile(m_tempAskPath, pcm)) {
        qWarning() << "[VOICE] failed to write wav";
        restoreAudio();
        setState(QStringLiteral("error"));
        emit errorOccurred(QStringLiteral("Не удалось сохранить запись"));
        setLastError(QStringLiteral("Не удалось сохранить запись"));
        m_errorClearTimer.start(8000);
        return;
    }

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
    if (m_state != QLatin1String("thinking")
            && m_state != QLatin1String("listening")
            && m_state != QLatin1String("speaking"))
        return;

    qWarning() << "[VOICE] failed:" << message;
    if (!m_tempAskPath.isEmpty()) {
        QFile::remove(m_tempAskPath);
        m_tempAskPath.clear();
    }
    if (!m_tempReplyPath.isEmpty()) {
        QFile::remove(m_tempReplyPath);
        m_tempReplyPath.clear();
    }
    if (m_player)
        m_player->stop();
    restoreAudio();
    if (m_sessionAlert)
        m_sessionAlert->setSpeechBlocked(false);
    setLastError(message);
    setState(QStringLiteral("error"));
    emit errorOccurred(message);
    m_errorClearTimer.start(8000);
}

void VoiceAssistant::onTtsPreview(const QByteArray &audioBytes, const QString &)
{
    if (audioBytes.isEmpty())
        return;
    if (m_state == QLatin1String("listening") || m_state == QLatin1String("thinking"))
        return;
    playReply(audioBytes);
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

    // Duck режет весь endpoint, включая этот плеер. Приветствие громкое, потому что
    // не дакает; ответ должен звучать так же — снимаем приглушение перед TTS.
    restoreAudio();
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
