#include "thermalmonitor.h"

#include <QtGlobal>
#include <QChar>
#include <QDebug>
#include <QString>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <winioctl.h>
#  include <wbemidl.h>
#  include <oleauto.h>
#endif

namespace ThermalMonitor {

#ifdef Q_OS_WIN

namespace {

bool ensureCom()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
        return true;
    return hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
}

double variantToDouble(VARIANT vt)
{
    if (vt.vt == VT_R8)
        return vt.dblVal;
    if (vt.vt == VT_R4)
        return static_cast<double>(vt.fltVal);
    if (vt.vt == VT_I4)
        return static_cast<double>(vt.lVal);
    if (vt.vt == VT_UI4)
        return static_cast<double>(vt.ulVal);
    if (vt.vt == VT_I2)
        return static_cast<double>(vt.iVal);
    if (vt.vt == VT_UI2)
        return static_cast<double>(vt.uiVal);
    if (vt.vt == VT_INT)
        return static_cast<double>(vt.intVal);
    if (vt.vt == VT_UINT)
        return static_cast<double>(vt.uintVal);
    if (vt.vt == VT_BSTR && vt.bstrVal) {
        bool ok = false;
        const double v = QString::fromWCharArray(vt.bstrVal).toDouble(&ok);
        return ok ? v : -1.0;
    }

    VARIANT converted;
    VariantInit(&converted);
    if (SUCCEEDED(VariantChangeType(&converted, &vt, 0, VT_R8))) {
        const double v = converted.dblVal;
        VariantClear(&converted);
        return v;
    }
    VariantClear(&converted);
    return -1.0;
}

bool looksLikeCpuSensor(const QString &name)
{
    const QString n = name.toLower();
    if (n.contains(QStringLiteral("gpu"))
        || n.contains(QStringLiteral("hdd"))
        || n.contains(QStringLiteral("ssd"))
        || n.contains(QStringLiteral("nvme"))
        || n.contains(QStringLiteral("drive"))
        || n.contains(QStringLiteral("motherboard"))
        || n.contains(QStringLiteral("chipset"))
        || n.contains(QStringLiteral("ambient"))
        || n.contains(QStringLiteral("vrm")))
        return false;

    return n.contains(QStringLiteral("cpu"))
        || n.contains(QStringLiteral("package"))
        || n.contains(QStringLiteral("tctl"))
        || n.contains(QStringLiteral("tdie"))
        || n.contains(QStringLiteral("core"))
        || n.contains(QStringLiteral("processor"));
}

bool looksLikeSsdSensor(const QString &name)
{
    const QString n = name.toLower();
    if (n.contains(QStringLiteral("cpu"))
        || n.contains(QStringLiteral("gpu"))
        || n.contains(QStringLiteral("package"))
        || n.contains(QStringLiteral("motherboard"))
        || n.contains(QStringLiteral("chipset"))
        || n.contains(QStringLiteral("ambient"))
        || n.contains(QStringLiteral("vrm")))
        return false;

    return n.contains(QStringLiteral("ssd"))
        || n.contains(QStringLiteral("nvme"))
        || n.contains(QStringLiteral("hdd"))
        || n.contains(QStringLiteral("nand"))
        || n.contains(QStringLiteral("drive"));
}

bool plausibleDriveTemp(double v)
{
    return v >= 5.0 && v < 125.0;
}

IWbemServices *connectNamespace(const wchar_t *ns)
{
    IWbemLocator *locator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, reinterpret_cast<void **>(&locator));
    if (FAILED(hr) || !locator)
        return nullptr;

    BSTR namespacePath = SysAllocString(ns);
    if (!namespacePath) {
        locator->Release();
        return nullptr;
    }

    IWbemServices *services = nullptr;
    hr = locator->ConnectServer(namespacePath, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(namespacePath);
    locator->Release();
    if (FAILED(hr) || !services)
        return nullptr;

    hr = CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        services->Release();
        return nullptr;
    }
    return services;
}

/**
 * Scan Hardware Monitor style sensors (LibreHardwareMonitor / OpenHardwareMonitor).
 * Prefer CPU-named sensors; fallback to max plausible temperature.
 */
double readHardwareMonitorNamespace(const wchar_t *ns, const char *label)
{
    IWbemServices *svc = connectNamespace(ns);
    if (!svc)
        return -1.0;

    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT Name, Value FROM Sensor WHERE SensorType = 'Temperature'");
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        svc->Release();
        return -1.0;
    }

    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        svc->Release();
        return -1.0;
    }

    double bestCpu = -1.0;
    double bestAny = -1.0;
    IWbemClassObject *obj = nullptr;
    ULONG returned = 0;
    while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
        VARIANT vtName;
        VARIANT vtVal;
        VariantInit(&vtName);
        VariantInit(&vtVal);
        QString name;
        if (SUCCEEDED(obj->Get(L"Name", 0, &vtName, nullptr, nullptr))
            && vtName.vt == VT_BSTR && vtName.bstrVal)
            name = QString::fromWCharArray(vtName.bstrVal);

        double v = -1.0;
        if (SUCCEEDED(obj->Get(L"Value", 0, &vtVal, nullptr, nullptr)))
            v = variantToDouble(vtVal);

        VariantClear(&vtName);
        VariantClear(&vtVal);
        obj->Release();
        obj = nullptr;

        if (!(v > 5.0 && v < 125.0))
            continue;

        if (v > bestAny)
            bestAny = v;
        if (looksLikeCpuSensor(name) && v > bestCpu)
            bestCpu = v;
    }
    enumerator->Release();
    svc->Release();

    const double result = bestCpu > 0.0 ? bestCpu : bestAny;
    if (result > 0.0)
        qWarning() << "[THERMAL]" << label << "->" << result << "C";
    return result;
}

double tenthsKelvinToCelsius(double tenthsK)
{
    if (tenthsK < 2000.0) // nonsense / empty
        return -1.0;
    return (tenthsK / 10.0) - 273.15;
}

/**
 * Scan Hardware Monitor style sensors (LibreHardwareMonitor / OpenHardwareMonitor).
 * Prefer SSD/NVMe-named sensors.
 */
double readHardwareMonitorSsd(const wchar_t *ns, const char *label)
{
    IWbemServices *svc = connectNamespace(ns);
    if (!svc)
        return -1.0;

    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT Name, Value FROM Sensor WHERE SensorType = 'Temperature'");
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        svc->Release();
        return -1.0;
    }

    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        svc->Release();
        return -1.0;
    }

    double best = -1.0;
    IWbemClassObject *obj = nullptr;
    ULONG returned = 0;
    while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
        VARIANT vtName;
        VARIANT vtVal;
        VariantInit(&vtName);
        VariantInit(&vtVal);
        QString name;
        if (SUCCEEDED(obj->Get(L"Name", 0, &vtName, nullptr, nullptr))
            && vtName.vt == VT_BSTR && vtName.bstrVal)
            name = QString::fromWCharArray(vtName.bstrVal);

        double v = -1.0;
        if (SUCCEEDED(obj->Get(L"Value", 0, &vtVal, nullptr, nullptr)))
            v = variantToDouble(vtVal);

        VariantClear(&vtName);
        VariantClear(&vtVal);
        obj->Release();
        obj = nullptr;

        if (!plausibleDriveTemp(v) || !looksLikeSsdSensor(name))
            continue;
        if (v > best)
            best = v;
    }
    enumerator->Release();
    svc->Release();

    if (best > 0.0)
        qWarning() << "[THERMAL]" << label << "SSD ->" << best << "C";
    return best;
}

#ifndef StorageTemperatureValueNotReported
#  define StorageTemperatureValueNotReported ((SHORT)0x8000)
#endif

struct ReactorStorageTemperatureInfo {
    WORD Index;
    SHORT Temperature;
    SHORT OverThreshold;
    SHORT UnderThreshold;
    BOOLEAN OverThresholdChangable;
    BOOLEAN UnderThresholdChangable;
    BOOLEAN EventGenerated;
    BYTE Reserved0;
    DWORD Reserved1;
};

struct ReactorStorageTemperatureDescriptor {
    DWORD Version;
    DWORD Size;
    SHORT CriticalTemperature;
    SHORT WarningTemperature;
    WORD InfoCount;
    BYTE Reserved0[2];
    DWORD Reserved1[2];
    ReactorStorageTemperatureInfo TemperatureInfo[8];
};

double temperatureFromIoctlHandle(HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return -1.0;

    STORAGE_PROPERTY_QUERY query;
    ZeroMemory(&query, sizeof(query));
    query.PropertyId = static_cast<STORAGE_PROPERTY_ID>(23); // StorageDeviceTemperatureProperty
    query.QueryType = PropertyStandardQuery;

    ReactorStorageTemperatureDescriptor desc;
    ZeroMemory(&desc, sizeof(desc));
    DWORD bytes = 0;
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query),
                         &desc, sizeof(desc),
                         &bytes, nullptr))
        return -1.0;

    const WORD count = desc.InfoCount > 8 ? 8 : desc.InfoCount;
    double best = -1.0;
    for (WORD i = 0; i < count; ++i) {
        const SHORT raw = desc.TemperatureInfo[i].Temperature;
        if (raw == StorageTemperatureValueNotReported)
            continue;
        const double c = static_cast<double>(raw);
        if (!plausibleDriveTemp(c))
            continue;
        if (c > best)
            best = c;
    }
    return best;
}

HANDLE openVolumeHandle(QChar letter)
{
    const QString path = QStringLiteral("\\\\.\\") + letter.toUpper() + QLatin1Char(':');
    return CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                       0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

double readSsdViaIoctl(QChar letter)
{
    HANDLE vol = openVolumeHandle(letter);
    if (vol == INVALID_HANDLE_VALUE)
        return -1.0;

    double c = temperatureFromIoctlHandle(vol);
    if (c < 0.0) {
        STORAGE_DEVICE_NUMBER sdn;
        ZeroMemory(&sdn, sizeof(sdn));
        DWORD bytes = 0;
        if (DeviceIoControl(vol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            nullptr, 0, &sdn, sizeof(sdn), &bytes, nullptr)) {
            const QString phys = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(sdn.DeviceNumber);
            HANDLE disk = CreateFileW(reinterpret_cast<LPCWSTR>(phys.utf16()),
                                      0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
            if (disk != INVALID_HANDLE_VALUE) {
                c = temperatureFromIoctlHandle(disk);
                CloseHandle(disk);
            }
        }
    }
    CloseHandle(vol);
    if (c > 0.0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            qWarning() << "[THERMAL] IOCTL SSD" << letter << ":" << "->" << c << "C";
        }
    }
    return c;
}

double readSsdViaMsftPhysicalDisk(QChar letter)
{
    IWbemServices *svc = connectNamespace(L"ROOT\\Microsoft\\Windows\\Storage");
    if (!svc)
        return -1.0;

    const QString wql = QStringLiteral(
        "SELECT DiskNumber FROM MSFT_Partition WHERE DriveLetter = '%1'")
                            .arg(letter.toUpper());
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(reinterpret_cast<const wchar_t *>(wql.utf16()));
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        svc->Release();
        return -1.0;
    }

    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        svc->Release();
        return -1.0;
    }

    QString diskId;
    IWbemClassObject *obj = nullptr;
    ULONG returned = 0;
    if (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"DiskNumber", 0, &vt, nullptr, nullptr))) {
            const double n = variantToDouble(vt);
            if (n >= 0.0)
                diskId = QString::number(static_cast<int>(n));
        }
        VariantClear(&vt);
        obj->Release();
    }
    enumerator->Release();

    if (diskId.isEmpty()) {
        svc->Release();
        return -1.0;
    }

    const QString diskWql = QStringLiteral(
        "SELECT Temperature FROM MSFT_PhysicalDisk WHERE DeviceId = '%1'")
                                .arg(diskId);
    language = SysAllocString(L"WQL");
    query = SysAllocString(reinterpret_cast<const wchar_t *>(diskWql.utf16()));
    enumerator = nullptr;
    hr = svc->ExecQuery(language, query,
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        svc->Release();
        return -1.0;
    }

    double c = -1.0;
    obj = nullptr;
    returned = 0;
    if (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"Temperature", 0, &vt, nullptr, nullptr)))
            c = variantToDouble(vt);
        VariantClear(&vt);
        obj->Release();
    }
    enumerator->Release();
    svc->Release();

    if (!plausibleDriveTemp(c))
        return -1.0;
    qWarning() << "[THERMAL] MSFT_PhysicalDisk SSD" << letter << ":" << "->" << c << "C";
    return c;
}

double readAcpiThermalZones()
{
    IWbemServices *svc = connectNamespace(L"ROOT\\WMI");
    if (!svc)
        return -1.0;

    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        svc->Release();
        return -1.0;
    }

    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        svc->Release();
        return -1.0;
    }

    double maxC = -1.0;
    IWbemClassObject *obj = nullptr;
    ULONG returned = 0;
    while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &vt, nullptr, nullptr))) {
            const double c = tenthsKelvinToCelsius(variantToDouble(vt));
            if (c > maxC)
                maxC = c;
        }
        VariantClear(&vt);
        obj->Release();
        obj = nullptr;
    }
    enumerator->Release();
    svc->Release();

    if (maxC > 0.0 && maxC < 125.0) {
        qWarning() << "[THERMAL] ACPI zone ->" << maxC << "C";
        return maxC;
    }
    return -1.0;
}

double readPerfThermalZones()
{
    IWbemServices *svc = connectNamespace(L"ROOT\\CIMV2");
    if (!svc)
        return -1.0;

    // Win10/11: HighPrecisionTemperature (tenths of Kelvin). Older: Temperature.
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT HighPrecisionTemperature, Temperature "
        L"FROM Win32_PerfFormattedData_Counters_ThermalZoneInformation");
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        svc->Release();
        return -1.0;
    }

    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator) {
        svc->Release();
        return -1.0;
    }

    double maxC = -1.0;
    IWbemClassObject *obj = nullptr;
    ULONG returned = 0;
    while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK) {
        VARIANT vtHi;
        VARIANT vtLo;
        VariantInit(&vtHi);
        VariantInit(&vtLo);
        double raw = -1.0;
        if (SUCCEEDED(obj->Get(L"HighPrecisionTemperature", 0, &vtHi, nullptr, nullptr)))
            raw = variantToDouble(vtHi);
        if (raw < 0.0
            && SUCCEEDED(obj->Get(L"Temperature", 0, &vtLo, nullptr, nullptr)))
            raw = variantToDouble(vtLo);

        const double c = tenthsKelvinToCelsius(raw);
        if (c > maxC)
            maxC = c;

        VariantClear(&vtHi);
        VariantClear(&vtLo);
        obj->Release();
        obj = nullptr;
    }
    enumerator->Release();
    svc->Release();

    if (maxC > 0.0 && maxC < 125.0) {
        qWarning() << "[THERMAL] Perf thermal zone ->" << maxC << "C";
        return maxC;
    }
    return -1.0;
}

} // namespace

double readCpuCelsius()
{
    if (!ensureCom()) {
        qWarning() << "[THERMAL] COM init failed";
        return -1.0;
    }

    double c = readHardwareMonitorNamespace(L"ROOT\\LibreHardwareMonitor", "LHM");
    if (c > 0.0 && c < 150.0)
        return c;

    c = readHardwareMonitorNamespace(L"ROOT\\OpenHardwareMonitor", "OHM");
    if (c > 0.0 && c < 150.0)
        return c;

    c = readPerfThermalZones();
    if (c > 0.0 && c < 150.0)
        return c;

    c = readAcpiThermalZones();
    if (c > 0.0 && c < 150.0)
        return c;

    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        qWarning() << "[THERMAL] CPU temperature unavailable (LHM/OHM/Perf/ACPI) — TEMP —";
    }
    return -1.0;
}

double readSsdCelsius(const QString &volumeLetter)
{
    if (!ensureCom()) {
        qWarning() << "[THERMAL] COM init failed (SSD)";
        return -1.0;
    }

    QChar letter = QLatin1Char('D');
    const QString trimmed = volumeLetter.trimmed();
    if (!trimmed.isEmpty()) {
        const QChar ch = trimmed.at(0).toUpper();
        if (ch >= QLatin1Char('C') && ch <= QLatin1Char('Z'))
            letter = ch;
    }

    double c = readSsdViaIoctl(letter);
    if (c > 0.0)
        return c;

    c = readSsdViaMsftPhysicalDisk(letter);
    if (c > 0.0)
        return c;

    c = readHardwareMonitorSsd(L"ROOT\\LibreHardwareMonitor", "LHM");
    if (c > 0.0)
        return c;

    c = readHardwareMonitorSsd(L"ROOT\\OpenHardwareMonitor", "OHM");
    if (c > 0.0)
        return c;

    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        qWarning() << "[THERMAL] SSD temperature unavailable for" << letter
                   << "(IOCTL/MSFT/LHM/OHM) — SSD —";
    }
    return -1.0;
}

#else

double readCpuCelsius()
{
    return -1.0;
}

double readSsdCelsius(const QString &)
{
    return -1.0;
}

#endif

} // namespace ThermalMonitor
