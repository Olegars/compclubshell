#ifndef GPUPOWERLIMITER_H
#define GPUPOWERLIMITER_H

#include <QObject>
#include <QTimer>

class NetworkManager;

/**
 * Зелёный эко-режим: при idle+warmup (ПК включён, гостя нет) — лимит GPU через nvidia-smi.
 */
class GpuPowerLimiter : public QObject
{
    Q_OBJECT
public:
    explicit GpuPowerLimiter(NetworkManager *net, QObject *parent = nullptr);

private slots:
    void tick();

private:
    void loadConfig();
    bool nvidiaPresent() const;
    bool applyLimit(int watts);
    bool clearLimit();
    void updateMode(bool idleWarmup);

    NetworkManager *m_net = nullptr;
    QTimer m_tick;

    bool m_enabled = true;
    int m_idleLimitW = 45;
    int m_defaultLimitW = 0;
    QString m_nvidiaSmi;
    bool m_limited = false;
    bool m_lastIdleWarmup = false;
    int m_appliedW = 0;
};

#endif // GPUPOWERLIMITER_H
