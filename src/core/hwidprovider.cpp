#include "hwidprovider.h"

#include <QSysInfo>
#include <QSettings>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

QString HwidProvider::machineHwid()
{
    const QByteArray machineId = QSysInfo::machineUniqueId();
    if (!machineId.isEmpty()) {
        return QString::fromLatin1(machineId);
    }

#ifdef Q_OS_WIN
    QSettings settings(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography"),
        QSettings::NativeFormat);
    const QString machineGuid = settings.value(QStringLiteral("MachineGuid")).toString().trimmed();
    if (!machineGuid.isEmpty()) {
        return machineGuid;
    }
#endif

    qWarning() << "[HWID] Не удалось получить идентификатор машины, используется fallback.";
    return QStringLiteral("UNKNOWN_HWID_FALLBACK");
}
