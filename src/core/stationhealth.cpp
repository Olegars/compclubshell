#include "stationhealth.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
#  include <netioapi.h>
#  include <ipifcons.h>
#  include <wbemidl.h>
#  include <oleauto.h>
#endif

namespace StationHealth {

namespace {

QString normalizeMac(QString mac)
{
    mac = mac.trimmed().toUpper();
    mac.remove(QLatin1Char('-'));
    mac.remove(QLatin1Char(':'));
    mac.remove(QLatin1Char('.'));
    return mac;
}

QString acfValue(const QString &text, const QString &key)
{
    const QRegularExpression re(
        QStringLiteral("\"%1\"\\s+\"([^\"]*)\"").arg(QRegularExpression::escape(key)));
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

QString sha1Hex(const QString &s)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}

#ifdef Q_OS_WIN

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
    if (vt.vt == VT_I8)
        return static_cast<double>(vt.llVal);
    if (vt.vt == VT_UI8)
        return static_cast<double>(vt.ullVal);
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

qint64 variantToInt64(VARIANT vt)
{
    const double v = variantToDouble(vt);
    if (v < 0.0)
        return -1;
    return static_cast<qint64>(v);
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

    IWbemServices *svc = nullptr;
    hr = locator->ConnectServer(namespacePath, nullptr, nullptr, nullptr, 0,
                                nullptr, nullptr, &svc);
    SysFreeString(namespacePath);
    locator->Release();
    if (FAILED(hr) || !svc)
        return nullptr;

    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);
    return svc;
}

QString diskIdForLetter(IWbemServices *svc, QChar letter)
{
    const QString wql = QStringLiteral(
        "SELECT DiskNumber FROM MSFT_Partition WHERE DriveLetter = '%1'")
                            .arg(letter.toUpper());
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(reinterpret_cast<const wchar_t *>(wql.utf16()));
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        return {};
    }

    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator)
        return {};

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
    return diskId;
}

bool wmiFirst(IWbemServices *svc, const QString &wql, IWbemClassObject **outObj)
{
    *outObj = nullptr;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(reinterpret_cast<const wchar_t *>(wql.utf16()));
    if (!language || !query) {
        if (language) SysFreeString(language);
        if (query) SysFreeString(query);
        return false;
    }
    IEnumWbemClassObject *enumerator = nullptr;
    HRESULT hr = svc->ExecQuery(language, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr) || !enumerator)
        return false;
    ULONG returned = 0;
    const bool ok = enumerator->Next(WBEM_INFINITE, 1, outObj, &returned) == S_OK && *outObj;
    enumerator->Release();
    return ok;
}

#endif

void appendSteamLibrary(const QString &root, QVector<GameBuild> *out)
{
    if (root.isEmpty() || !out)
        return;
    const QString apps = QDir(root).filePath(QStringLiteral("steamapps"));
    QDir dir(apps);
    if (!dir.exists())
        return;

    const auto files = dir.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files);
    for (const QString &name : files) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        f.close();

        bool flagsOk = false;
        const int flags = acfValue(text, QStringLiteral("StateFlags")).toInt(&flagsOk);
        if (flagsOk && (flags & 4) == 0)
            continue;

        GameBuild g;
        g.platform = QStringLiteral("steam");
        g.appId = acfValue(text, QStringLiteral("appid"));
        g.buildId = acfValue(text, QStringLiteral("buildid"));
        g.name = acfValue(text, QStringLiteral("name")).left(80);
        if (g.appId.isEmpty())
            continue;
        out->append(g);
        if (out->size() >= 200)
            return;
    }
}

QStringList extraSteamLibraries(const QString &steamPath)
{
    QStringList extra;
    const QString vdf = QDir(steamPath).filePath(QStringLiteral("steamapps/libraryfolders.vdf"));
    QFile f(vdf);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return extra;
    const QString text = QString::fromUtf8(f.readAll());
    f.close();

    static const QRegularExpression pathRe(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
    auto it = pathRe.globalMatch(text);
    const QString self = QDir::cleanPath(steamPath).toLower();
    while (it.hasNext()) {
        const QString p = QDir::cleanPath(it.next().captured(1).replace(QLatin1Char('\\'), QLatin1Char('/')));
        if (p.isEmpty() || p.toLower() == self)
            continue;
        extra.append(p);
    }
    return extra;
}

void appendEpicManifests(const QString &dirPath, QVector<GameBuild> *out)
{
    if (dirPath.isEmpty() || !out)
        return;
    QDir dir(dirPath);
    if (!dir.exists())
        return;

    const auto files = dir.entryList({QStringLiteral("*.item")}, QDir::Files);
    for (const QString &name : files) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        if (o.value(QStringLiteral("bIsIncompleteInstall")).toBool())
            continue;
        GameBuild g;
        g.platform = QStringLiteral("epic");
        g.appId = o.value(QStringLiteral("AppName")).toString();
        if (g.appId.isEmpty())
            g.appId = o.value(QStringLiteral("CatalogItemId")).toString();
        g.buildId = o.value(QStringLiteral("AppVersionString")).toString();
        if (g.buildId.isEmpty())
            g.buildId = o.value(QStringLiteral("MainWindowProcess")).toString();
        g.name = o.value(QStringLiteral("DisplayName")).toString().left(80);
        if (g.appId.isEmpty() && g.name.isEmpty())
            continue;
        out->append(g);
        if (out->size() >= 200)
            return;
    }
}

} // namespace

NicInfo nicInfo(const QString &macHint)
{
    NicInfo out;
#ifdef Q_OS_WIN
    const QString want = normalizeMac(macHint);
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
        return out;

    NicInfo matched;
    bool haveMatched = false;
    NicInfo bestUp;
    int bestMbps = -1;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 &row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (row.PhysicalAddressLength < 6)
            continue;

        QString mac;
        for (ULONG b = 0; b < row.PhysicalAddressLength && b < 8; ++b) {
            if (!mac.isEmpty())
                mac += QLatin1Char(':');
            mac += QString::asprintf("%02X", row.PhysicalAddress[b]);
        }
        const QString compact = normalizeMac(mac);
        const bool up = row.MediaConnectState == MediaConnectStateConnected
            || row.OperStatus == IfOperStatusUp;
        const ULONG64 rxBps = row.ReceiveLinkSpeed;
        const ULONG64 txBps = row.TransmitLinkSpeed;
        const ULONG64 bps = rxBps ? rxBps : txBps;
        const int mbps = bps > 0 ? static_cast<int>((bps + 500000) / 1000000) : 0;

        NicInfo cur;
        cur.mac = mac;
        cur.mediaConnected = up;
        cur.receiveLinkBps = rxBps;
        cur.transmitLinkBps = txBps;
        cur.linkMbps = up ? mbps : 0;
        cur.inErrors = row.InErrors;
        cur.outErrors = row.OutErrors;
        cur.inDiscards = row.InDiscards;

        if (!want.isEmpty() && compact == want) {
            matched = cur;
            haveMatched = true;
            break;
        }
        if (up && mbps > bestMbps && row.Type == IF_TYPE_ETHERNET_CSMACD) {
            bestMbps = mbps;
            bestUp = cur;
        }
    }
    FreeMibTable(table);
    if (haveMatched)
        return matched;
    return bestUp;
#else
    Q_UNUSED(macHint);
    return out;
#endif
}

int nicLinkMbps(const QString &macHint)
{
    return nicInfo(macHint).linkMbps;
}

SsdHealth readSsdHealth(const QString &volumeLetter)
{
    SsdHealth out;
    out.status = QStringLiteral("unknown");
    out.media = QStringLiteral("unknown");
#ifdef Q_OS_WIN
    if (!ensureCom())
        return out;

    QChar letter = QLatin1Char('D');
    const QString trimmed = volumeLetter.trimmed();
    if (!trimmed.isEmpty()) {
        const QChar ch = trimmed.at(0).toUpper();
        if (ch >= QLatin1Char('C') && ch <= QLatin1Char('Z'))
            letter = ch;
    }

    IWbemServices *svc = connectNamespace(L"ROOT\\Microsoft\\Windows\\Storage");
    if (!svc)
        return out;

    const QString diskId = diskIdForLetter(svc, letter);
    if (diskId.isEmpty()) {
        svc->Release();
        return out;
    }

    IWbemClassObject *obj = nullptr;
    if (wmiFirst(svc,
                 QStringLiteral("SELECT HealthStatus, MediaType, BusType FROM MSFT_PhysicalDisk WHERE DeviceId = '%1'")
                     .arg(diskId),
                 &obj)) {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"HealthStatus", 0, &vt, nullptr, nullptr))) {
            const int hs = static_cast<int>(variantToDouble(vt));
            if (hs == 0)
                out.status = QStringLiteral("healthy");
            else if (hs == 1)
                out.status = QStringLiteral("warning");
            else if (hs >= 2)
                out.status = QStringLiteral("unhealthy");
        }
        VariantClear(&vt);
        VariantInit(&vt);
        int mediaType = -1;
        int busType = -1;
        if (SUCCEEDED(obj->Get(L"MediaType", 0, &vt, nullptr, nullptr)))
            mediaType = static_cast<int>(variantToDouble(vt));
        VariantClear(&vt);
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"BusType", 0, &vt, nullptr, nullptr)))
            busType = static_cast<int>(variantToDouble(vt));
        VariantClear(&vt);
        if (mediaType == 3)
            out.media = QStringLiteral("hdd");
        else if (mediaType == 5)
            out.media = QStringLiteral("scm");
        else if (busType == 17)
            out.media = QStringLiteral("nvme");
        else if (mediaType == 4)
            out.media = QStringLiteral("ssd");
        obj->Release();
    }

    obj = nullptr;
    if (wmiFirst(svc,
                 QStringLiteral("SELECT Wear, ReadErrorsTotal, WriteErrorsTotal "
                                "FROM MSFT_StorageReliabilityCounter WHERE DeviceId = '%1'")
                     .arg(diskId),
                 &obj)) {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"Wear", 0, &vt, nullptr, nullptr))) {
            const double w = variantToDouble(vt);
            if (w >= 0.0)
                out.wearPct = qBound(0, static_cast<int>(w + 0.5), 100);
        }
        VariantClear(&vt);
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"ReadErrorsTotal", 0, &vt, nullptr, nullptr)))
            out.readErrors = variantToInt64(vt);
        VariantClear(&vt);
        VariantInit(&vt);
        if (SUCCEEDED(obj->Get(L"WriteErrorsTotal", 0, &vt, nullptr, nullptr)))
            out.writeErrors = variantToInt64(vt);
        VariantClear(&vt);
        obj->Release();
    }
    svc->Release();

    if (out.wearPct >= 95 || (out.readErrors > 0 && out.wearPct >= 80)
        || (out.writeErrors > 0 && out.wearPct >= 80))
        out.status = QStringLiteral("unhealthy");
    else if (out.wearPct >= 80 && out.status == QLatin1String("healthy"))
        out.status = QStringLiteral("warning");
#else
    Q_UNUSED(volumeLetter);
#endif
    return out;
}

GameInventory scanGames(const QString &steamPath, const QString &epicPath)
{
    GameInventory inv;
    QVector<GameBuild> games;

    appendSteamLibrary(steamPath, &games);
    if (games.size() < 200) {
        for (const QString &lib : extraSteamLibraries(steamPath)) {
            appendSteamLibrary(lib, &games);
            if (games.size() >= 200)
                break;
        }
    }

    const QString programData = qEnvironmentVariable("PROGRAMDATA");
    const QString epicDefault = programData.isEmpty()
        ? QString()
        : QDir(programData).filePath(QStringLiteral("Epic/EpicGamesLauncher/Data/Manifests"));
    appendEpicManifests(epicDefault, &games);
    if (!epicPath.isEmpty() && QDir::cleanPath(epicPath) != QDir::cleanPath(epicDefault)) {
        appendEpicManifests(QDir(epicPath).filePath(QStringLiteral("Manifests")), &games);
        appendEpicManifests(epicPath, &games);
    }

    QStringList hashParts;
    for (const GameBuild &g : games) {
        if (g.platform == QLatin1String("steam"))
            inv.steamCount++;
        else if (g.platform == QLatin1String("epic"))
            inv.epicCount++;
        hashParts.append(g.platform + QLatin1Char(':') + g.appId + QLatin1Char(':') + g.buildId);
    }
    hashParts.sort();
    inv.games = games;
    inv.hash = sha1Hex(hashParts.join(QLatin1Char('\n')));
    return inv;
}

} // namespace StationHealth
