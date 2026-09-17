#ifndef FANRELAYCONTROLLER_H
#define FANRELAYCONTROLLER_H

#include <QObject>
#include <QString>

/**
 * HW-584 16-channel relay (NetMod-ServerApp).
 * GET http://{host}:{tcpPort}/{cmd}  — TCP-порт, по умолчанию 8080.
 * Старый заводской W5100 (driver w5100_http): http://{host}/{pathPort}/{cmd} на TCP :80.
 *
 * Channel N (1–16): OFF=(N-1)*2, ON=(N-1)*2+1 as zero-padded 00–31.
 * Status: GET …/99 → 16 ASCII 0/1, первый символ = IO 1.
 *
 * Пины в NetMod должны быть Output, иначе /00–/31 молчат.
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
    static constexpr int DefaultTcpPort = 8080;

    struct Result {
        bool ok = false;
        QString error;
        QString body;
    };

    /** Factory W5100 used a path segment; NetMod uses a real TCP port. */
    static bool usesPathPort(const QString &driver);

    static QString commandUrl(const QString &host, int port, const QString &cmd,
                              const QString &driver = QString());

    static QString commandForChannel(int channel, bool on);
    static Result setChannel(const QString &host, int port, int channel, bool on,
                             int timeoutMs = 2000, const QString &driver = QString());
    static Result readStatus(const QString &host, int port, int timeoutMs = 2000,
                             const QString &driver = QString());
    /** Returns -1 on error, 0 off, 1 on. */
    static int channelStateFromStatus(const QString &statusBody, int channel);

    /**
     * Apply cascade speed 1..3 on K1/K2.
     * При |Δ|≥2 сначала mid на softStepMs, затем цель.
     * Returns applied speed or -1.
     */
    static int setSpeed(const QString &host, int port, int channelK1, int channelK2,
                        int speed, QString *errorOut = nullptr, int timeoutMs = 2000,
                        int softStepMs = SoftStepMs, const QString &driver = QString());

    /** Decode speed from /99 bits for K1/K2. */
    static int speedFromStatus(const QString &statusBody, int channelK1, int channelK2);
};

#endif // FANRELAYCONTROLLER_H
