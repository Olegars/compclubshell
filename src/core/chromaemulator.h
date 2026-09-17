#ifndef CHROMAEMULATOR_H
#define CHROMAEMULATOR_H

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include "localhttpserver.h"
#include "reactivelighting.h"

/** Razer Chroma REST on 127.0.0.1:54235 — games talk to us instead of Synapse. */
class ChromaEmulator : public QObject
{
    Q_OBJECT
public:
    static const quint16 kPort = 54235;

    explicit ChromaEmulator(ReactiveLighting *hub, QObject *parent = nullptr);

    void setEnabled(bool on);

private:
    LocalHttpResponse onHttp(const LocalHttpRequest &req);
    void applyEffect(const QJsonObject &root);
    QColor colorFromEffect(const QJsonObject &root) const;
    void sessionAlive();

    ReactiveLighting *m_hub = nullptr;
    LocalHttpServer m_server;
    QTimer m_watchdog;
    bool m_enabled = false;
    int m_session = 0;
    bool m_hasSession = false;
};

#endif
