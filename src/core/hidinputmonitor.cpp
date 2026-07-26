#include "hidinputmonitor.h"
#include "networkmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QList>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#endif

QString HidInputMonitor::DeviceFingerprint::stableKey() const
{
    return kind + QLatin1Char('|') + vid + QLatin1Char('|') + pid + QLatin1Char('|')
        + serial + QLatin1Char('|') + instanceId;
}

QJsonObject HidInputMonitor::DeviceFingerprint::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("vid"), vid);
    o.insert(QStringLiteral("pid"), pid);
    o.insert(QStringLiteral("serial"), serial);
    o.insert(QStringLiteral("instance_id"), instanceId);
    o.insert(QStringLiteral("description"), description);
    return o;
}

HidInputMonitor::HidInputMonitor(NetworkManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
{
    m_timer.setInterval(4000);
    connect(&m_timer, &QTimer::timeout, this, &HidInputMonitor::pollDevices);
}

void HidInputMonitor::captureAndBind(int computerId, int bookingId)
{
    if (computerId <= 0 || !m_network || m_network->serverUrl().isEmpty())
        return;

    m_computerId = computerId;
    m_bookingId = bookingId;

    const QJsonObject fingerprint = enumerateFingerprint();
    m_baselineSig = fingerprintSignature(fingerprint);

    QJsonObject body;
    body.insert(QStringLiteral("computer_id"), computerId);
    if (bookingId > 0)
        body.insert(QStringLiteral("booking_id"), bookingId);
    body.insert(QStringLiteral("fingerprint"), fingerprint);

    QUrl url(m_network->serverUrl() + QStringLiteral("/api/shell/hid/snapshot"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QNetworkReply *reply = m_network->networkAccessManager()->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[HID] snapshot failed:" << reply->errorString();
            return;
        }
        qDebug() << "[HID] snapshot bound OK";
    });
}

void HidInputMonitor::startWatch(int computerId, int bookingId)
{
    if (computerId <= 0)
        return;

    m_computerId = computerId;
    m_bookingId = bookingId;
    m_changeBurst = 0;
    m_burstWindowStartMs = QDateTime::currentMSecsSinceEpoch();

    if (m_baselineSig.isEmpty()) {
        const QJsonObject fingerprint = enumerateFingerprint();
        m_baselineSig = fingerprintSignature(fingerprint);
    }

    if (!m_watching) {
        m_watching = true;
        emit watchingChanged();
    }
    m_timer.start();
    qDebug() << "[HID] watch started computer" << computerId;
}

void HidInputMonitor::stopWatch()
{
    m_timer.stop();
    m_changeBurst = 0;
    if (m_watching) {
        m_watching = false;
        emit watchingChanged();
    }
}

QJsonObject HidInputMonitor::enumerateFingerprint() const
{
    QJsonArray mice;
    QJsonArray keyboards;

#ifdef Q_OS_WIN
    auto collectClass = [](const GUID &guid, const QString &kind, QList<DeviceFingerprint> *out) {
        HDEVINFO hDevInfo = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT);
        if (hDevInfo == INVALID_HANDLE_VALUE)
            return;

        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &deviceInfo); ++i) {
            WCHAR idBuf[512] = {};
            if (!SetupDiGetDeviceInstanceIdW(hDevInfo, &deviceInfo, idBuf, 512, nullptr))
                continue;

            const QString instanceId = QString::fromWCharArray(idBuf);
            const QString upper = instanceId.toUpper();
            if (upper.contains(QStringLiteral("ROOT\\"))
                || upper.contains(QStringLiteral("SWD\\"))
                || upper.contains(QStringLiteral("HID\\VID_0000"))) {
                continue;
            }

            WCHAR descBuf[256] = {};
            QString description;
            if (SetupDiGetDeviceRegistryPropertyW(
                    hDevInfo, &deviceInfo, SPDRP_DEVICEDESC, nullptr,
                    reinterpret_cast<PBYTE>(descBuf), sizeof(descBuf), nullptr)) {
                description = QString::fromWCharArray(descBuf);
            }

            QString vid;
            QString pid;
            const int v = upper.indexOf(QStringLiteral("VID_"));
            const int p = upper.indexOf(QStringLiteral("PID_"));
            if (v >= 0)
                vid = upper.mid(v + 4, 4);
            if (p >= 0)
                pid = upper.mid(p + 4, 4);

            QString serial;
            const int slash = instanceId.lastIndexOf(QLatin1Char('\\'));
            if (slash >= 0) {
                const QString tail = instanceId.mid(slash + 1);
                if (!tail.contains(QLatin1Char('&')) || tail.length() > 8)
                    serial = tail;
            }

            DeviceFingerprint fp;
            fp.kind = kind;
            fp.vid = vid;
            fp.pid = pid;
            fp.serial = serial;
            fp.instanceId = instanceId;
            fp.description = description;
            out->append(fp);
        }

        SetupDiDestroyDeviceInfoList(hDevInfo);
    };

    QList<DeviceFingerprint> devices;
    collectClass(GUID_DEVCLASS_MOUSE, QStringLiteral("mouse"), &devices);
    collectClass(GUID_DEVCLASS_KEYBOARD, QStringLiteral("keyboard"), &devices);

    for (const DeviceFingerprint &d : devices) {
        if (d.kind == QLatin1String("mouse"))
            mice.append(d.toJson());
        else
            keyboards.append(d.toJson());
    }
#else
    Q_UNUSED(mice);
    Q_UNUSED(keyboards);
#endif

    QJsonObject root;
    root.insert(QStringLiteral("mice"), mice);
    root.insert(QStringLiteral("keyboards"), keyboards);
    root.insert(QStringLiteral("captured_at"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return root;
}

QString HidInputMonitor::fingerprintSignature(const QJsonObject &fp) const
{
    QStringList keys;
    const auto collect = [&keys](const QJsonArray &arr) {
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            keys << (o.value(QStringLiteral("kind")).toString()
                     + QLatin1Char('|')
                     + o.value(QStringLiteral("vid")).toString()
                     + QLatin1Char('|')
                     + o.value(QStringLiteral("pid")).toString()
                     + QLatin1Char('|')
                     + o.value(QStringLiteral("serial")).toString()
                     + QLatin1Char('|')
                     + o.value(QStringLiteral("instance_id")).toString());
        }
    };
    collect(fp.value(QStringLiteral("mice")).toArray());
    collect(fp.value(QStringLiteral("keyboards")).toArray());
    keys.sort();
    return keys.join(QLatin1Char(';'));
}

void HidInputMonitor::pollDevices()
{
    if (m_computerId <= 0)
        return;

    const QJsonObject current = enumerateFingerprint();
    const QString sig = fingerprintSignature(current);
    if (sig == m_baselineSig)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_burstWindowStartMs > 60000) {
        m_burstWindowStartMs = now;
        m_changeBurst = 0;
    }
    ++m_changeBurst;

    const QJsonArray mice = current.value(QStringLiteral("mice")).toArray();
    const QJsonArray keys = current.value(QStringLiteral("keyboards")).toArray();
    const bool likelyDisconnect = mice.isEmpty() || keys.isEmpty();

    QJsonObject payload;
    payload.insert(QStringLiteral("previous_signature"), m_baselineSig);
    payload.insert(QStringLiteral("current"), current);
    payload.insert(QStringLiteral("burst_count"), m_changeBurst);

    QString type = QStringLiteral("device_changed");
    if (likelyDisconnect)
        type = QStringLiteral("disconnected");
    if (m_changeBurst >= 4)
        type = QStringLiteral("unstable");

    m_baselineSig = sig;

    qWarning() << "[HID] anomaly" << type << "burst" << m_changeBurst;
    reportAlert(type, payload);
    emit alertReported(type);
}

void HidInputMonitor::reportAlert(const QString &type, const QJsonObject &payload)
{
    if (!m_network || m_network->serverUrl().isEmpty() || m_computerId <= 0)
        return;

    QJsonObject body;
    body.insert(QStringLiteral("computer_id"), m_computerId);
    if (m_bookingId > 0)
        body.insert(QStringLiteral("booking_id"), m_bookingId);
    body.insert(QStringLiteral("type"), type);
    body.insert(QStringLiteral("severity"),
                type == QLatin1String("unstable") ? QStringLiteral("critical")
                                                  : QStringLiteral("warn"));
    body.insert(QStringLiteral("payload"), payload);

    QUrl url(m_network->serverUrl() + QStringLiteral("/api/shell/hid/alert"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QNetworkReply *reply = m_network->networkAccessManager()->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, type]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            qWarning() << "[HID] alert POST failed:" << type << reply->errorString();
        else
            qDebug() << "[HID] alert posted:" << type;
    });
}
