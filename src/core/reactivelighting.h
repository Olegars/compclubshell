#ifndef REACTIVELIGHTING_H
#define REACTIVELIGHTING_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class DmxController;
class ValveGsi;
class ChromaEmulator;
class GameSenseEmulator;

/**
 * Local game → Art-Net overlay. Shell is the only painter.
 * Sources: Valve GSI (CS2/Dota), Razer Chroma REST, SteelSeries GameSense,
 * plus club lifecycle events (pc_on / session / shutdown) from the cloud.
 */
class ReactiveLighting : public QObject
{
    Q_OBJECT
public:
    enum Priority {
        Idle = 0,
        Ambient = 20,
        Event = 50,
        Round = 70,
        Alert = 90
    };
    Q_ENUM(Priority)

    struct EventPreset {
        bool enabled = true;
        int durationMs = 0;
        QString color = QStringLiteral("white");
        QString effect = QStringLiteral("none");
        int brightness = 100;
        bool strobe = false;
        int strobeOnMs = 90;
        int strobeOffMs = 90;
        QStringList cycleColors;
        int cycleHoldMs = 400;
        int fadeMs = 300;
    };

    explicit ReactiveLighting(DmxController *dmx, QObject *parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }
    void setListen(bool on);
    ValveGsi *valveGsi() const { return m_valve; }
    QString lastEvent() const { return m_lastEvent; }

    void setEventPresets(const QJsonObject &events);
    bool playPreset(const QString &id, Priority pri, const QString &hint = QString(),
                    const QString &gameColor = QString(), int gameBrightness = -1);
    void pulse(const QString &source, Priority pri, const QString &color, int brightness,
               bool strobe = false, int ttlMs = 0, const QString &hint = QString());
    void release(const QString &source);

    static QString hexColor(int r, int g, int b);

signals:
    void lastEventChanged();

private:
    struct Layer {
        Priority pri = Idle;
        QString color;
        int brightness = 100;
        QString effect = QStringLiteral("none");
        bool strobe = false;
        int strobeOnMs = 90;
        int strobeOffMs = 90;
        QStringList cycleColors;
        int cycleHoldMs = 400;
        int fadeMs = 280;
        QString hint;
        qint64 stamp = 0;
        QTimer *ttl = nullptr;
    };

    EventPreset presetFor(const QString &id) const;
    EventPreset fallbackPreset(const QString &id) const;
    static EventPreset parsePreset(const QJsonObject &obj, const EventPreset &base);
    static double jsonNum(const QJsonObject &obj, const QString &key, double fallback);
    static bool jsonBool(const QJsonObject &obj, const QString &key, bool fallback);
    void applyTop();
    void setHint(const QString &text);
    void clearLayers();
    void armTtl(Layer &layer, const QString &source, int ttlMs);
    void releaseLifecycleExcept(const QString &keep);

    DmxController *m_dmx = nullptr;
    ValveGsi *m_valve = nullptr;
    ChromaEmulator *m_chroma = nullptr;
    GameSenseEmulator *m_sense = nullptr;
    bool m_enabled = false;
    QString m_lastEvent;
    QHash<QString, Layer> m_layers;
    QHash<QString, EventPreset> m_presets;
};

#endif
