#ifndef DMXCONTROLLER_H
#define DMXCONTROLLER_H

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

/**
 * Art-Net (ArtDmx, UDP 6454) → DMX512 node on LAN.
 * Cloud never talks to the node; Shell sends the universe.
 *
 * Fixture layouts (start is 1-based DMX):
 *   rgb        — R G B, brightness scales RGB
 *   dimmer_rgb — dimmer R G B
 *   rgbw       — R G B W (white preset uses W)
 *
 * Rainbow: HSV from wall clock so every PC in the room sends the same hue.
 */
class DmxController : public QObject
{
    Q_OBJECT
public:
    struct Fixture {
        int start = 1;
        int count = 1;
        QString layout;
        QString color;
        int brightness = 0;
        QString effect;
        int r = 255;
        int g = 255;
        int b = 255;
    };

    struct Node {
        QString host;
        int port = 6454;
        int universe = 0;
        QVector<Fixture> fixtures;
    };

    explicit DmxController(QObject *parent = nullptr);

    void setNodes(const QVector<Node> &nodes, int fadeMs = 0);
    void setRainbowPeriodMs(int ms);
    void setAllBrightness(int brightness);
    bool hasNodes() const { return !m_nodes.isEmpty(); }
    bool sendOnce(QString *errorOut = nullptr);
    void startRefresh();
    void stopRefresh();
    bool refreshRunning() const;
    bool fading() const;

    static QColor scaledRgb(const QString &color, int brightness, int r, int g, int b,
                             const QString &effect, int rainbowPeriodMs);

private:
    QByteArray buildArtDmx(int universe, const QByteArray &dmx) const;
    void paintUniverse(QByteArray &dmx, const Node &node);
    void writeFixture(QByteArray &dmx, const Fixture &fx, const QColor &c) const;
    void onTick();
    QString fixtureKey(const Node &node, const Fixture &fx) const;
    static QColor lerpColor(const QColor &from, const QColor &to, float t);

    QUdpSocket m_sock;
    QTimer m_timer;
    QVector<Node> m_nodes;
    int m_rainbowPeriodMs = 8000;
    quint8 m_sequence = 1;
    QHash<QString, QColor> m_liveColors;
    QHash<QString, QColor> m_fadeFrom;
    qint64 m_fadeStartMs = 0;
    int m_fadeMs = 0;
};

#endif // DMXCONTROLLER_H
