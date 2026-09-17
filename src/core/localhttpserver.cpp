#include "localhttpserver.h"

#include <QFile>
#include <QUrl>
#include <algorithm>

LocalHttpServer::LocalHttpServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &LocalHttpServer::onNewConnection);
}

void LocalHttpServer::setHandler(Handler handler)
{
    m_handler = std::move(handler);
}

bool LocalHttpServer::listen(quint16 port)
{
    if (m_server.isListening() && m_server.serverPort() == port)
        return true;
    close();
    return m_server.listen(QHostAddress::LocalHost, port);
}

bool LocalHttpServer::listenAny(quint16 port)
{
    if (m_server.isListening() && m_server.serverPort() == port)
        return true;
    close();
    return m_server.listen(QHostAddress::AnyIPv4, port);
}

bool LocalHttpServer::listenEphemeral()
{
    close();
    return m_server.listen(QHostAddress::LocalHost, 0);
}

quint16 LocalHttpServer::serverPort() const
{
    return m_server.serverPort();
}

bool LocalHttpServer::isListening() const
{
    return m_server.isListening();
}

void LocalHttpServer::close()
{
    m_server.close();
    for (auto it = m_buf.begin(); it != m_buf.end(); ++it) {
        if (it.key())
            it.key()->deleteLater();
    }
    m_buf.clear();
}

void LocalHttpServer::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *sock = m_server.nextPendingConnection();
        m_buf.insert(sock, QByteArray());
        connect(sock, &QTcpSocket::readyRead, this, &LocalHttpServer::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
            m_buf.remove(sock);
            sock->deleteLater();
        });
    }
}

void LocalHttpServer::onReadyRead()
{
    auto *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock)
        return;
    while (tryConsume(sock)) {
    }
}

bool LocalHttpServer::tryConsume(QTcpSocket *sock)
{
    m_buf[sock].append(sock->readAll());
    QByteArray &buf = m_buf[sock];
    const int hdrEnd = buf.indexOf("\r\n\r\n");
    if (hdrEnd < 0)
        return false;

    const QByteArray headerBlock = buf.left(hdrEnd);
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty()) {
        buf.clear();
        return false;
    }

    const QByteArray first = lines.first().trimmed();
    const int sp1 = first.indexOf(' ');
    const int sp2 = first.indexOf(' ', sp1 + 1);
    LocalHttpRequest req;
    req.method = QString::fromLatin1(first.left(sp1)).toUpper();
    QString rawPath = QString::fromLatin1(first.mid(sp1 + 1, (sp2 > sp1 ? sp2 : first.size()) - sp1 - 1));
    if (rawPath.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || rawPath.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        rawPath = QUrl(rawPath).path();
    }
    const int qpos = rawPath.indexOf(QLatin1Char('?'));
    if (qpos >= 0)
        rawPath = rawPath.left(qpos);
    if (rawPath.isEmpty())
        rawPath = QStringLiteral("/");
    req.path = rawPath;

    int contentLen = 0;
    bool hasLen = false;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString val = QString::fromLatin1(line.mid(colon + 1)).trimmed();
        req.headers.insert(key, val);
        if (key == QLatin1String("content-length")) {
            contentLen = val.toInt();
            hasLen = true;
        }
    }
    if (!hasLen)
        contentLen = 0;
    contentLen = std::clamp(contentLen, 0, 1024 * 1024);

    const int bodyStart = hdrEnd + 4;
    if (buf.size() < bodyStart + contentLen)
        return false;

    req.body = buf.mid(bodyStart, contentLen);
    buf.remove(0, bodyStart + contentLen);

    LocalHttpResponse res;
    if (req.method == QLatin1String("OPTIONS")) {
        res.body.clear();
        res.contentType = "text/plain";
    } else if (m_handler) {
        res = m_handler(req);
    } else {
        res.status = 500;
        res.body = "{\"error\":\"no handler\"}";
    }
    reply(sock, req, res);
    return !buf.isEmpty();
}

void LocalHttpServer::reply(QTcpSocket *sock, const LocalHttpRequest &req,
                            const LocalHttpResponse &res)
{
    Q_UNUSED(req);
    QByteArray out;
    out += "HTTP/1.1 ";
    out += QByteArray::number(res.status);
    out += (res.status == 200 ? " OK" : " ERR");
    out += "\r\nContent-Type: ";
    out += res.contentType;

    if (!res.filePath.isEmpty()) {
        QFile f(res.filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            const QByteArray err = "{\"error\":\"file open failed\"}";
            out = "HTTP/1.1 404 ERR\r\nContent-Type: application/json\r\nContent-Length: ";
            out += QByteArray::number(err.size());
            out += "\r\nConnection: close\r\n\r\n";
            out += err;
            sock->write(out);
            sock->disconnectFromHost();
            return;
        }
        out += "\r\nContent-Length: ";
        out += QByteArray::number(f.size());
        out += "\r\nConnection: close";
        out += "\r\nAccess-Control-Allow-Origin: *";
        out += "\r\n\r\n";
        sock->write(out);
        while (!f.atEnd()) {
            const QByteArray chunk = f.read(256 * 1024);
            if (chunk.isEmpty())
                break;
            sock->write(chunk);
            sock->flush();
        }
        sock->disconnectFromHost();
        return;
    }

    out += "\r\nContent-Length: ";
    out += QByteArray::number(res.body.size());
    out += "\r\nConnection: keep-alive";
    out += "\r\nAccess-Control-Allow-Origin: *";
    out += "\r\nAccess-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS";
    out += "\r\nAccess-Control-Allow-Headers: Content-Type";
    out += "\r\n\r\n";
    out += res.body;
    sock->write(out);
}
