#ifndef THERMALMONITOR_H
#define THERMALMONITOR_H

#include <QString>

namespace ThermalMonitor {

/**
 * Считывает температуру CPU (°C).
 * Порядок: LibreHardwareMonitor → OpenHardwareMonitor → Win32 Perf thermal zones → ACPI.
 * При ошибке возвращает отрицательное значение.
 */
double readCpuCelsius();

/**
 * Температура SSD кеша (°C) по букве тома (D).
 * Порядок: IOCTL StorageTemperature → MSFT_PhysicalDisk → LHM/OHM (NVMe/SSD).
 * При ошибке возвращает отрицательное значение.
 */
double readSsdCelsius(const QString &volumeLetter = QString());

} // namespace ThermalMonitor

#endif // THERMALMONITOR_H
