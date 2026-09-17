#include "bearingwearprobe.h"

#include "networkmanager.h"
#include "voiceassistant.h"
#include "pathresolver.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QMediaDevices>
#include <QSettings>
#include <QtMath>
#include <QTime>
#include <algorithm>
#include <cmath>

namespace {

int roomMicScore(const QString &name)
{
    const QString n = name.toLower();
    if (n.contains(QLatin1String("headset")) || n.contains(QLatin1String("headphone"))
        || n.contains(QLatin1String("discord")) || n.contains(QLatin1String("broadcast")))
        return -20;
    int score = 0;
    if (n.contains(QLatin1String("array")) || n.contains(QLatin1String("internal")))
        score += 30;
    if (n.contains(QLatin1String("realtek")) || n.contains(QLatin1String("mic in")))
        score += 18;
    if (n.contains(QLatin1String("analog")) || n.contains(QLatin1String("rear")))
        score += 12;
    if (n.contains(QLatin1String("microphone")))
        score += 4;
    return score;
}

QAudioDevice pickRoomMic()
{
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    QAudioDevice best = QMediaDevices::defaultAudioInput();
    int bestScore = best.isNull() ? -1000 : roomMicScore(best.description());
    for (const QAudioDevice &dev : inputs) {
        const int score = roomMicScore(dev.description());
        if (score > bestScore) {
            best = dev;
            bestScore = score;
        }
    }
    return best;
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
        for (int i = 0; i < n; ++i)
            s[i] = qint16(qBound(-1.0f, f[i], 1.0f) * 32767.0f);
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
    return raw;
}

} // namespace

BearingWearProbe::BearingWearProbe(NetworkManager *net,
                                   VoiceAssistant *voice,
                                   QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_voice(voice)
{
    loadConfig();
    m_tick.setInterval(45000);
    connect(&m_tick, &QTimer::timeout, this, &BearingWearProbe::tick);
    if (m_enabled)
        m_tick.start();
    QTimer::singleShot(20000, this, &BearingWearProbe::tick);
}

BearingWearProbe::~BearingWearProbe()
{
    abortProbe(QStringLiteral("destroy"));
}

void BearingWearProbe::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("BearingProbe/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_nightStart = qBound(0, s.value(QStringLiteral("BearingProbe/night_start"), 1).toInt(), 23);
    m_nightEnd = qBound(0, s.value(QStringLiteral("BearingProbe/night_end"), 6).toInt(), 23);
    m_holdMs = qBound(1500, s.value(QStringLiteral("BearingProbe/hold_ms"), 4000).toInt(), 12000);
    m_highRatio = s.value(QStringLiteral("BearingProbe/high_ratio"), 0.22).toDouble();
    m_peakProminence = s.value(QStringLiteral("BearingProbe/peak_prominence"), 4.5).toDouble();
    m_minRms = s.value(QStringLiteral("BearingProbe/min_rms"), 180).toDouble();
}

bool BearingWearProbe::inNightWindow() const
{
    const int h = QTime::currentTime().hour();
    if (m_nightStart == m_nightEnd)
        return true;
    if (m_nightStart < m_nightEnd)
        return h >= m_nightStart && h < m_nightEnd;
    return h >= m_nightStart || h < m_nightEnd;
}

bool BearingWearProbe::canProbe() const
{
    if (!m_enabled || m_busy || !m_net)
        return false;
    if (!inNightWindow())
        return false;
    if (m_net->isGuestSessionActive() || m_net->maintenance())
        return false;
    if (m_voice && m_voice->isUiActive())
        return false;
    if (!m_net->hasPersonalFanRelay())
        return false;
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    if (m_lastNightKey == today)
        return false;
    const int staggerSec = (m_net->computerId() > 0 ? (m_net->computerId() % 17) : 0) * 80;
    const QTime start(m_nightStart, 0);
    const int elapsed = start.secsTo(QTime::currentTime());
    const int sinceStart = elapsed >= 0 ? elapsed : elapsed + 24 * 3600;
    if (sinceStart < staggerSec)
        return false;
    return true;
}

void BearingWearProbe::tick()
{
    if (!canProbe())
        return;
    beginProbe();
}

void BearingWearProbe::beginProbe()
{
    m_busy = true;
    m_pcm.clear();
    m_baseline = {};
    m_high = {};
    m_net->setFanProbeLock(true);
    qWarning() << "[BEARING] night probe start, pc" << m_net->getCurrentPcName();
    startCapture(QStringLiteral("baseline"));
}

void BearingWearProbe::startCapture(const QString &phase)
{
    finishCapture();
    m_phase = phase;
    m_pcm.clear();

    const QAudioDevice device = pickRoomMic();
    if (device.isNull()) {
        abortProbe(QStringLiteral("no mic"));
        return;
    }

    QAudioFormat fmt = device.preferredFormat();
    fmt.setChannelCount(1);
    fmt.setSampleRate(m_sampleRate);
    if (!device.isFormatSupported(fmt)) {
        fmt = device.preferredFormat();
        fmt.setChannelCount(1);
    }
    m_sampleRate = fmt.sampleRate() > 0 ? fmt.sampleRate() : 16000;
    m_captureFormat = int(fmt.sampleFormat());

    m_source = std::make_unique<QAudioSource>(device, fmt, this);
    m_io = m_source->start();
    if (!m_io) {
        abortProbe(QStringLiteral("mic start fail"));
        return;
    }
    connect(m_io, &QIODevice::readyRead, this, [this]() {
        if (m_io)
            m_pcm.append(m_io->readAll());
    });

    const int captureMs = (phase == QLatin1String("baseline")) ? 1400 : qMax(1800, m_holdMs - 1500);
    QTimer::singleShot(captureMs, this, [this, phase]() {
        if (!m_busy || m_phase != phase)
            return;
        finishCapture();
        const QByteArray pcm = pcmToInt16(m_pcm, QAudioFormat::SampleFormat(m_captureFormat));
        if (phase == QLatin1String("baseline")) {
            m_baseline = analyzePcm(pcm, m_sampleRate);
            m_net->setPersonalFanSpeedDirect(3);
            QTimer::singleShot(1100, this, [this]() {
                if (m_busy)
                    startCapture(QStringLiteral("high"));
            });
            return;
        }
        m_high = analyzePcm(pcm, m_sampleRate);
        compareAndReport();
        restoreFan();
    });
}

void BearingWearProbe::finishCapture()
{
    if (m_source) {
        m_source->stop();
        m_source.reset();
    }
    m_io = nullptr;
}

void BearingWearProbe::compareAndReport()
{
    const double highRatio = (m_high.rms > 1.0) ? (m_high.highRms / m_high.rms) : 0;
    const double rmsGain = (m_baseline.highRms > 1.0) ? (m_high.highRms / m_baseline.highRms) : 0;
    const double peakGain = m_high.peakToMean - m_baseline.peakToMean;
    const bool tonal = m_high.peakHz >= 2200 && m_high.peakHz <= 9500
        && m_high.peakToMean >= m_peakProminence;
    const bool louder = m_high.rms >= m_minRms && (rmsGain >= 1.35 || highRatio >= m_highRatio);
    const bool wear = tonal && louder && peakGain >= 1.2;

    qWarning() << "[BEARING] stats rms" << m_high.rms << "highRms" << m_high.highRms
               << "peakHz" << m_high.peakHz << "prom" << m_high.peakToMean
               << "ratio" << highRatio << "wear" << wear;

    m_lastNightKey = QDate::currentDate().toString(Qt::ISODate);
    if (!wear)
        return;

    const QString pc = m_net->getCurrentPcName();
    const QString desc = QStringLiteral("Подшипник SpaceFan на %1 изношен, требуется смазка")
                             .arg(pc);
    QJsonObject payload;
    payload.insert(QStringLiteral("peak_hz"), int(m_high.peakHz));
    payload.insert(QStringLiteral("rms"), m_high.rms);
    payload.insert(QStringLiteral("high_ratio"), highRatio);
    payload.insert(QStringLiteral("fan_speed"), 3);
    m_net->reportShellIncident(QStringLiteral("fan_bearing_wear"),
                               QStringLiteral("medium"),
                               desc,
                               payload);
}

void BearingWearProbe::restoreFan()
{
    if (m_net) {
        m_net->setPersonalFanSpeedDirect(1);
        m_net->setFanProbeLock(false);
    }
    m_busy = false;
    m_phase.clear();
}

void BearingWearProbe::abortProbe(const QString &reason)
{
    qWarning() << "[BEARING] abort" << reason;
    finishCapture();
    restoreFan();
}

BearingWearProbe::BandStats BearingWearProbe::analyzePcm(const QByteArray &pcm, int sampleRate)
{
    BandStats out;
    const int n = pcm.size() / 2;
    if (n < 512 || sampleRate < 8000)
        return out;
    const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
    const int start = n / 5; // skip spool / settle
    const int count = n - start;
    if (count < 256)
        return out;

    double acc = 0;
    double prev = 0;
    double hpPrev = 0;
    double hpAcc = 0;
    const double rc = 1.0 / (2.0 * M_PI * 2000.0);
    const double dt = 1.0 / double(sampleRate);
    const double alpha = rc / (rc + dt);
    for (int i = start; i < n; ++i) {
        const double x = double(s[i]);
        acc += x * x;
        const double hp = alpha * (hpPrev + x - prev);
        hpAcc += hp * hp;
        hpPrev = hp;
        prev = x;
    }
    out.rms = std::sqrt(acc / double(count));
    out.highRms = std::sqrt(hpAcc / double(count));

    const int win = std::min(4096, count);
    const int offset = n - win;
    const double fs = double(sampleRate);
    double bestMag = 0;
    double bestHz = 0;
    double sumMag = 0;
    int bins = 0;
    const int k0 = int(2200.0 * win / fs);
    const int k1 = int(std::min(fs / 2.0 - 50.0, 9500.0) * win / fs);
    for (int k = std::max(1, k0); k <= k1; k += 4) {
        double re = 0;
        double im = 0;
        const double w = 2.0 * M_PI * double(k) / double(win);
        for (int i = 0; i < win; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * double(i) / double(win - 1));
            const double x = double(s[offset + i]) * hann;
            re += x * std::cos(w * i);
            im -= x * std::sin(w * i);
        }
        const double mag = std::sqrt(re * re + im * im);
        sumMag += mag;
        ++bins;
        if (mag > bestMag) {
            bestMag = mag;
            bestHz = double(k) * fs / double(win);
        }
    }
    const double mean = bins > 0 ? (sumMag / double(bins)) : 1.0;
    out.peakHz = bestHz;
    out.peakToMean = mean > 1.0 ? (bestMag / mean) : 0;
    return out;
}
