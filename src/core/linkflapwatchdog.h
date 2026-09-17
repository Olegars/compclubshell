#ifndef LINKFLAPWATCHDOG_H
#define LINKFLAPWATCHDOG_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <QJsonObject>

class NetworkManager;

/**
 * Авто-детекция деградации кабеля: падение линка с ≥1 Гбит до ≤100 Мбит
 * (и рост InErrors). Событие уходит в heartbeat; сервер считает за смену
 * и создаёт инцидент «Заменить патч-корд на ПК-ХХ».
 */
class LinkFlapWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit LinkFlapWatchdog(NetworkManager *net, QObject *parent = nullptr);

    /** Текущий линк Мбит/с для heartbeat (−1 = ещё не мерили). */
    int lastLinkMbps() const { return m_lastMbps; }
    int pendingFlapCount() const { return m_pendingFlaps; }
    void consumeFlaps(int n);

private slots:
    void tick();

private:
    void loadConfig();
    bool isGigabit(int mbps) const;
    bool isDegraded(int mbps) const;

    NetworkManager *m_net = nullptr;
    QTimer m_tick;
    bool m_enabled = true;
    int m_pollMs = 15000;
    int m_lastMbps = -1;
    bool m_hadGigabit = false;
    quint64 m_lastInErrors = 0;
    int m_pendingFlaps = 0;
    qint64 m_lastFlapMs = 0;
    int m_cooldownMs = 45000;
};

#endif
