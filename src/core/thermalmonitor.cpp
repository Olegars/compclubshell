#include "thermalmonitor.h"

#include <QtGlobal>
#include <QDebug>
#include <QString>

#ifdef Q_OS_WIN
#  include <windows.h>
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

#else

double readCpuCelsius()
{
    return -1.0;
}

#endif

} // namespace ThermalMonitor
