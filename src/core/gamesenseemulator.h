#ifndef GAMESENSEEMULATOR_H
#define GAMESENSEEMULATOR_H

#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include "localhttpserver.h"
#include "reactivelighting.h"

/** Fake SteelSeries Engine: coreProps.json + GameSense HTTP. */
class GameSenseEmulator : public QObject
{
    Q_OBJECT
public:
    explicit GameSenseEmulator(ReactiveLighting *hub, QObject *parent = nullptr);

    void setEnabled(bool on);

private:
    LocalHttpResponse onHttp(const LocalHttpRequest &req);
    void handleGameEvent(const QJsonObject &root);
    bool writeCoreProps(quint16 port);
    void removeCoreProps();
    QStringList corePropsPaths() const;

    ReactiveLighting *m_hub = nullptr;
    LocalHttpServer m_server;
    bool m_enabled = false;
    QStringList m_written;
};

#endif
