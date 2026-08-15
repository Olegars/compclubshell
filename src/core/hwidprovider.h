#ifndef HWIDPROVIDER_H
#define HWIDPROVIDER_H

#include <QString>

/**
 * Идентификатор места для гибридного клона: UUID железа, не MachineGuid образа.
 * Порядок: SMBIOS UUID → MAC onboard LAN → MachineGuid (warning).
 */
class HwidProvider
{
public:
    static QString machineHwid();
    /** MAC того же onboard NIC, что уходит в power heartbeat. */
    static QString onboardMac();
    /** Старый MachineGuid образа — только для миграции привязки после смены алгоритма HWID. */
    static QString legacyMachineGuid();

private:
    static QString smbiosUuid();
    static QString machineGuid();
    static bool uuidUsable(const QString &uuid);
};

#endif // HWIDPROVIDER_H
