#pragma once

#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

class NetworkManager;

/**
 * Windows HID mouse/keyboard fingerprint + change watch.
 * Bound to computer_id on the backend (computer_input_devices / computer_input_alerts).
 */
class HidInputMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool watching READ watching NOTIFY watchingChanged)

public:
    explicit HidInputMonitor(NetworkManager *network, QObject *parent = nullptr);

    bool watching() const { return m_watching; }

    /** Enumerate current mouse+keyboard set and POST /api/shell/hid/snapshot */
    Q_INVOKABLE void captureAndBind(int computerId, int bookingId = 0);

    /** Start polling for device set changes (session lifetime). */
    Q_INVOKABLE void startWatch(int computerId, int bookingId = 0);

    Q_INVOKABLE void stopWatch();

signals:
    void watchingChanged();
    void alertReported(const QString &type);

private:
    struct DeviceFingerprint {
        QString kind; // mouse | keyboard
        QString vid;
        QString pid;
        QString serial;
        QString instanceId;
        QString description;

        QString stableKey() const;
        QJsonObject toJson() const;
    };

    QJsonObject enumerateFingerprint() const;
    QString fingerprintSignature(const QJsonObject &fp) const;
    void pollDevices();
    void reportAlert(const QString &type, const QJsonObject &payload);

    NetworkManager *m_network = nullptr;
    QTimer m_timer;
    bool m_watching = false;
    int m_computerId = 0;
    int m_bookingId = 0;
    QString m_baselineSig;
    int m_changeBurst = 0;
    qint64 m_burstWindowStartMs = 0;
};
