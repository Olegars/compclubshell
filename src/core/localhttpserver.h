#ifndef LOCALHTTPSERVER_H
#define LOCALHTTPSERVER_H

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <functional>

struct LocalHttpRequest {
    QString method;
    QString path;
    QHash<QString, QString> headers;
    QByteArray body;
};

struct LocalHttpResponse {
    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray body = "{}";
    /** If set, stream this file instead of body (Content-Length from size). */
    QString filePath;
};

/** Tiny localhost HTTP/1.1 server for game RGB/GSI callbacks. Bind loopback only. */
class LocalHttpServer : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<LocalHttpResponse(const LocalHttpRequest &)>;

    explicit LocalHttpServer(QObject *parent = nullptr);

    void setHandler(Handler handler);
    /** Bind loopback only (GSI / RGB callbacks). */
    bool listen(quint16 port);
    /** Bind all IPv4 interfaces (LAN patch seed). */
    bool listenAny(quint16 port);
    bool listenEphemeral();
    quint16 serverPort() const;
    bool isListening() const;
    void close();

private:
    void onNewConnection();
    void onReadyRead();
    bool tryConsume(QTcpSocket *sock);
    void reply(QTcpSocket *sock, const LocalHttpRequest &req, const LocalHttpResponse &res);

    QTcpServer m_server;
    Handler m_handler;
    QHash<QTcpSocket *, QByteArray> m_buf;
};

#endif
