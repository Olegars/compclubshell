#ifndef OPENRGBCLIENT_H
#define OPENRGBCLIENT_H

#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QString>
#include <QVector>

class QTcpSocket;
class QTimer;

/**
 * Local OpenRGB SDK client (127.0.0.1:6742).
 * Runs on its own thread. Mirrors one RGB into the motherboard ADDR_LED zone
 * in Direct mode. Socket stalls stay off the shell GUI thread.
 *
 * Binary layout follows OpenRGB RGBController::SetDeviceDescription, protocol 1–6.
 */
class OpenRgbClient : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString host = QStringLiteral("127.0.0.1");
        int port = 6742;
        QString deviceFilter = QStringLiteral("ASRock");
        int zoneIndex = -1;
        int maxFps = 20;
        int retrySec = 3;
        int ledCount = 0;
    };

    explicit OpenRgbClient(const Config &config, QObject *parent = nullptr);

signals:
    void statusChanged(bool connected, const QString &text);

public slots:
    void start();
    void submitColor(const QColor &color);
    void holdTestColor(const QColor &color, int durationMs);
    void blackout();

private:
    struct ZoneInfo {
        QString name;
        int leds = 0;
    };
    struct DevInfo {
        bool ok = false;
        quint32 type = 0;
        QString name;
        QString vendor;
        QVector<ZoneInfo> zones;
    };

    void connectNow();
    void drop(const QString &why);
    void setStatus(bool ok, const QString &text);
    void warnThrottled(const QString &why);
    void onReadyRead();
    void handlePacket(quint32 id, quint32 dev, const QByteArray &payload);
    void beginScan();
    void requestDevice(int index);
    void considerDevice(const DevInfo &dev, int index);
    void finishScan();
    void commitSelection();
    void scheduleSend();
    void flushSend();
    bool sendPacket(quint32 dev, quint32 id, const QByteArray &payload);
    bool writeZone(const QColor &color);
    int scoreDevice(const DevInfo &dev) const;
    int selectZone(const QVector<ZoneInfo> &zones) const;
    static DevInfo parseDevice(const QByteArray &payload, quint32 protocol);

    Config m_cfg;
    QTcpSocket *m_sock = nullptr;
    QTimer *m_retry = nullptr;
    QTimer *m_flush = nullptr;
    QTimer *m_testTimer = nullptr;
    QTimer *m_watch = nullptr;
    QByteArray m_buf;
    QString m_statusText;
    QString m_readyLabel;
    bool m_statusOk = false;
    bool m_stopped = false;
    bool m_dropping = false;
    bool m_ready = false;
    bool m_resizePending = false;
    bool m_hasLive = false;
    bool m_hasSent = false;
    bool m_testActive = false;
    QColor m_live;
    QColor m_test;
    int m_sentR = -1;
    int m_sentG = -1;
    int m_sentB = -1;
    qint64 m_lastSendMs = 0;
    qint64 m_lastWarnMs = 0;
    int m_minIntervalMs = 50;
    int m_retryMs = 3000;
    quint32 m_proto = 5;
    int m_phase = 0;
    int m_count = 0;
    int m_cursor = 0;
    int m_bestScore = -1;
    int m_bestDev = -1;
    int m_bestZone = -1;
    int m_bestLeds = 0;
    QString m_bestZoneName;
    QString m_bestDevName;
    int m_dev = -1;
    int m_zone = -1;
    int m_leds = 0;
};

#endif
