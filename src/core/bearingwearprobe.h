#ifndef BEARINGWEARPROBE_H
#define BEARINGWEARPROBE_H

#include <QObject>
#include <QByteArray>
#include <QAudioSource>
#include <QTimer>
#include <QString>
#include <memory>

class NetworkManager;
class VoiceAssistant;

/**
 * Ночной слухач HW-584: краткий разгон персонального вентилятора до 3-й скорости
 * и поиск паразитного ВЧ-свиста подшипника. Пишет /admin/incidents.
 */
class BearingWearProbe : public QObject
{
    Q_OBJECT
public:
    explicit BearingWearProbe(NetworkManager *net,
                              VoiceAssistant *voice,
                              QObject *parent = nullptr);
    ~BearingWearProbe() override;

private slots:
    void tick();

private:
    struct BandStats {
        double rms = 0;
        double highRms = 0;
        double peakHz = 0;
        double peakToMean = 0;
    };

    void loadConfig();
    bool inNightWindow() const;
    bool canProbe() const;
    void beginProbe();
    void startCapture(const QString &phase);
    void finishCapture();
    void compareAndReport();
    void restoreFan();
    void abortProbe(const QString &reason);
    static BandStats analyzePcm(const QByteArray &pcm, int sampleRate);

    NetworkManager *m_net = nullptr;
    VoiceAssistant *m_voice = nullptr;
    QTimer m_tick;
    std::unique_ptr<QAudioSource> m_source;
    QIODevice *m_io = nullptr;

    bool m_enabled = true;
    int m_nightStart = 1;
    int m_nightEnd = 6;
    int m_holdMs = 4000;
    int m_sampleRate = 16000;
    double m_highRatio = 0.22;
    double m_peakProminence = 4.5;
    double m_minRms = 180.0;

    bool m_busy = false;
    QString m_phase;
    QByteArray m_pcm;
    int m_captureFormat = 0;
    BandStats m_baseline;
    BandStats m_high;
    QString m_lastNightKey;
};

#endif // BEARINGWEARPROBE_H
