#ifndef STATIONHEALTH_H
#define STATIONHEALTH_H

#include <QString>
#include <QVector>

namespace StationHealth {

struct SsdHealth {
    int wearPct = -1;
    qint64 readErrors = -1;
    qint64 writeErrors = -1;
    QString status;
    QString media; // nvme|ssd|hdd|scm|unknown
};

struct GameBuild {
    QString platform;
    QString appId;
    QString buildId;
    QString name;
};

struct GameInventory {
    QVector<GameBuild> games;
    QString hash;
    int steamCount = 0;
    int epicCount = 0;
};

struct NicInfo {
    int linkMbps = -1;          // 0 = down, -1 = unknown
    bool mediaConnected = false;
    quint64 receiveLinkBps = 0;
    quint64 transmitLinkBps = 0;
    quint64 inErrors = 0;
    quint64 outErrors = 0;
    quint64 inDiscards = 0;
    QString mac;
};

/** Реальная скорость линка onboard NIC (Мбит/с). 0 = нет линка, -1 = не удалось. */
int nicLinkMbps(const QString &macHint);

/** Полный снимок линка (скорость + ошибки) для детекции деградации кабеля. */
NicInfo nicInfo(const QString &macHint);

/** SMART/wear кэширующего тома (D:). status: healthy|warning|unhealthy|unknown */
SsdHealth readSsdHealth(const QString &volumeLetter);

/** Steam appmanifest + Epic .item на локальном SSD. */
GameInventory scanGames(const QString &steamPath, const QString &epicPath);

} // namespace StationHealth

#endif // STATIONHEALTH_H
