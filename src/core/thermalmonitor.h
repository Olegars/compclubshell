#ifndef THERMALMONITOR_H
#define THERMALMONITOR_H

namespace ThermalMonitor {

/**
 * Считывает температуру CPU (°C).
 * Порядок: LibreHardwareMonitor → OpenHardwareMonitor → Win32 Perf thermal zones → ACPI.
 * При ошибке возвращает отрицательное значение.
 */
double readCpuCelsius();

} // namespace ThermalMonitor

#endif // THERMALMONITOR_H
