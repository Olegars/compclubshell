#ifndef FANRELAYCONTROLLER_H
#define FANRELAYCONTROLLER_H

#include <QObject>
#include <QString>

/**
 * HW-584 / W5100 16-channel network relay.
 * HTTP на TCP :80, «порт» модуля — сегмент пути:
 *   GET http://{host}/{modulePort}/{cmd}
 * например http://192.168.1.4/30000/99
 * Channel N (1–16): OFF=(N-1)*2, ON=(N-1)*2+1 as zero-padded 00–31.
 * Status: GET …/{modulePort}/99 → 16 ASCII bits, MSB = relay 1.
 *
 * 3-speed cascade (2 channels per fan):
 *   speed 1 night 120V: K1=OFF K2=OFF
 *   speed 2 mid   170V: K1=ON  K2=OFF
 *   speed 3 high  220V: K1=OFF K2=ON
 *
 * Прыжок 1↔3 идёт через mid (~softStepMs), чтобы смягчить каскад.
 */
class FanRelayController
{
public:
    static constexpr int SoftStepMs = 2500;

    struct Result {
        bool ok = false;
        QString error;
        QString body;
    };

    /** Build http://host/{modulePort}/{cmd} (TCP 80). */
    static QString commandUrl(const QString &host, int modulePort, const QString &cmd);

    static QString commandForChannel(int channel, bool on);
    static Result setChannel(const QString &host, int modulePort, int channel, bool on,
                             int timeoutMs = 2000);
    static Result readStatus(const QString &host, int modulePort, int timeoutMs = 2000);
    /** Returns -1 on error, 0 off, 1 on. */
    static int channelStateFromStatus(const QString &statusBody, int channel);

    /**
     * Apply cascade speed 1..3 on K1/K2.
     * При |Δ|≥2 сначала mid на softStepMs, затем цель.
     * Returns applied speed or -1.
     */
    static int setSpeed(const QString &host, int modulePort, int channelK1, int channelK2,
                        int speed, QString *errorOut = nullptr, int timeoutMs = 2000,
                        int softStepMs = SoftStepMs);

    /** Decode speed from /99 bits for K1/K2. */
    static int speedFromStatus(const QString &statusBody, int channelK1, int channelK2);
};

#endif // FANRELAYCONTROLLER_H
