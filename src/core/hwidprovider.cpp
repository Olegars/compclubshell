#include "hwidprovider.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QSettings>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool isAllSameByte(const unsigned char *data, int len, unsigned char value)
{
    for (int i = 0; i < len; ++i) {
        if (data[i] != value)
            return false;
    }
    return true;
}

QString formatSmbiosUuid(const unsigned char *uuid)
{
    // SMBIOS 2.6+: first three fields little-endian.
    return QString::asprintf(
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[3], uuid[2], uuid[1], uuid[0],
        uuid[5], uuid[4],
        uuid[7], uuid[6],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

bool macLooksVirtual(const QString &name, const QString &mac)
{
    const QString n = name.toLower();
    if (n.contains(QLatin1String("virtual"))
        || n.contains(QLatin1String("vmware"))
        || n.contains(QLatin1String("vbox"))
        || n.contains(QLatin1String("hyper-v"))
        || n.contains(QLatin1String("loopback"))
        || n.contains(QLatin1String("tap-"))
        || n.contains(QLatin1String("vpn"))
        || n.contains(QLatin1String("bluetooth"))
        || n.contains(QLatin1String("wi-fi"))
        || n.contains(QLatin1String("wifi"))
        || n.contains(QLatin1String("wlan"))) {
        return true;
    }
    Q_UNUSED(mac);
    return false;
}

} // namespace

bool HwidProvider::uuidUsable(const QString &uuid)
{
    QString compact = uuid;
    compact.remove(QLatin1Char('-'));
    compact.remove(QLatin1Char('{'));
    compact.remove(QLatin1Char('}'));
    compact = compact.trimmed().toLower();
    if (compact.size() != 32)
        return false;
    if (compact == QLatin1String("00000000000000000000000000000000"))
        return false;
    if (compact == QLatin1String("ffffffffffffffffffffffffffffffff"))
        return false;
    return true;
}

QString HwidProvider::smbiosUuid()
{
#ifdef Q_OS_WIN
    const DWORD signature = 'RSMB';
    const DWORD size = GetSystemFirmwareTable(signature, 0, nullptr, 0);
    if (size < 8)
        return {};

    QByteArray buf(int(size), 0);
    if (GetSystemFirmwareTable(signature, 0, buf.data(), size) != size)
        return {};

    // RawSMBIOSData: 4 byte header + DWORD Length + table
    if (buf.size() < 8)
        return {};
    const auto *raw = reinterpret_cast<const unsigned char *>(buf.constData());
    const quint32 tableLen = *reinterpret_cast<const quint32 *>(raw + 4);
    const unsigned char *table = raw + 8;
    const unsigned char *end = raw + buf.size();
    if (table + tableLen < end)
        end = table + tableLen;

    const unsigned char *p = table;
    while (p + 4 <= end) {
        const unsigned char type = p[0];
        const unsigned char length = p[1];
        if (length < 4)
            break;
        if (type == 1 && length >= 0x19 && p + 0x18 < end) {
            const unsigned char *uuid = p + 0x08;
            if (!isAllSameByte(uuid, 16, 0x00) && !isAllSameByte(uuid, 16, 0xFF)) {
                const QString formatted = formatSmbiosUuid(uuid);
                if (uuidUsable(formatted))
                    return formatted;
            }
        }
        const unsigned char *next = p + length;
        while (next + 1 < end && !(next[0] == 0 && next[1] == 0))
            ++next;
        if (next + 1 >= end)
            break;
        p = next + 2;
        if (type == 127)
            break;
    }
#endif
    return {};
}

QString HwidProvider::onboardMac()
{
    QString fallback;

    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        const auto flags = iface.flags();
        if (flags & QNetworkInterface::IsLoopBack)
            continue;
        if (!(flags & QNetworkInterface::IsUp))
            continue;
        const QString mac = iface.hardwareAddress().trimmed().toUpper();
        if (mac.isEmpty() || mac == QLatin1String("00:00:00:00:00:00"))
            continue;
        if (iface.type() == QNetworkInterface::Ethernet
            && !macLooksVirtual(iface.humanReadableName(), mac)) {
            return mac;
        }
        if (fallback.isEmpty() && !macLooksVirtual(iface.humanReadableName(), mac))
            fallback = mac;
        else if (fallback.isEmpty())
            fallback = mac;
    }
    return fallback;
}

QString HwidProvider::legacyMachineGuid()
{
    return machineGuid();
}

QString HwidProvider::machineGuid()
{
#ifdef Q_OS_WIN
    QSettings settings(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography"),
        QSettings::NativeFormat);
    return settings.value(QStringLiteral("MachineGuid")).toString().trimmed();
#else
    return QString::fromLatin1(QSysInfo::machineUniqueId());
#endif
}

QString HwidProvider::machineHwid()
{
    const QString uuid = smbiosUuid();
    if (uuidUsable(uuid)) {
        qWarning() << "[HWID] SMBIOS UUID" << uuid;
        return uuid;
    }

    const QString mac = onboardMac();
    if (!mac.isEmpty()) {
        qWarning() << "[HWID] SMBIOS UUID недоступен, используем MAC onboard" << mac;
        return QStringLiteral("mac:") + mac.toLower();
    }

    const QString guid = machineGuid();
    if (!guid.isEmpty()) {
        qWarning() << "[HWID] WARNING: fallback на MachineGuid образа — клоны будут одним терминалом";
        return guid;
    }

    qWarning() << "[HWID] Не удалось получить идентификатор машины, используется fallback.";
    return QStringLiteral("UNKNOWN_HWID_FALLBACK");
}
