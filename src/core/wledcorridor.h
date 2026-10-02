#ifndef WLEDCORRIDOR_H
#define WLEDCORRIDOR_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QTimer;

// Corridor GLEDOPTO (ESP32 WLED, 4 segments). Cloud queues cues;
// this painter POSTs /json/state on the LAN and restores idle.
class WledCorridor : public QObject
{
    Q_OBJECT
public:
    explicit WledCorridor(QNetworkAccessManager *nam, QObject *parent = nullptr);

    void start(const QString &serverUrl, int computerId);
    void stop();

private:
    void poll();
    void playCue(const QJsonObject &cue);
    void postWled(const QString &host, int port, const QJsonObject &state,
                  const std::function<void(bool, const QString &)> &done);
    void ack(int cueId, bool ok, const QString &error);
    void armRestore(int controllerId, const QString &host, int port,
                    const QJsonObject &idle, int durationMs);
    void cancelRestore(int controllerId);

    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QString m_serverUrl;
    int m_computerId = 0;
    bool m_pollInFlight = false;
    QHash<int, QTimer *> m_restore;
};

#endif
