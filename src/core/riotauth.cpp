#include "riotauth.h"
#include "networkmanager.h"
#include "processmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSslError>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <UIAutomation.h>
#endif

static bool writeTextFile(const QString &path, const QString &content)
{
    if (content.isEmpty())
        return false;
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[RIOT] write fail:" << path << file.errorString();
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << content;
    file.close();
    return true;
}

static QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    return in.readAll();
}

static bool writeBinaryFile(const QString &path, const QByteArray &data)
{
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[RIOT] bin write fail:" << path << file.errorString();
        return false;
    }
    file.write(data);
    file.close();
    return true;
}

static QByteArray readBinaryFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

static QString jsonStringField(const QJsonObject &obj, const QString &key)
{
    const QJsonValue v = obj.value(key);
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(v.toDouble());
    return QString();
}

static QJsonObject extractVdfFiles(const QJsonObject &authData)
{
    QJsonObject vdf = authData.value(QStringLiteral("vdf_files")).toObject();
    const QJsonObject auth = authData.value(QStringLiteral("auth")).toObject();
    const QJsonObject cache = auth.value(QStringLiteral("cache")).toObject();
    QJsonObject cacheVdf = cache.value(QStringLiteral("vdf_files")).toObject();
    if (cacheVdf.isEmpty() && !cache.isEmpty() && cache.contains(QStringLiteral("local_vdf")))
        cacheVdf = cache;

    // Берём непустой local_vdf из любого вложения (QML/API иногда дублируют)
    auto pick = [](const QJsonObject &a, const QJsonObject &b) {
        QJsonObject out = a;
        const QStringList keys = {
            QStringLiteral("local_vdf"),
            QStringLiteral("config_vdf"),
            QStringLiteral("loginusers_vdf"),
        };
        for (const QString &k : keys) {
            if (jsonStringField(out, k).trimmed().isEmpty()) {
                const QString alt = jsonStringField(b, k);
                if (!alt.trimmed().isEmpty())
                    out.insert(k, alt);
            }
        }
        return out;
    };
    if (vdf.isEmpty())
        return cacheVdf;
    return pick(vdf, cacheVdf);
}

static QString riotDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/Riot Games/Riot Client/Data");
}

static QString riotConfigDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/Riot Games/Riot Client/Config");
}

static bool looksLikeRiotSessionYaml(const QString &body)
{
    if (body.trimmed().size() < 80)
        return false;
    const QString lower = body.toLower();
    return lower.contains(QStringLiteral("riot"))
        || lower.contains(QStringLiteral("session"))
        || lower.contains(QStringLiteral("cookie"))
        || lower.contains(QStringLiteral("persist"));
}

static bool yamlLooksDurableSession(const QString &body)
{
    if (body.trimmed().size() < 200)
        return false;
    const QString lower = body.toLower();
    // Silent нужен сильный токен — один tdid (~500–1000b) не хватает
    if (lower.contains(QStringLiteral("rso_token"))
        || lower.contains(QStringLiteral("access_token"))
        || lower.contains(QStringLiteral("id_token"))
        || lower.contains(QStringLiteral("refresh_token"))
        || lower.contains(QStringLiteral("name: \"ssid\""))
        || lower.contains(QStringLiteral("name: \"clid\"")))
        return true;
    // riot-login.persist: { ... } после longLivedSessionAccepted (не null)
    if (lower.contains(QStringLiteral("riot-login:"))
        && lower.contains(QStringLiteral("persist:"))
        && !lower.contains(QStringLiteral("persist: null"))
        && !lower.contains(QStringLiteral("persist:null"))
        && body.trimmed().size() >= 800)
        return true;
    return false;
}

// Полный machine-cache (yaml + Cookies/Sessions) — как EA blob
static bool looksLikeRiotDataBlob(const QString &body)
{
    const QString t = body.trimmed();
    return t.contains(QStringLiteral("riot_cache_version"))
        && t.contains(QStringLiteral("\"files\""));
}

static bool riotRelPathAllowed(const QString &rel)
{
    const QString r = QDir::fromNativeSeparators(rel);
    if (r.contains(QStringLiteral("Cache_Data"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("Code Cache"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("GPUCache"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("DawnCache"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("ShaderCache"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("Crashpad"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("/Logs/"), Qt::CaseInsensitive)
        || r.endsWith(QStringLiteral(".log"), Qt::CaseInsensitive)
        || r.contains(QStringLiteral("VideoDecodeStats"), Qt::CaseInsensitive))
        return false;
    return true;
}

static QStringList riotCacheRelativePaths()
{
    QStringList rels;
    const QString root = riotDataDir();
    QDir rootDir(root);
    if (!rootDir.exists())
        return rels;

    // Обязательные yaml
    const QStringList yamlNames = {
        QStringLiteral("RiotGamesPrivateSettings.yaml"),
        QStringLiteral("RiotClientPrivateSettings.yaml"),
        QStringLiteral("RiotClientSettings.yaml"),
    };
    for (const QString &name : yamlNames) {
        if (QFileInfo::exists(root + QLatin1Char('/') + name))
            rels << name;
    }

    // Config yaml (рядом с Data) — riot-login.persist и пр.
    const QStringList configYamlNames = {
        QStringLiteral("RiotClientSettings.yaml"),
    };
    for (const QString &name : configYamlNames) {
        const QString abs = riotConfigDir() + QLatin1Char('/') + name;
        if (QFileInfo::exists(abs))
            rels << QStringLiteral("Config/") + name;
    }

    // CEF session cookies / storage — без них silent не работает
    const QStringList dirRoots = {
        QStringLiteral("Cookies"),
        QStringLiteral("Sessions"),
        QStringLiteral("Local Storage"),
        QStringLiteral("Session Storage"),
        QStringLiteral("Network"),
    };
    for (const QString &sub : dirRoots) {
        const QString abs = root + QLatin1Char('/') + sub;
        if (!QDir(abs).exists())
            continue;
        QDirIterator it(abs, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QString rel = QDir::fromNativeSeparators(rootDir.relativeFilePath(it.filePath()));
            if (!riotRelPathAllowed(rel))
                continue;
            if (it.fileInfo().size() > 3 * 1024 * 1024)
                continue;
            rels << rel;
        }
    }
    rels.removeDuplicates();
    return rels;
}

static QString packRiotCacheBlob()
{
    const QString root = riotDataDir();
    QJsonObject files;
    qint64 total = 0;
    constexpr qint64 kMaxTotal = 8 * 1024 * 1024;
    int cookieFiles = 0;
    int sessionFiles = 0;

    for (const QString &rel : riotCacheRelativePaths()) {
        const QString abs = rel.startsWith(QStringLiteral("Config/"), Qt::CaseInsensitive)
            ? riotConfigDir() + QLatin1Char('/') + rel.mid(7)
            : root + QLatin1Char('/') + rel;
        const QByteArray raw = readBinaryFile(abs);
        if (raw.isEmpty())
            continue;
        if (total + raw.size() > kMaxTotal) {
            qWarning() << "[RIOT] cache pack: лимит, skip" << rel;
            continue;
        }
        files.insert(rel, QString::fromLatin1(raw.toBase64()));
        total += raw.size();
        if (rel.startsWith(QStringLiteral("Cookies"), Qt::CaseInsensitive))
            ++cookieFiles;
        if (rel.startsWith(QStringLiteral("Sessions"), Qt::CaseInsensitive))
            ++sessionFiles;
    }

    if (files.isEmpty())
        return {};

    QJsonObject rootObj;
    rootObj.insert(QStringLiteral("riot_cache_version"), 1);
    rootObj.insert(QStringLiteral("files"), files);
    const QByteArray json = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);
    qWarning() << "[RIOT] cache pack: files" << files.size()
               << "| cookies" << cookieFiles << "| sessions" << sessionFiles
               << "| raw" << total << "| json" << json.size()
               << "| keys" << files.keys();
    return QString::fromUtf8(json);
}

static bool unpackRiotCacheBlob(const QString &blob)
{
    const QString trimmed = blob.trimmed();
    if (trimmed.isEmpty())
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[RIOT] cache unpack: не JSON" << err.errorString();
        return false;
    }
    const QJsonObject rootObj = doc.object();
    if (rootObj.value(QStringLiteral("riot_cache_version")).toInt() < 1) {
        qWarning() << "[RIOT] cache unpack: нет riot_cache_version";
        return false;
    }
    const QJsonObject files = rootObj.value(QStringLiteral("files")).toObject();
    if (files.isEmpty())
        return false;

    const QString root = riotDataDir();
    int ok = 0;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString rel = it.key();
        if (rel.contains(QLatin1String("..")) || QDir::isAbsolutePath(rel))
            continue;
        if (!riotRelPathAllowed(rel))
            continue;
        const QByteArray raw = QByteArray::fromBase64(it.value().toString().toLatin1());
        if (raw.isEmpty())
            continue;
        const QString abs = rel.startsWith(QStringLiteral("Config/"), Qt::CaseInsensitive)
            ? riotConfigDir() + QLatin1Char('/') + rel.mid(7)
            : root + QLatin1Char('/') + rel;
        if (writeBinaryFile(abs, raw))
            ++ok;
    }
    qWarning() << "[RIOT] cache unpack: восстановлено" << ok << "/" << files.size()
               << "→" << root;
    return ok > 0;
}

static bool riotBlobLooksSilentReady(const QString &blob)
{
    if (!looksLikeRiotDataBlob(blob))
        return false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(blob.trimmed().toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject files = doc.object().value(QStringLiteral("files")).toObject();
    bool hasYaml = false;
    bool hasCookiesOrSessions = false;
    bool hasDurableYaml = false;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString k = it.key();
        if (k.contains(QStringLiteral("RiotGamesPrivateSettings"), Qt::CaseInsensitive)
            || k.contains(QStringLiteral("RiotClientSettings"), Qt::CaseInsensitive)) {
            hasYaml = true;
            const QByteArray raw = QByteArray::fromBase64(it.value().toString().toLatin1());
            if (yamlLooksDurableSession(QString::fromUtf8(raw)))
                hasDurableYaml = true;
        }
        if (k.startsWith(QStringLiteral("Cookies"), Qt::CaseInsensitive)
            || k.startsWith(QStringLiteral("Sessions"), Qt::CaseInsensitive)
            || k.contains(QStringLiteral("Local Storage"), Qt::CaseInsensitive))
            hasCookiesOrSessions = true;
    }
    return hasYaml && (hasCookiesOrSessions || hasDurableYaml);
}

#ifdef Q_OS_WIN
static QString processImageForPid(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return QString();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    QString name;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                name = QString::fromWCharArray(pe.szExeFile);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return name;
}

static QSet<DWORD> collectRiotPids()
{
    QSet<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile);
            if (name.contains(QStringLiteral("RiotClient"), Qt::CaseInsensitive)
                || name.compare(QStringLiteral("Riot Client.exe"), Qt::CaseInsensitive) == 0
                || name.contains(QStringLiteral("LeagueClient"), Qt::CaseInsensitive)) {
                pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static void sendVk(WORD vk)
{
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void sendReturnKey() { sendVk(VK_RETURN); }

static void sendTabs(int count, const char *why)
{
    qWarning() << "[RIOT] Tab x" << count << why;
    for (int i = 0; i < count; ++i) {
        sendVk(VK_TAB);
        Sleep(140);
    }
}

static void typeUnicode(const QString &text)
{
    for (const QChar ch : text) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = 0;
        in[0].ki.wScan = ch.unicode();
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(35);
    }
}

static void setShellTopmostFrom(QObject *auth, bool enabled)
{
    if (auto *pm = qobject_cast<ProcessManager *>(auth->parent()))
        pm->setShellTopmost(enabled);
}

// Off-screen park: Riot остаётся SW_SHOW (CEF/UIA живы), но за виртуальным экраном.
static bool riotRectLooksOnScreen(const RECT &rc)
{
    // Уже запаркованные окна ~ -32000
    return rc.left > -10000 && rc.top > -10000;
}

void RiotAuth::keepShellOverlayUp()
{
    setShellTopmostFrom(this, true);
}

void RiotAuth::parkRiotOffscreen(quintptr hwndVal, const char *why)
{
#ifdef Q_OS_WIN
    HWND h = reinterpret_cast<HWND>(hwndVal);
    if (!h || !IsWindow(h)) {
        h = FindWindowW(nullptr, L"Riot Client");
        if (!h || !IsWindow(h))
            h = reinterpret_cast<HWND>(m_loginHwnd);
    }
    if (!h || !IsWindow(h))
        return;

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    if (w < 100 || hgt < 100)
        return;

    // Сохраняем on-screen rect один раз (не перезаписываем координатами -32000)
    if (riotRectLooksOnScreen(rc)) {
        m_riotSavedX = rc.left;
        m_riotSavedY = rc.top;
        m_riotSavedW = w;
        m_riotSavedH = hgt;
        m_riotParkedHwnd = reinterpret_cast<quintptr>(h);
        m_riotParkedOffscreen = true;
    } else if (!m_riotParkedOffscreen) {
        // Уже off-screen без нашего save — всё равно помечаем
        m_riotParkedHwnd = reinterpret_cast<quintptr>(h);
        m_riotParkedOffscreen = true;
        if (m_riotSavedW <= 0) {
            m_riotSavedW = w;
            m_riotSavedH = hgt;
        }
    }

    // Держим размер, только сдвигаем. Не SW_HIDE — CEF/логин должны жить.
    const int ox = -32000;
    const int oy = -32000;
    if (rc.left != ox || rc.top != oy) {
        SetWindowPos(h, nullptr, ox, oy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        qWarning() << "[RIOT] park off-screen" << ox << oy
                   << "size" << (m_riotSavedW > 0 ? m_riotSavedW : w)
                   << "x" << (m_riotSavedH > 0 ? m_riotSavedH : hgt)
                   << "|" << why;
    }
    keepShellOverlayUp();
#else
    Q_UNUSED(hwndVal);
    Q_UNUSED(why);
#endif
}

void RiotAuth::restoreRiotOnscreen(const char *why)
{
#ifdef Q_OS_WIN
    HWND h = reinterpret_cast<HWND>(m_riotParkedHwnd);
    if (!h || !IsWindow(h))
        h = reinterpret_cast<HWND>(m_loginHwnd);
    if (!h || !IsWindow(h))
        h = FindWindowW(nullptr, L"Riot Client");
    if (!h || !IsWindow(h))
        return;

    int x = m_riotSavedX;
    int y = m_riotSavedY;
    int w = m_riotSavedW;
    int hgt = m_riotSavedH;
    if (w < 400 || hgt < 300) {
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        w = 1536;
        hgt = 864;
        x = (sw - w) / 2;
        y = (sh - hgt) / 2;
    }
    SetWindowPos(h, nullptr, x, y, w, hgt,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_riotParkedOffscreen = false;
    qWarning() << "[RIOT] restore on-screen" << x << y << w << "x" << hgt << "|" << why;
    keepShellOverlayUp();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::keepOverlayOverRiot(quintptr hwndVal)
{
    // Только TOPMOST оверлея. Park — явно после Enter / при ожидании Play.
    Q_UNUSED(hwndVal);
    keepShellOverlayUp();
}

struct RiotLoginEnumCtx {
    HWND best = nullptr;
    QString title;
    int score = -1;
    QSet<DWORD> riotPids;
    QStringList dump;
    bool doDump = false;
};

static bool isRiotLoginTitle(const QString &t)
{
    if (t.isEmpty())
        return false;
    return t.contains(QStringLiteral("Riot Client"), Qt::CaseInsensitive)
           || t.compare(QStringLiteral("Riot"), Qt::CaseInsensitive) == 0
           || t.contains(QStringLiteral("Sign in"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Войти"), Qt::CaseInsensitive);
}

static void considerRiotCandidate(RiotLoginEnumCtx *c, HWND h, const QString &t,
                                  const QString &cls, int w, int hgt, DWORD pid, bool fromRiot)
{
    if (!fromRiot && !isRiotLoginTitle(t))
        return;

    const bool chrome = cls.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("Chrome_RenderWidget"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("Intermediate D3D"), Qt::CaseInsensitive);

    // Любое крупное окно Riot-процесса — кандидат (раньше порог 70 отсекал всё)
    int score = w * hgt / 1000;
    if (fromRiot) score += 100;
    if (isRiotLoginTitle(t)) score += 80;
    if (chrome) score += 50;
    if (w >= 500 && hgt >= 400)
        score += 40;

    if (score < 50)
        return;

    if (score > c->score) {
        c->score = score;
        c->best = h;
        c->title = (t.isEmpty() ? QStringLiteral("(no title)") : t)
                   + QStringLiteral(" [") + cls + QLatin1Char(' ')
                   + QString::number(w) + QLatin1Char('x') + QString::number(hgt)
                   + QStringLiteral(" pid=") + QString::number(pid)
                   + QStringLiteral(" score=") + QString::number(score)
                   + QLatin1Char(']');
    }
}

static BOOL CALLBACK enumRiotLoginProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<RiotLoginEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    const bool fromRiot = c->riotPids.contains(pid);

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString cls = QString::fromWCharArray(wcls);

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    if (w < 400 || hgt < 300)
        return TRUE;

    if (c->doDump && fromRiot) {
        c->dump << QStringLiteral("TOP pid=%1 %2 | \"%3\" | %4 %5x%6")
                       .arg(pid)
                       .arg(processImageForPid(pid))
                       .arg(t.isEmpty() ? QStringLiteral("(no title)") : t)
                       .arg(cls)
                       .arg(w)
                       .arg(hgt);
    }

    considerRiotCandidate(c, h, t, cls, w, hgt, pid, fromRiot);
    return TRUE;
}
#endif

RiotAuth::RiotAuth(QObject *parent)
    : IPlatformAuth(parent)
{
}

RiotAuth::~RiotAuth()
{
    stopScout();
}

void RiotAuth::silentKill(const QString &image)
{
#ifdef Q_OS_WIN
    auto *p = new QProcess;
    p->setProgram(QStringLiteral("taskkill"));
    p->setArguments({QStringLiteral("/F"), QStringLiteral("/T"),
                     QStringLiteral("/IM"), image});
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     p, &QObject::deleteLater);
    p->start();
#else
    Q_UNUSED(image);
#endif
}

static void softCloseLeagueWindows()
{
#ifdef Q_OS_WIN
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (!IsWindowVisible(hwnd))
            return TRUE;
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 255);
        const QString t = QString::fromWCharArray(title);
        if (t.contains(QStringLiteral("League of Legends"), Qt::CaseInsensitive)
            || t.compare(QStringLiteral("Riot Client"), Qt::CaseInsensitive) == 0
            || t.contains(QStringLiteral("Vanguard"), Qt::CaseInsensitive)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }, 0);
    qWarning() << "[RIOT] soft WM_CLOSE League/Riot windows";
#endif
}

// Space на Tab-фокусе открывал recovery.riotgames («Не можете войти?») — закрываем.
static void closeRiotRecoveryBrowsers()
{
#ifdef Q_OS_WIN
    int closed = 0;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd))
            return TRUE;
        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, 511);
        if (!title[0])
            return TRUE;
        const QString t = QString::fromWCharArray(title);
        const bool recovery =
            t.contains(QStringLiteral("Не можете войти"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Восстановление учетн"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Восстановление учётн"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Account recovery"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Can't sign in"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Cant sign in"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("recovery.riotgames"), Qt::CaseInsensitive);
        if (!recovery)
            return TRUE;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ++(*reinterpret_cast<int *>(lp));
        qWarning().noquote() << "[RIOT] WM_CLOSE recovery browser:" << t;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&closed));
    if (closed > 0)
        qWarning() << "[RIOT] closeRiotRecoveryBrowsers closed" << closed;
#endif
}

static bool riotVgcIsRunning()
{
#ifdef Q_OS_WIN
    QProcess p;
    p.setProgram(QStringLiteral("sc"));
    p.setArguments({QStringLiteral("query"), QStringLiteral("vgc")});
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    p.start();
    if (!p.waitForFinished(4000))
        return false;
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    return out.contains(QStringLiteral("RUNNING"), Qt::CaseInsensitive);
#else
    return true;
#endif
}

static bool restartVanguardService()
{
#ifdef Q_OS_WIN
    // На этом ПК vgc часто STOPPED + SERVICE_EXIT_CODE 216 (= VAN 216).
    // vgk (kernel) жив, user-mode vgc мёртв — без рестарта League сразу VAN 216.
    auto runSc = [](const QStringList &args) {
        QProcess p;
        p.setProgram(QStringLiteral("sc"));
        p.setArguments(args);
        p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
            a->flags |= CREATE_NO_WINDOW;
        });
        p.start();
        p.waitForFinished(8000);
        return QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError());
    };

    if (riotVgcIsRunning()) {
        qWarning() << "[RIOT] vgc уже RUNNING";
        return true;
    }

    qWarning() << "[RIOT] vgc не RUNNING — stop/start (fix VAN 216)";
    runSc({QStringLiteral("stop"), QStringLiteral("vgc")});
    Sleep(2000);
    const QString startOut = runSc({QStringLiteral("start"), QStringLiteral("vgc")});
    qWarning().noquote() << "[RIOT] sc start vgc:" << startOut.left(200).trimmed();

    for (int i = 0; i < 24; ++i) { // ~12s
        Sleep(500);
        if (riotVgcIsRunning()) {
            qWarning() << "[RIOT] vgc RUNNING после рестарта, attempt" << (i + 1);
            return true;
        }
    }
    qWarning() << "[RIOT] CRITICAL: vgc так и не RUNNING — жди VAN 216."
               << "Нужен reboot / Secure Boot+TPM / переустановка Vanguard";
    return false;
#else
    return true;
#endif
}

void RiotAuth::prepareGracefulShutdown()
{
    qWarning() << "[RIOT] graceful shutdown — WM_CLOSE, ждём flush CEF/persist yaml";
    m_launchAborted = true;
    m_playClickStop = true;
    m_overlayDismissed = true;
    softCloseLeagueWindows();
    // Попытка штатного выхода через lockfile API (если есть)
#ifdef Q_OS_WIN
    int port = 0;
    QString lockPass;
    QString protocol;
    if (readRiotLockfile(&port, &lockPass, &protocol)) {
        const QByteArray authHdr = QByteArrayLiteral("Basic ")
            + QStringLiteral("riot:%1").arg(lockPass).toUtf8().toBase64();
        // Распространённые quit endpoints (могут 404 — не страшно)
        const QStringList quitPaths = {
            QStringLiteral("/process-control/v1/process/quit"),
            QStringLiteral("/riotclient/kill"),
            QStringLiteral("/riot-client/v1/quit"),
        };
        for (const QString &path : quitPaths) {
            QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
            QNetworkRequest req(url);
            req.setRawHeader("Authorization", authHdr);
            req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
            QNetworkReply *reply = ensureNam()->post(req, QByteArray());
            QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        }
        qWarning() << "[RIOT] graceful: отправили quit API (best-effort)";
    }
#endif
}

void RiotAuth::forceKillRemaining()
{
    qWarning() << "[RIOT] forceKillRemaining (после flush)";
    silentKill(QStringLiteral("RiotClientServices.exe"));
    silentKill(QStringLiteral("RiotClient.exe"));
    silentKill(QStringLiteral("RiotClientUx.exe"));
    silentKill(QStringLiteral("Riot Client.exe"));
    silentKill(QStringLiteral("LeagueClient.exe"));
    silentKill(QStringLiteral("LeagueClientUx.exe"));
    silentKill(QStringLiteral("LeagueClientUxRender.exe"));
    silentKill(QStringLiteral("LeagueCrashHandler64.exe"));
}

void RiotAuth::killLauncher()
{
    qWarning() << "[RIOT] killLauncher";
    m_riotParkedOffscreen = false;
    m_riotParkedHwnd = 0;
    m_riotSavedW = 0;
    m_riotSavedH = 0;
    // Перед стартом новой сессии: soft → короткая пауза → force
    softCloseLeagueWindows();
#ifdef Q_OS_WIN
    Sleep(2000);
#endif
    forceKillRemaining();
    m_launchAborted = true;
    m_playClickStop = true;
    m_overlayDismissed = true;
}

void RiotAuth::clearLocalSession()
{
    const QString base = riotDataDir();
    const QStringList files = {
        base + QStringLiteral("/RiotGamesPrivateSettings.yaml"),
        base + QStringLiteral("/RiotClientPrivateSettings.yaml"),
        base + QStringLiteral("/RiotClientSettings.yaml"),
        // Config persist yaml тоже даёт silent (riot-login.persist)
        riotConfigDir() + QStringLiteral("/RiotClientSettings.yaml"),
    };
    for (const QString &path : files) {
        if (QFile::exists(path) && QFile::remove(path))
            qWarning() << "[RIOT] cleared session file:" << path;
    }
    // CEF cookies/sessions — иначе silent чужого аккаунта
    const QStringList dirs = {
        QStringLiteral("Cookies"),
        QStringLiteral("Sessions"),
        QStringLiteral("Local Storage"),
        QStringLiteral("Session Storage"),
        QStringLiteral("Network"),
    };
    for (const QString &sub : dirs) {
        const QString abs = base + QLatin1Char('/') + sub;
        if (QDir(abs).exists()) {
            QDir(abs).removeRecursively();
            qWarning() << "[RIOT] cleared session dir:" << abs;
        }
    }
}

static QString riotGamesRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/Riot Games");
}

static bool riotRemovePathLogged(const QString &path, const char *kind)
{
    const QFileInfo fi(path);
    if (!fi.exists())
        return false;
    bool ok = false;
    if (fi.isDir())
        ok = QDir(path).removeRecursively();
    else
        ok = QFile::remove(path);
    if (ok) {
        qWarning() << "[RIOT] personal wipe: removed" << kind << path;
    } else {
        qWarning() << "[RIOT] personal wipe: FAILED" << kind << path;
    }
    return ok;
}

static bool riotPrivateYamlPresent(const QString &yamlPath)
{
    QFileInfo fi(yamlPath);
    if (!fi.exists())
        return false;
    // Empty stub is still "present" for RSO — treat any existing file as bad.
    return true;
}

void RiotAuth::clearLocalSessionHard()
{
    qWarning() << "[RIOT] personal: disk wipe START";

    const QString clientRoot = riotGamesRoot() + QStringLiteral("/Riot Client");
    const QString clientData = clientRoot + QStringLiteral("/Data");
    const QString clientConfig = clientRoot + QStringLiteral("/Config");
    const QString lolRoot = riotGamesRoot() + QStringLiteral("/League of Legends");
    const QString lolData = lolRoot + QStringLiteral("/Data");
    const QString lolConfig = lolRoot + QStringLiteral("/Config");

    // Durable / private yaml — Client + League (League copy also silent-restores)
    const QStringList yamlFiles = {
        clientData + QStringLiteral("/RiotGamesPrivateSettings.yaml"),
        clientData + QStringLiteral("/RiotClientPrivateSettings.yaml"),
        clientData + QStringLiteral("/RiotClientSettings.yaml"),
        clientData + QStringLiteral("/ShutdownData.yaml"),
        clientConfig + QStringLiteral("/RiotClientSettings.yaml"),
        lolData + QStringLiteral("/RiotGamesPrivateSettings.yaml"),
        lolData + QStringLiteral("/RiotClientPrivateSettings.yaml"),
        lolData + QStringLiteral("/RiotClientSettings.yaml"),
        lolData + QStringLiteral("/ShutdownData.yaml"),
        lolConfig + QStringLiteral("/RiotClientSettings.yaml"),
    };
    for (const QString &path : yamlFiles)
        riotRemovePathLogged(path, "yaml");

    // Any other *.yaml under Client/League Data (future persist names)
    for (const QString &dataDir : {clientData, lolData}) {
        QDir dir(dataDir);
        if (!dir.exists())
            continue;
        const QFileInfoList yamls = dir.entryInfoList(
            QStringList{QStringLiteral("*.yaml"), QStringLiteral("*.yml")},
            QDir::Files | QDir::Hidden | QDir::System);
        for (const QFileInfo &fi : yamls)
            riotRemovePathLogged(fi.absoluteFilePath(), "yaml");
    }

    // CEF / Electron persist under Data + Client root
    const QStringList cefDirNames = {
        QStringLiteral("Cookies"),
        QStringLiteral("Sessions"),
        QStringLiteral("Local Storage"),
        QStringLiteral("Session Storage"),
        QStringLiteral("Network"),
        QStringLiteral("IndexedDB"),
        QStringLiteral("Service Worker"),
        QStringLiteral("Cache"),
        QStringLiteral("Cache_Data"),
        QStringLiteral("Code Cache"),
        QStringLiteral("GPUCache"),
        QStringLiteral("DawnCache"),
        QStringLiteral("ShaderCache"),
        QStringLiteral("blob_storage"),
        QStringLiteral("File System"),
        QStringLiteral("WebStorage"),
    };
    for (const QString &root : {clientData, clientRoot, lolData, lolRoot}) {
        for (const QString &sub : cefDirNames)
            riotRemovePathLogged(root + QLatin1Char('/') + sub, "dir");
        riotRemovePathLogged(root + QStringLiteral("/HttpCache"), "dir");
    }

    // Roaming (rare, but wipe if present)
    const QString roaming = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                            + QStringLiteral("/Riot Games");
    if (QDir(roaming).exists()) {
        riotRemovePathLogged(roaming + QStringLiteral("/Riot Client/Data/RiotGamesPrivateSettings.yaml"),
                             "yaml");
        riotRemovePathLogged(roaming + QStringLiteral("/Riot Client/Config/RiotClientSettings.yaml"),
                             "yaml");
        riotRemovePathLogged(roaming + QStringLiteral("/League of Legends/Data/RiotGamesPrivateSettings.yaml"),
                             "yaml");
    }

    qWarning() << "[RIOT] personal: disk wipe DONE";
}

bool RiotAuth::applyCache(const QJsonObject &authData)
{
    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }
    if (!exe.isEmpty())
        m_launcherExe = exe;
    m_gameTitle = authData.value(QStringLiteral("game_title")).toString();

    const QString mode = authData.value(QStringLiteral("auth")).toObject()
                             .value(QStringLiteral("mode")).toString();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    const QString login = authData.value(QStringLiteral("login")).toString().trimmed();
    const QString password = authData.value(QStringLiteral("password")).toString();

    // Личный вход: NEVER unpack DB machine-cache; hard-wipe disk → форма логина.
    const bool personal = (mode.compare(QStringLiteral("personal"), Qt::CaseInsensitive) == 0)
        || (platformSource.compare(QStringLiteral("personal_account"), Qt::CaseInsensitive) == 0)
        || login.isEmpty() || password.isEmpty();
    if (personal) {
        // Probe only — do NOT unpack / write. Dashboard personal payload has no vdf;
        // club take-account must never reach here with mode=personal.
        const QJsonObject vdfProbe = extractVdfFiles(authData);
        const int localChars = jsonStringField(vdfProbe, QStringLiteral("local_vdf")).size();
        const int configChars = jsonStringField(vdfProbe, QStringLiteral("config_vdf")).size();
        qWarning() << "[RIOT] personal: DB cache SKIP"
                    << "| mode:" << mode
                    << "| platform_source:" << platformSource
                    << "| top.vdf_files:" << authData.contains(QStringLiteral("vdf_files"))
                    << "| local_vdf chars:" << localChars
                    << "| config_vdf chars:" << configChars
                    << "| auth.cache keys:"
                    << authData.value(QStringLiteral("auth")).toObject()
                           .value(QStringLiteral("cache")).toObject().keys();

        // Kill leftovers that may hold file locks, wait until gone, then wipe.
        qWarning() << "[RIOT] applyCache: personal — kill Riot + full logout wipe";
        forceKillRemaining();
#ifdef Q_OS_WIN
        {
            auto riotAlive = []() -> bool {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == INVALID_HANDLE_VALUE)
                    return false;
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof(pe);
                bool found = false;
                if (Process32FirstW(snap, &pe)) {
                    do {
                        const QString n = QString::fromWCharArray(pe.szExeFile);
                        if (n.compare(QStringLiteral("RiotClientServices.exe"), Qt::CaseInsensitive) == 0
                            || n.compare(QStringLiteral("RiotClientUx.exe"), Qt::CaseInsensitive) == 0
                            || n.compare(QStringLiteral("LeagueClientUx.exe"), Qt::CaseInsensitive) == 0
                            || n.compare(QStringLiteral("LeagueClient.exe"), Qt::CaseInsensitive) == 0) {
                            found = true;
                            break;
                        }
                    } while (Process32NextW(snap, &pe));
                }
                CloseHandle(snap);
                return found;
            };
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 8000;
            while (riotAlive() && QDateTime::currentMSecsSinceEpoch() < deadline) {
                forceKillRemaining();
                QThread::msleep(250);
            }
            if (riotAlive())
                qWarning() << "[RIOT] WARN: killAndWait timeout — wipe всё равно";
            else
                qWarning() << "[RIOT] killAndWait: процессы Riot завершены";
            QThread::msleep(300);
        }
#endif
        clearLocalSessionHard();
        qWarning() << "[RIOT] personal: full logout wipe | yaml+CEF Client/League Data+Config (+Roaming)";

        const QString yamlPath = riotDataDir()
                                 + QStringLiteral("/RiotGamesPrivateSettings.yaml");
        if (riotPrivateYamlPresent(yamlPath)) {
            qWarning() << "[RIOT] WARN: personal wipe — RiotGamesPrivateSettings.yaml still present,"
                       << "retry delete after forceKill";
            forceKillRemaining();
#ifdef Q_OS_WIN
            QThread::msleep(800);
#endif
            riotRemovePathLogged(yamlPath, "yaml");
            clearLocalSessionHard();
            if (riotPrivateYamlPresent(yamlPath)) {
                qWarning() << "[RIOT] WARN: personal wipe — yaml STILL present after retry:"
                           << yamlPath;
            } else {
                qWarning() << "[RIOT] personal wipe: verified yaml gone after retry";
            }
        } else {
            qWarning() << "[RIOT] personal wipe: verified RiotGamesPrivateSettings.yaml absent";
        }

        m_expectInteractive = false;
        m_allowsGameDetect = true;
        m_needBackup = false;
        return true;
    }

    const QJsonObject vdf = extractVdfFiles(authData);
    QString blob = jsonStringField(vdf, QStringLiteral("local_vdf"));
    if (!looksLikeRiotDataBlob(blob) && !yamlLooksDurableSession(blob)
        && !looksLikeRiotSessionYaml(blob)) {
        const QString alt = jsonStringField(vdf, QStringLiteral("config_vdf"));
        if (looksLikeRiotDataBlob(alt) || yamlLooksDurableSession(alt)
            || looksLikeRiotSessionYaml(alt))
            blob = alt;
    }

    qWarning() << "[RIOT] applyCache: vdf keys" << vdf.keys()
               << "| local_vdf" << jsonStringField(vdf, QStringLiteral("local_vdf")).size()
               << "| config_vdf" << jsonStringField(vdf, QStringLiteral("config_vdf")).size()
               << "| mode" << mode
               << "| top.vdf_files" << authData.contains(QStringLiteral("vdf_files"))
               << "| auth.cache" << authData.value(QStringLiteral("auth")).toObject()
                      .value(QStringLiteral("cache")).toObject().keys();

    if (riotBlobLooksSilentReady(blob)) {
        clearLocalSession(); // чистое место под cookies/yaml
        if (unpackRiotCacheBlob(blob)) {
            qWarning() << "[RIOT] applyCache: тихий вход — Data blob для" << login
                       << "| chars:" << blob.size();
            m_expectInteractive = false;
            m_allowsGameDetect = true;
            m_needBackup = false;
            return true;
        }
        qWarning() << "[RIOT] applyCache: unpack Data blob fail";
    } else if (looksLikeRiotDataBlob(blob)) {
        qWarning() << "[RIOT] applyCache: blob есть, но без Cookies/Sessions/durable yaml → interactive"
                   << "| chars:" << blob.size();
    } else if (yamlLooksDurableSession(blob)) {
        // На этой сборке Riot Cookies/ нет — silent = yaml с сильными токенами
        clearLocalSession();
        const QString yamlPath = riotDataDir() + QStringLiteral("/RiotGamesPrivateSettings.yaml");
        if (writeTextFile(yamlPath, blob)) {
            const QString cfg = jsonStringField(vdf, QStringLiteral("config_vdf"));
            if (!cfg.trimmed().isEmpty())
                writeTextFile(riotConfigDir() + QStringLiteral("/RiotClientSettings.yaml"), cfg);
            qWarning() << "[RIOT] applyCache: тихий вход — durable yaml для" << login
                       << "| chars:" << blob.size();
            m_expectInteractive = false;
            m_allowsGameDetect = true;
            m_needBackup = false;
            return true;
        }
        qWarning() << "[RIOT] applyCache: не удалось записать durable yaml";
    } else if (looksLikeRiotSessionYaml(blob)) {
        qWarning() << "[RIOT] applyCache: только тонкий yaml (" << blob.size()
                   << "chars) — silent невозможен → interactive";
    } else {
        qWarning() << "[RIOT] applyCache: нет machine-cache для" << login
                   << "| local_vdf chars:" << blob.size();
    }

    // No usable DB machine-cache → wipe leftover disk durable so scout always runs.
    clearLocalSession();
    qWarning() << "[RIOT] clearLocalSession() — no usable DB machine-cache (force scout)";

    m_expectInteractive = true;
    m_allowsGameDetect = false;
    m_needBackup = true;
    qWarning() << "[RIOT] applyCache: клубный → interactive scout для" << login;
    return false;
}

void RiotAuth::startLauncher(QProcess *process,
                             const QJsonObject &authData,
                             const QString &appIdHint)
{
    Q_UNUSED(appIdHint);
    if (!process)
        return;

    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }

    QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();
    if (argsStr.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        argsStr = launcher.value(QStringLiteral("args")).toString().trimmed();
    }

    const QString defaultRiot =
        QStringLiteral("C:\\Riot Games\\Riot Client\\RiotClientServices.exe");
    const QString title = authData.value(QStringLiteral("game_title")).toString();
    if (!title.isEmpty())
        m_gameTitle = title;

    auto looksLikeExePath = [](const QString &s) {
        const QString t = s.trimmed();
        if (t.isEmpty())
            return false;
        if (t.contains(QStringLiteral("--launch-product"), Qt::CaseInsensitive))
            return false;
        return t.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Riot Client"), Qt::CaseInsensitive)
            || t.startsWith(QStringLiteral(":\\"))
            || (t.size() >= 3 && t.at(1) == QLatin1Char(':'));
    };

    if (looksLikeExePath(argsStr)) {
        QString recovered = argsStr;
        if (recovered.startsWith(QStringLiteral(":\\")))
            recovered.prepend(QLatin1Char('C'));
        if (recovered.contains(QStringLiteral("RiotClientServices"), Qt::CaseInsensitive))
            exe = recovered;
        argsStr.clear();
    }

    if (!exe.contains(QStringLiteral("RiotClientServices"), Qt::CaseInsensitive)) {
        if (QFileInfo::exists(defaultRiot))
            exe = defaultRiot;
    }

    if (argsStr.isEmpty()) {
        if (m_gameTitle.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive))
            argsStr = QStringLiteral("--launch-product=valorant --launch-patchline=live");
        else if (m_gameTitle.contains(QStringLiteral("League"), Qt::CaseInsensitive))
            argsStr = QStringLiteral("--launch-product=league_of_legends --launch-patchline=live");
    }

    if (exe.isEmpty()) {
        qCritical() << "[RIOT] exe_path пуст";
        return;
    }

    const QFileInfo fi(exe);
    m_launcherExe = exe;
    m_productArgs = argsStr;
    m_productLaunchScheduled = false;
    m_productLaunchOk = false;
    m_playClickDone = false;
    m_playClickStop = false;
    m_overlayDismissScheduled = false;
    m_overlayDismissed = false;
    m_lcuTutorialSkipDone = false;
    m_leagueHeaderPlayDone = false;
    m_headerPlayAttempts = 0;
    m_leagueLobbyPosted = false;
    m_lobbyPostAttempts = 0;
    m_launchAborted = false;
    m_riotParkedOffscreen = false;
    m_riotParkedHwnd = 0;
    m_riotSavedW = 0;
    m_riotSavedH = 0;
    process->setWorkingDirectory(fi.absolutePath());

    // С product-args с первого старта — после логина Riot часто сам продолжает launch.
    // Доп. ShellExecute только ПОСЛЕ записи session yaml (раньше — сброс логина).
    QStringList args;
    if (!argsStr.isEmpty())
        args = QProcess::splitCommand(argsStr);
    // --stay-signed-in на этой машине бесполезен (keypairSupported=false) и шумит attestation
    if (m_expectInteractive)
        qWarning() << "[RIOT] старт с product (после логина ждём session yaml):" << args.join(QLatin1Char(' '));

    // Оверлей остаётся topmost; Riot не должен всплывать поверх loading.
    keepOverlayOverRiot();

    qWarning().noquote() << "[RIOT] Launch exe:" << exe;
    qWarning().noquote() << "[RIOT] Launch args:" << args;
    if (!restartVanguardService())
        qWarning() << "[RIOT] WARN: стартуем Riot при мёртвом vgc — вероятен VAN 216";
    process->start(exe, args);
}

bool RiotAuth::isGameProcessRunning() const
{
#ifdef Q_OS_WIN
    const QStringList images = {
        QStringLiteral("LeagueClient.exe"),
        QStringLiteral("LeagueClientUx.exe"),
        QStringLiteral("VALORANT-Win64-Shipping.exe"),
        QStringLiteral("Valorant.exe"),
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile);
            for (const QString &img : images) {
                if (name.compare(img, Qt::CaseInsensitive) == 0) {
                    found = true;
                    break;
                }
            }
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    return false;
#endif
}

bool RiotAuth::parseProductArgs(QString *productId, QString *patchline) const
{
    if (!productId || !patchline)
        return false;
    *productId = QStringLiteral("league_of_legends");
    *patchline = QStringLiteral("live");
    if (m_gameTitle.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive))
        *productId = QStringLiteral("valorant");

    const QStringList parts = QProcess::splitCommand(m_productArgs);
    for (const QString &p : parts) {
        if (p.startsWith(QStringLiteral("--launch-product="), Qt::CaseInsensitive))
            *productId = p.mid(QStringLiteral("--launch-product=").size());
        else if (p.startsWith(QStringLiteral("--launch-patchline="), Qt::CaseInsensitive))
            *patchline = p.mid(QStringLiteral("--launch-patchline=").size());
    }
    return !productId->isEmpty();
}

QString RiotAuth::sessionSettingsPath() const
{
    return riotDataDir() + QStringLiteral("/RiotGamesPrivateSettings.yaml");
}

void RiotAuth::backupCache(NetworkManager *net, int terminalId, const QString &login,
                           int accountId, int gameId)
{
    if (login.isEmpty()) {
        qWarning() << "[RIOT] cache backup: login пуст";
        return;
    }
    if (!net || net->serverUrl().isEmpty()) {
        qWarning() << "[RIOT] cache backup: serverUrl пуст";
        return;
    }

    // Полный Data blob: yaml + Cookies/Sessions (один yaml ~470b silent не даёт)
    const QString privateYaml = readTextFile(
        riotDataDir() + QStringLiteral("/RiotGamesPrivateSettings.yaml"));
    qWarning() << "[RIOT] cache backup check: yaml chars" << privateYaml.size()
               << "| durable:" << yamlLooksDurableSession(privateYaml)
               << "| persist null:" << privateYaml.contains(QStringLiteral("persist: null"));

    const QString blob = packRiotCacheBlob();
    if (blob.isEmpty() || !riotBlobLooksSilentReady(blob)) {
        qWarning() << "[RIOT] cache backup: Data blob неполный (нужны Cookies/Sessions или durable yaml)"
                   << "| chars:" << blob.size()
                   << "| data:" << riotDataDir();
        // Fallback: хотя бы yaml, чтобы не потерять совсем
        const QString yaml = privateYaml;
        const QString configYaml = readTextFile(
            riotConfigDir() + QStringLiteral("/RiotClientSettings.yaml"));
        const bool yamlOk = looksLikeRiotSessionYaml(yaml) || yamlLooksDurableSession(yaml);
        if (!yamlOk)
            return;
        // Thin yaml (только tdid) не silent-ready: обычно после taskkill /F без галки persist.
        // После soft-close + persistLogin:true yaml должен стать durable.
        if (!yamlLooksDurableSession(yaml)) {
            qWarning() << "[RIOT] cache backup: thin yaml — НЕ пишем в DB"
                       << "| chars:" << yaml.size()
                       << "| Нужны: галка «не выходить» + soft-close flush (не /F сразу)";
            return;
        }
        QJsonObject rootPayload;
        rootPayload.insert(QStringLiteral("login"), login);
        rootPayload.insert(QStringLiteral("terminal_id"), terminalId);
        if (accountId > 0)
            rootPayload.insert(QStringLiteral("account_id"), accountId);
        if (gameId > 0)
            rootPayload.insert(QStringLiteral("game_id"), gameId);
        rootPayload.insert(QStringLiteral("platform"), QStringLiteral("riot"));
        rootPayload.insert(QStringLiteral("local_vdf"), yaml);
        if (!configYaml.trimmed().isEmpty())
            rootPayload.insert(QStringLiteral("config_vdf"), configYaml);
        QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply *reply = net->networkAccessManager()->post(
            request, QJsonDocument(rootPayload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, reply, [reply]() {
            reply->deleteLater();
            qWarning() << "[RIOT] cache backup (yaml-only)"
                       << (reply->error() == QNetworkReply::NoError ? "OK" : reply->errorString());
        });
        return;
    }

    QJsonObject rootPayload;
    rootPayload.insert(QStringLiteral("login"), login);
    rootPayload.insert(QStringLiteral("terminal_id"), terminalId);
    if (accountId > 0)
        rootPayload.insert(QStringLiteral("account_id"), accountId);
    if (gameId > 0)
        rootPayload.insert(QStringLiteral("game_id"), gameId);
    rootPayload.insert(QStringLiteral("platform"), QStringLiteral("riot"));
    rootPayload.insert(QStringLiteral("local_vdf"), blob);

    QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qWarning() << "[RIOT] cache backup → server, bytes:" << jsonData.size()
               << "login:" << login << "account_id:" << accountId << "game_id:" << gameId
               << "terminal:" << terminalId
               << "blob chars:" << blob.size();

    QNetworkReply *reply = net->networkAccessManager()->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            qWarning() << "[RIOT] cache backup OK" << QString::fromUtf8(body.left(200));
        else
            qWarning() << "[RIOT] cache backup fail:" << reply->errorString()
                       << QString::fromUtf8(body.left(200));
    });
}

void RiotAuth::scheduleProductLaunch()
{
    if (m_productLaunchScheduled)
        return;
    m_productLaunchScheduled = true;
    m_productLaunchOk = false;
    m_playClickDone = false;
    m_playClickStop = false;
    m_launchAborted = false;
    m_overlayDismissScheduled = false;
    m_overlayDismissed = false;
    m_sessionPollTicks = 0;
    m_rsoRetryCount = 0;
    qWarning() << "[RIOT] после логина: RSO → protocol/API → UIA Play";
    pollSessionThenLaunchProduct();
}

QNetworkAccessManager *RiotAuth::ensureNam()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::sslErrors,
                this, [](QNetworkReply *reply, const QList<QSslError> &) {
                    if (reply)
                        reply->ignoreSslErrors();
                });
    }
    return m_nam;
}

bool RiotAuth::readRiotLockfile(int *port, QString *password, QString *protocol) const
{
    const QString lockPath =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/Riot Games/Riot Client/Config/lockfile");
    QFile lf(lockPath);
    if (!lf.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QStringList parts = QString::fromUtf8(lf.readAll()).trimmed().split(QLatin1Char(':'));
    lf.close();
    if (parts.size() < 5)
        return false;
    if (port)
        *port = parts.at(2).toInt();
    if (password)
        *password = parts.at(3);
    if (protocol)
        *protocol = parts.at(4).isEmpty() ? QStringLiteral("https") : parts.at(4);
    return port && *port > 0 && password && !password->isEmpty();
}

bool RiotAuth::readLeagueLockfile(int *port, QString *password, QString *protocol) const
{
    // Lockfile League Client (не Riot Client!)
    const QStringList paths = {
        QStringLiteral("C:/Riot Games/League of Legends/lockfile"),
        QStringLiteral("D:/Riot Games/League of Legends/lockfile"),
        QStringLiteral("E:/Riot Games/League of Legends/lockfile"),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/Riot Games/League of Legends/lockfile"),
    };
    for (const QString &lockPath : paths) {
        QFile lf(lockPath);
        if (!lf.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QStringList parts = QString::fromUtf8(lf.readAll()).trimmed().split(QLatin1Char(':'));
        lf.close();
        if (parts.size() < 5)
            continue;
        const int p = parts.at(2).toInt();
        const QString pass = parts.at(3);
        if (p <= 0 || pass.isEmpty())
            continue;
        if (port)
            *port = p;
        if (password)
            *password = pass;
        if (protocol)
            *protocol = parts.at(4).isEmpty() ? QStringLiteral("https") : parts.at(4);
        qWarning().noquote() << "[RIOT] League lockfile:" << lockPath << "port" << p;
        return true;
    }
    return false;
}

void RiotAuth::skipLolTutorialViaLcu(const char *why)
{
    if (m_overlayDismissed)
        return;

    int port = 0;
    QString password;
    QString protocol;
    if (!readLeagueLockfile(&port, &password, &protocol)) {
        qWarning() << "[RIOT] LCU lockfile нет — skip tutorial позже |" << why;
        return;
    }

    const QByteArray auth = QByteArrayLiteral("Basic ")
        + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64();

    auto putJson = [this, port, protocol, auth, why](const QString &path, const QByteArray &body) {
        QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Authorization", auth);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply *reply = ensureNam()->put(req, body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, why, path]() {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray resp = reply->readAll();
            qWarning().noquote() << "[RIOT] LCU PUT" << path << "HTTP" << code
                                 << "| body:" << QString::fromUtf8(resp.left(200))
                                 << "|" << why;
            if (code >= 200 && code < 300)
                m_lcuTutorialSkipDone = true;
            // НЕ ставим m_overlayDismissed: ещё нужен клик верхней «ИГРАТЬ»
            reply->deleteLater();
        });
    };

    auto getPath = [this, port, protocol, auth, why](const QString &path) {
        QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", auth);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply *reply = ensureNam()->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, why, path]() {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            qWarning().noquote() << "[RIOT] LCU GET" << path
                                 << "HTTP" << code
                                 << "| body:" << QString::fromUtf8(body.left(300))
                                 << "|" << why;
            if (path.contains(QStringLiteral("/settings")) && code >= 200 && code < 300) {
                const QString s = QString::fromUtf8(body);
                if (s.contains(QStringLiteral("\"hasSeenTutorialPath\":true"))
                    || s.contains(QStringLiteral("\"hasSkippedTutorialPath\":true"))
                    || s.contains(QStringLiteral("\"shouldSeeNewPlayerExperience\":false"))) {
                    m_lcuTutorialSkipDone = true;
                    qWarning() << "[RIOT] LCU: FTUE settings ok — ждём UI и клик «ИГРАТЬ»";
                }
            }
            reply->deleteLater();
        });
    };

    getPath(QStringLiteral("/lol-npe-tutorial-path/v1/settings"));
    getPath(QStringLiteral("/lol-npe-tutorial-path/v1/tutorials"));

    // Скрыть welcome/FTUE оверлей на аккаунте
    const QByteArray settingsBody = QByteArrayLiteral(
        "{\"hasSeenTutorialPath\":true,\"hasSkippedTutorialPath\":true,"
        "\"shouldSeeNewPlayerExperience\":false}");
    putJson(QStringLiteral("/lol-npe-tutorial-path/v1/settings"), settingsBody);
    m_lcuTutorialSkipDone = true;
}

void RiotAuth::openLeagueLobbyViaLcu(const char *why)
{
    // Новый аккаунт: 430 часто закрыт — сначала intro bots 870
    if (m_lobbyPostAttempts >= 4 || m_leagueLobbyPosted)
        return;
    int port = 0;
    QString password;
    QString protocol;
    if (!readLeagueLockfile(&port, &password, &protocol)) {
        qWarning() << "[RIOT] LCU lobby — нет lockfile |" << why;
        return;
    }

    static const int kQueues[] = {870, 840, 430, 400};
    const int queueId = kQueues[qMin(m_lobbyPostAttempts, 3)];
    ++m_lobbyPostAttempts;

    const QByteArray body =
        QByteArrayLiteral("{\"queueId\":") + QByteArray::number(queueId) + '}';
    QUrl url(QStringLiteral("%1://127.0.0.1:%2/lol-lobby/v2/lobby")
                 .arg(protocol).arg(port));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Basic ")
                         + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64());
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    qWarning() << "[RIOT] LCU POST lobby queueId=" << queueId << "|" << why
               << "try" << m_lobbyPostAttempts;
    QNetworkReply *reply = ensureNam()->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, why, queueId]() {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray resp = reply->readAll();
        qWarning().noquote() << "[RIOT] LCU lobby HTTP" << code
                             << "queue" << queueId
                             << "| body:" << QString::fromUtf8(resp.left(220))
                             << "|" << why;
        if (code >= 200 && code < 300) {
            m_leagueLobbyPosted = true;
            m_overlayDismissed = true;
            qWarning() << "[RIOT] LCU lobby OK — ушли с Пути новичка";
        }
        reply->deleteLater();
    });
}

void RiotAuth::ensureSingleLeagueClient()
{
#ifdef Q_OS_WIN
    struct Item {
        DWORD pid = 0;
        HWND hwnd = nullptr;
        int area = 0;
        bool titled = false;
        bool visible = false;
    };
    QVector<Item> items;

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *list = reinterpret_cast<QVector<Item> *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid)
            return TRUE;
        const QString img = processImageForPid(pid);
        if (!img.contains(QStringLiteral("LeagueClientUx"), Qt::CaseInsensitive))
            return TRUE;
        RECT r{};
        GetWindowRect(hwnd, &r);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        if (w < 800 || h < 450)
            return TRUE;
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 255);
        Item it;
        it.pid = pid;
        it.hwnd = hwnd;
        it.area = w * h;
        it.visible = IsWindowVisible(hwnd);
        it.titled = QString::fromWCharArray(title).contains(QStringLiteral("League"),
                                                            Qt::CaseInsensitive);
        list->push_back(it);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&items));

    // Также все LeagueClientUx.exe из snapshot (даже без большого окна)
    QVector<DWORD> uxPids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                const QString name = QString::fromWCharArray(pe.szExeFile);
                if (name.compare(QStringLiteral("LeagueClientUx.exe"), Qt::CaseInsensitive) == 0)
                    uxPids.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (items.size() <= 1 && uxPids.size() <= 1)
        return;

    Item keep;
    bool haveKeep = false;
    auto better = [](const Item &a, const Item &b) {
        if (a.visible != b.visible)
            return a.visible;
        if (a.titled != b.titled)
            return a.titled;
        return a.area > b.area;
    };
    for (const Item &it : items) {
        if (!haveKeep || better(it, keep)) {
            keep = it;
            haveKeep = true;
        }
    }
    if (!haveKeep && !uxPids.isEmpty())
        keep.pid = uxPids.first();

    QSet<DWORD> killPids;
    for (const Item &it : items) {
        if (keep.pid && it.pid != keep.pid)
            killPids.insert(it.pid);
    }
    for (DWORD pid : uxPids) {
        if (keep.pid && pid != keep.pid)
            killPids.insert(pid);
    }
    for (DWORD pid : killPids) {
        qWarning() << "[RIOT] дубликат LeagueClientUx — kill pid" << qulonglong(pid);
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
    }
#else
    Q_UNUSED(this);
#endif
}

void RiotAuth::postRiotApi(const QString &path, const QByteArray &body, const char *why)
{
    int port = 0;
    QString password;
    QString protocol;
    if (!readRiotLockfile(&port, &password, &protocol)) {
        qWarning() << "[RIOT] lockfile нет для" << path << "|" << why;
        return;
    }

    QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Basic ")
                         + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64());
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    qWarning().noquote() << "[RIOT] API POST" << url.toString()
                         << (body.isEmpty() ? QStringLiteral("{}") : QString::fromUtf8(body))
                         << "|" << why;
    QNetworkReply *reply = ensureNam()->post(req, body.isEmpty() ? QByteArrayLiteral("{}") : body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, why, path]() {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray resp = reply->readAll();
        qWarning().noquote() << "[RIOT] API" << path << "HTTP" << code
                             << "| body:" << QString::fromUtf8(resp.left(250))
                             << "|" << why;
        // 204 на старых endpoints = «принято, но ничего не сделано».
        // На product-launcher/.../launch 2xx считаем успехом запуска.
        if (code >= 200 && code < 300 && path.contains(QStringLiteral("/launch"))) {
            m_productLaunchOk = true;
            qWarning() << "[RIOT] product-launcher OK — ждём процесс игры";
        }
        reply->deleteLater();
    });
}

void RiotAuth::putRiotCredentials(const char *why)
{
    if (m_apiLoginInFlight || m_login.isEmpty() || m_password.isEmpty())
        return;

    int port = 0;
    QString lockPass;
    QString protocol;
    if (!readRiotLockfile(&port, &lockPass, &protocol)) {
        qWarning() << "[RIOT] API login: lockfile ещё нет |" << why;
        return;
    }

    m_apiLoginInFlight = true;
    const int gen = m_launchGen;
    const QByteArray authHdr = QByteArrayLiteral("Basic ")
        + QStringLiteral("riot:%1").arg(lockPass).toUtf8().toBase64();

    // Сначала GET session: не слать пароль пока RSO «not yet initialized»
    QUrl sessionUrl(QStringLiteral("%1://127.0.0.1:%2/rso-auth/v1/session")
                        .arg(protocol).arg(port));
    QNetworkRequest sessionReq(sessionUrl);
    sessionReq.setRawHeader("Authorization", authHdr);
    sessionReq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply *sessionReply = ensureNam()->get(sessionReq);
    connect(sessionReply, &QNetworkReply::finished, this,
            [this, sessionReply, why, gen, protocol, port, authHdr]() {
        const int code = sessionReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = sessionReply->readAll();
        sessionReply->deleteLater();
        if (gen != m_launchGen || m_launchAborted) {
            m_apiLoginInFlight = false;
            return;
        }

        const QByteArray lower = body.toLower();
        qWarning().noquote() << "[RIOT] API pre-login session HTTP" << code
                             << "| body:" << QString::fromUtf8(body.left(220))
                             << "|" << why;

        if (lower.contains("not yet initialized") || code == 0) {
            m_apiLoginInFlight = false;
            qWarning() << "[RIOT] API login: RSO ещё init — повторим позже";
            return;
        }

        if (code >= 200 && code < 300 && lower.contains("authenticated")
            && !lower.contains("auth_failure")
            && !lower.contains("needs_credentials")) {
            m_apiLoginInFlight = false;
            m_apiLoginAttempted = true;
            m_credentialsSent = true;
            m_allowsGameDetect = true;
            m_phase = Phase::Done;
            qWarning() << "[RIOT] уже authenticated | persistLogin:"
                       << lower.contains("\"persistlogin\":true");
            if (m_scoutTimer) {
                m_scoutTimer->stop();
                m_scoutTimer->deleteLater();
                m_scoutTimer = nullptr;
            }
            scheduleProductLaunch();
            return;
        }

        QJsonObject cred;
        cred.insert(QStringLiteral("username"), m_login);
        cred.insert(QStringLiteral("password"), m_password);
        cred.insert(QStringLiteral("persistLogin"), true);
        const QByteArray json = QJsonDocument(cred).toJson(QJsonDocument::Compact);

        QUrl url(QStringLiteral("%1://127.0.0.1:%2/rso-auth/v1/session/credentials")
                     .arg(protocol).arg(port));
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Authorization", authHdr);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

        qWarning() << "[RIOT] PUT credentials persistLogin=true |" << why
                   << "| user:" << m_login;
        m_apiLoginAttempted = true;

        QNetworkReply *reply = ensureNam()->sendCustomRequest(req, QByteArrayLiteral("PUT"), json);
        connect(reply, &QNetworkReply::finished, this, [this, reply, why, gen]() {
            m_apiLoginInFlight = false;
            const int c = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray resp = reply->readAll();
            reply->deleteLater();
            if (gen != m_launchGen || m_launchAborted)
                return;

            const QByteArray rl = resp.toLower();
            qWarning().noquote() << "[RIOT] credentials HTTP" << c
                                 << "| body:" << QString::fromUtf8(resp.left(400));

            const bool ok = (c >= 200 && c < 300)
                && rl.contains("authenticated")
                && !rl.contains("auth_failure")
                && !rl.contains("needs_credentials");
            const bool persist = rl.contains("\"persistlogin\":true");

            if (ok) {
                m_credentialsSent = true;
                m_allowsGameDetect = true;
                m_phase = Phase::Done;
                qWarning() << "[RIOT] API login OK | persistLogin:" << persist;
                if (!persist)
                    qWarning() << "[RIOT] WARN: persistLogin false — silent next run маловероятен";
                if (m_scoutTimer) {
                    m_scoutTimer->stop();
                    m_scoutTimer->deleteLater();
                    m_scoutTimer = nullptr;
                }
                scheduleProductLaunch();
                return;
            }

            qWarning() << "[RIOT] API login fail → type password |" << why;
            m_phase = Phase::TypePassword;
            m_phaseTick = m_ticks;
        });
    });
}

void RiotAuth::tryEnablePersistLogin(const char *why)
{
    if (m_persistEnableAttempted || m_apiLoginInFlight)
        return;

    int port = 0;
    QString lockPass;
    QString protocol;
    if (!readRiotLockfile(&port, &lockPass, &protocol)) {
        qWarning() << "[RIOT] persist API: lockfile нет |" << why;
        return;
    }

    m_persistEnableAttempted = true;
    const int gen = m_launchGen;
    const QByteArray authHdr = QByteArrayLiteral("Basic ")
        + QStringLiteral("riot:%1").arg(lockPass).toUtf8().toBase64();

    QUrl sessionUrl(QStringLiteral("%1://127.0.0.1:%2/rso-auth/v1/session")
                        .arg(protocol).arg(port));
    QNetworkRequest sessionReq(sessionUrl);
    sessionReq.setRawHeader("Authorization", authHdr);
    sessionReq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    qWarning() << "[RIOT] GET session — проверка persistLogin |" << why;
    QNetworkReply *sessionReply = ensureNam()->get(sessionReq);
    connect(sessionReply, &QNetworkReply::finished, this,
            [this, sessionReply, why, gen, protocol, port, authHdr]() {
        const int code = sessionReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = sessionReply->readAll();
        sessionReply->deleteLater();
        if (gen != m_launchGen || m_launchAborted)
            return;

        const QByteArray lower = body.toLower();
        const bool authenticated = code >= 200 && code < 300
            && lower.contains("authenticated")
            && !lower.contains("auth_failure")
            && !lower.contains("needs_credentials");
        const bool persistOn = lower.contains("\"persistlogin\":true");

        qWarning().noquote() << "[RIOT] persist check HTTP" << code
                             << "| persistLogin:" << persistOn
                             << "| body:" << QString::fromUtf8(body.left(220))
                             << "|" << why;

        if (!authenticated) {
            qWarning() << "[RIOT] persist API: не authenticated — skip";
            return;
        }
        if (persistOn) {
            qWarning() << "[RIOT] persistLogin уже true — backup когда CEF допишется";
            m_needBackup = true;
            return;
        }

        // НЕ зовём PUT /session (405) и НЕ PUT /credentials после UI-логина:
        // повторный credentials даёт auth_failure и сносит живую RSO-сессию →
        // «Не удалось запустить League of Legends». Persist только через UI-галку.
        qWarning() << "[RIOT] persistLogin false после UI — API post-auth SKIP"
                   << "(credentials ломает session). Ждём CEF от галки «не выходить» |"
                   << why;
        m_needBackup = true;
        QTimer::singleShot(8000, this, [this, gen]() {
            if (gen != m_launchGen || m_launchAborted)
                return;
            m_needBackup = true;
            qWarning() << "[RIOT] persist flush — needBackup для отложенного cache";
        });
    });
}

bool RiotAuth::ensurePersistCheckbox(quintptr hwndVal)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return false;

    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void **>(&automation)))
        || !automation)
        return false;

    IUIAutomationElement *root = nullptr;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
        automation->Release();
        return false;
    }

    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (!trueCond || FAILED(root->FindAll(TreeScope_Descendants, trueCond, &all)) || !all) {
        if (trueCond) trueCond->Release();
        root->Release();
        automation->Release();
        return false;
    }
    trueCond->Release();

    int len = 0;
    all->get_Length(&len);
    bool toggled = false;
    QStringList checkboxDump;
    struct PersistCand {
        IUIAutomationElement *el = nullptr;
        QString name;
        bool looksPersist = false;
    };
    QVector<PersistCand> checks;

    for (int i = 0; i < len; ++i) {
        IUIAutomationElement *el = nullptr;
        all->GetElement(i, &el);
        if (!el)
            continue;

        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name).trimmed() : QString();
        if (name)
            SysFreeString(name);

        const bool looksPersist =
            n.contains(QStringLiteral("Оставаться"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("системе"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Не выходить"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("не выходить"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("выходить"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Stay signed"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Remember"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Запомнить"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("signed in"), Qt::CaseInsensitive);

        if (ct == UIA_CheckBoxControlTypeId) {
            checkboxDump << (n.isEmpty() ? QStringLiteral("(empty)") : n);
            PersistCand c;
            c.el = el;
            c.name = n;
            c.looksPersist = looksPersist;
            checks.append(c);
            continue; // release later
        }

        // Текст рядом с галкой (не CheckBox) — тоже кандидат
        if (looksPersist) {
            PersistCand c;
            c.el = el;
            c.name = n;
            c.looksPersist = true;
            checks.append(c);
            continue;
        }
        el->Release();
    }

    if (!checkboxDump.isEmpty())
        qWarning().noquote() << "[RIOT] UIA CheckBoxes:" << checkboxDump.join(QStringLiteral(" | "));

    auto tryToggle = [&](IUIAutomationElement *el, const QString &n) -> bool {
        IUnknown *patUnk = nullptr;
        if (FAILED(el->GetCurrentPattern(UIA_TogglePatternId, &patUnk)) || !patUnk)
            return false;
        IUIAutomationTogglePattern *toggle = nullptr;
        if (FAILED(patUnk->QueryInterface(IID_IUIAutomationTogglePattern,
                                          reinterpret_cast<void **>(&toggle)))
            || !toggle) {
            patUnk->Release();
            return false;
        }
        ToggleState st = ToggleState_Off;
        toggle->get_CurrentToggleState(&st);
        if (st != ToggleState_On) {
            toggle->Toggle();
            qWarning().noquote() << "[RIOT] UIA Toggle persist ON:\"" << n << "\"";
        } else {
            qWarning().noquote() << "[RIOT] UIA persist уже ON:\"" << n << "\"";
        }
        toggle->Release();
        patUnk->Release();
        return true;
    };

    // 1) Именной persist
    for (PersistCand &c : checks) {
        if (toggled)
            break;
        if (!c.looksPersist)
            continue;
        if (tryToggle(c.el, c.name))
            toggled = true;
        else
            qWarning().noquote() << "[RIOT] persist candidate без Toggle:\"" << c.name << "\"";
    }
    // 2) На форме логина часто одна галка без имени — Toggle её
    if (!toggled) {
        int boxCount = 0;
        for (const PersistCand &c : checks) {
            CONTROLTYPEID ct = 0;
            c.el->get_CurrentControlType(&ct);
            if (ct == UIA_CheckBoxControlTypeId)
                ++boxCount;
        }
        if (boxCount == 1) {
            for (PersistCand &c : checks) {
                CONTROLTYPEID ct = 0;
                c.el->get_CurrentControlType(&ct);
                if (ct != UIA_CheckBoxControlTypeId)
                    continue;
                if (tryToggle(c.el, c.name.isEmpty() ? QStringLiteral("(lone checkbox)") : c.name))
                    toggled = true;
                break;
            }
        }
    }

    for (PersistCand &c : checks)
        c.el->Release();

    all->Release();
    root->Release();
    automation->Release();
    return toggled;
#else
    Q_UNUSED(hwndVal);
    return false;
#endif
}

void RiotAuth::pollSessionThenLaunchProduct()
{
    if (m_sessionPollTimer) {
        m_sessionPollTimer->stop();
        m_sessionPollTimer->deleteLater();
        m_sessionPollTimer = nullptr;
    }

    m_sessionPollTimer = new QTimer(this);
    m_sessionPollTimer->setInterval(500);
    connect(m_sessionPollTimer, &QTimer::timeout, this, [this]() {
        ++m_sessionPollTicks;
        if (isGameProcessRunning()) {
            qWarning() << "[RIOT] игра уже есть — launch не нужен";
            m_productLaunchOk = true;
            notifyProductReady();
            m_sessionPollTimer->stop();
            m_sessionPollTimer->deleteLater();
            m_sessionPollTimer = nullptr;
            return;
        }

        const QFileInfo fi(sessionSettingsPath());
        const bool yamlReady = fi.exists() && fi.size() > 80;
        // Также пробуем по таймеру: RSO иногда готов раньше yaml
        const bool timeReady = m_sessionPollTicks >= 10; // ~5s после Enter
        if (!yamlReady && !timeReady) {
            if (m_sessionPollTicks > 60) {
                qWarning() << "[RIOT] timeout ожидания сессии — launch всё равно";
                m_sessionPollTimer->stop();
                m_sessionPollTimer->deleteLater();
                m_sessionPollTimer = nullptr;
                launchProductViaApi("timeout");
            }
            return;
        }

        if (yamlReady)
            qWarning() << "[RIOT] session yaml готов, size" << fi.size();
        else
            qWarning() << "[RIOT] yaml ещё нет, но прошло ~5s — проверяем RSO и launch";

        m_sessionPollTimer->stop();
        m_sessionPollTimer->deleteLater();
        m_sessionPollTimer = nullptr;

        QTimer::singleShot(2000, this, [this]() {
            if (isGameProcessRunning()) {
                m_productLaunchOk = true;
                qWarning() << "[RIOT] игра стартовала сама — OK";
                notifyProductReady();
                return;
            }
            // RSO check → UIA «Играть» (прямой LeagueClient.exe Windows блокирует)
            launchProductViaApi("after RSO ready");
        });
    });
    m_sessionPollTimer->start();
}

void RiotAuth::finishLaunchNudge(const char *why)
{
    if (isGameProcessRunning()) {
        qWarning() << "[RIOT] процесс игры уже есть |" << why;
        m_productLaunchOk = true;
        m_playClickStop = true;
        ensureSingleLeagueClient();
        notifyProductReady();
        return;
    }
    if (m_playClickDone) {
        qWarning() << "[RIOT] UIA Play уже делали |" << why;
        return;
    }
    m_playClickDone = true;

    // Ждём готовую кнопку «Играть» (UIA). Без heuristic и без forceRetry после клика.
    qWarning() << "[RIOT] ждём UIA «Играть» (макс. 1 клик) |" << why;

    auto onGameUp = [this]() {
        m_playClickStop = true;
        m_productLaunchOk = true;
        ensureSingleLeagueClient();
        notifyProductReady();
        QTimer::singleShot(3000, this, [this]() {
            if (!m_launchAborted)
                ensureSingleLeagueClient();
        });
        QTimer::singleShot(8000, this, [this]() {
            if (!m_launchAborted)
                ensureSingleLeagueClient();
        });
    };

    auto tryPlay = [this, onGameUp](const char *tag) {
        if (m_launchAborted)
            return;
        if (isGameProcessRunning()) {
            qWarning() << "[RIOT] процесс уже есть — Play не жмём |" << tag;
            onGameUp();
            return;
        }
        if (m_playClickStop)
            return;
        qWarning() << "[RIOT] клик Play |" << tag;
        clickPlayButton(tag);
    };

    // С --launch-product НЕ жмём Play рано: двойной старт League → vgc падает (VAN 216).
    restoreRiotOnscreen("finishLaunchNudge");
    keepShellOverlayUp();
    const bool hasProductLaunch =
        m_productArgs.contains(QStringLiteral("--launch-product"), Qt::CaseInsensitive);
    if (hasProductLaunch) {
        qWarning() << "[RIOT] есть --launch-product → ждём League сами, Play только если не поднялся";
        QTimer::singleShot(12000, this, [tryPlay]() { tryPlay("Play @12s (product wait)"); });
        QTimer::singleShot(20000, this, [tryPlay]() { tryPlay("Play @20s (product wait)"); });
        QTimer::singleShot(30000, this, [tryPlay]() { tryPlay("Play @30s (product wait)"); });
        for (int ms : {5000, 10000, 15000, 22000, 28000, 36000}) {
            QTimer::singleShot(ms, this, [this, onGameUp]() {
                if (m_launchAborted)
                    return;
                if (!isGameProcessRunning()) {
                    restoreRiotOnscreen("poll keep on-screen");
                    keepShellOverlayUp();
                }
                dismissAccessDeniedDialog("poll");
                dismissAlreadyRunningDialog("poll");
                if (isGameProcessRunning())
                    onGameUp();
            });
        }
        QTimer::singleShot(42000, this, [this, onGameUp]() {
            if (m_launchAborted)
                return;
            if (isGameProcessRunning()) {
                qWarning() << "[RIOT] LeagueClient поднялся";
                onGameUp();
            } else {
                qWarning() << "[RIOT] LeagueClient не поднялся после ожидания product/Play";
            }
        });
    } else {
        QTimer::singleShot(5000, this, [tryPlay]() { tryPlay("Play @5s"); });
        QTimer::singleShot(8000, this, [tryPlay]() { tryPlay("Play @8s"); });
        QTimer::singleShot(12000, this, [tryPlay]() { tryPlay("Play @12s"); });
        QTimer::singleShot(16000, this, [tryPlay]() { tryPlay("Play @16s"); });
        QTimer::singleShot(20000, this, [tryPlay]() { tryPlay("Play @20s"); });
        for (int ms : {4000, 7000, 10000, 14000, 18000}) {
            QTimer::singleShot(ms, this, [this, onGameUp]() {
                if (m_launchAborted)
                    return;
                if (!isGameProcessRunning()) {
                    restoreRiotOnscreen("poll keep on-screen");
                    keepShellOverlayUp();
                }
                dismissAccessDeniedDialog("poll");
                dismissAlreadyRunningDialog("poll");
                if (isGameProcessRunning())
                    onGameUp();
            });
        }
        QTimer::singleShot(22000, this, [this, onGameUp]() {
            if (m_launchAborted)
                return;
            if (isGameProcessRunning()) {
                qWarning() << "[RIOT] LeagueClient поднялся";
                onGameUp();
            } else {
                qWarning() << "[RIOT] LeagueClient не поднялся после ожидания «Играть»";
            }
        });
    }
}

void RiotAuth::discoverRiotLaunchPaths(const char *why)
{
    int port = 0;
    QString password;
    QString protocol;
    if (!readRiotLockfile(&port, &password, &protocol))
        return;

    const QByteArray auth = QByteArrayLiteral("Basic ")
        + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64();

    auto getPath = [this, port, protocol, auth, why](const QString &path) {
        QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", auth);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply *reply = ensureNam()->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, why, path]() {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            reply->deleteLater();
            if (code < 200 || code >= 300) {
                qWarning() << "[RIOT] discover" << path << "HTTP" << code;
                return;
            }
            // Вытащим пути с launch / product-launcher
            QStringList found;
            const QString text = QString::fromUtf8(body);
            QRegularExpression re(QStringLiteral("\"(/[^\"]*(?:launch|product-launcher|patchline)[^\"]*)\""));
            auto it = re.globalMatch(text);
            while (it.hasNext() && found.size() < 40) {
                const QString p = it.next().captured(1);
                if (!found.contains(p))
                    found << p;
            }
            qWarning() << "[RIOT] discover" << path << "→" << found.size() << "paths |" << why;
            for (const QString &p : found)
                qWarning().noquote() << "[RIOT]   path:" << p;

            Q_UNUSED(why);
            // Только лог — POST в launch-restriction даёт 405 и ничего не запускает
        });
    };

    // Один раз swagger для диагностики; авто-POST отключён
    getPath(QStringLiteral("/swagger/v3/openapi.json"));

    // Проверим, не блокирует ли Riot запуск
    QString productId, patchline;
    parseProductArgs(&productId, &patchline);
    auto getOnly = [this, port, protocol, auth, why](const QString &path) {
        QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", auth);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply *reply = ensureNam()->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, why, path]() {
            qWarning().noquote() << "[RIOT] GET" << path
                                 << "HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                                 << "| body:" << QString::fromUtf8(reply->readAll().left(250))
                                 << "|" << why;
            reply->deleteLater();
        });
    };
    getOnly(QStringLiteral("/launch-restriction/v1/ready"));
    getOnly(QStringLiteral("/launch-restriction/v1/restrictions/%1").arg(productId));
}

QString RiotAuth::resolveGameExePath() const
{
    QString productId;
    QString patchline;
    parseProductArgs(&productId, &patchline);
    if (productId.contains(QStringLiteral("valorant"), Qt::CaseInsensitive)) {
        const QStringList candidates = {
            QStringLiteral("C:/Riot Games/VALORANT/live/VALORANT.exe"),
            QStringLiteral("D:/Riot Games/VALORANT/live/VALORANT.exe"),
        };
        for (const QString &c : candidates) {
            if (QFileInfo::exists(c))
                return c;
        }
        return {};
    }
    const QStringList candidates = {
        QStringLiteral("C:/Riot Games/League of Legends/LeagueClient.exe"),
        QStringLiteral("D:/Riot Games/League of Legends/LeagueClient.exe"),
        QStringLiteral("E:/Riot Games/League of Legends/LeagueClient.exe"),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return {};
}

void RiotAuth::launchGameExeDirect(const char *why)
{
    // Windows/Vanguard: «нет доступа» к LeagueClient.exe из шелла/explorer.
    // Не вызываем — только спамим диалог ошибки.
    qWarning() << "[RIOT] skip direct LeagueClient — Access Denied на этой машине |" << why;
}

#ifdef Q_OS_WIN
static void riotClickScreen(int x, int y, const char *why, bool doubleClick = false)
{
    // Absolute coords — надёжнее SetCursorPos+relative на multi-DPI
    const int sx = qMax(1, GetSystemMetrics(SM_CXSCREEN));
    const int sy = qMax(1, GetSystemMetrics(SM_CYSCREEN));
    const LONG ax = (x * 65535L) / sx;
    const LONG ay = (y * 65535L) / sy;

    SetCursorPos(x, y);
    Sleep(50);

    INPUT in[3] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dx = ax;
    in[0].mi.dy = ay;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[2].type = INPUT_MOUSE;
    in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(3, in, sizeof(INPUT));
    if (doubleClick) {
        Sleep(60);
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, in + 1, sizeof(INPUT));
        qWarning() << "[RIOT] mouse dbl-click" << x << y << "|" << why;
    } else {
        qWarning() << "[RIOT] mouse click" << x << y << "|" << why;
    }
}

// Riot Client v135 RU tab-order from password (automation scout):
// After UIA typing, checkbox is Tab×4 (not Tab×5 — that is Submit without persist).
// Persist: Tab×4 = «Не выходить», Space toggles. Then Tab×1 → Enter on Submit.
static bool riotNameLooksPlay(const QString &raw)
{
    const QString n = raw.trimmed().toLower();
    if (n.isEmpty())
        return false;
    return n == QStringLiteral("играть")
        || n == QStringLiteral("play")
        || n.contains(QStringLiteral("играть"))
        || (n.contains(QStringLiteral("play")) && !n.contains(QStringLiteral("display")));
}

static int riotRectArea(const RECT &r)
{
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return 0;
    return w * h;
}

static bool riotInvokeElement(IUIAutomationElement *el, const char *why)
{
    if (!el)
        return false;
    IUnknown *patUnk = nullptr;
    if (FAILED(el->GetCurrentPattern(UIA_InvokePatternId, &patUnk)) || !patUnk)
        return false;
    IUIAutomationInvokePattern *invoke = nullptr;
    bool ok = false;
    if (SUCCEEDED(patUnk->QueryInterface(IID_IUIAutomationInvokePattern,
                                         reinterpret_cast<void **>(&invoke)))
        && invoke) {
        const HRESULT hr = invoke->Invoke();
        ok = SUCCEEDED(hr);
        qWarning() << "[RIOT] UIA Invoke «Играть» hr" << Qt::hex << quint32(hr)
                   << "| ok:" << ok << "|" << why;
        invoke->Release();
    }
    patUnk->Release();
    return ok;
}

static void riotForceForeground(HWND hwnd); // defined below
#endif

bool RiotAuth::clickPlayButton(const char *why)
{
#ifdef Q_OS_WIN
    if (m_launchAborted || m_playClickStop || isGameProcessRunning()) {
        m_playClickStop = true;
        qWarning() << "[RIOT] Play click skip — abort/стоп/игра |" << why;
        return false;
    }

    HWND hwnd = FindWindowW(nullptr, L"Riot Client");
    if (!hwnd || !IsWindow(hwnd))
        hwnd = reinterpret_cast<HWND>(m_loginHwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        qWarning() << "[RIOT] UIA Play — нет HWND |" << why;
        return false;
    }

    // CEF/UIA за экраном почти пустые (desc≈6). Нужен on-screen под оверлеем.
    restoreRiotOnscreen("before Play UIA");
    keepShellOverlayUp();
    Sleep(400);

    // Не снимаем topmost: мышь попадёт в оверлей. Сначала UIA Invoke под оверлеем.
    // ElementFromHandle + Invoke работают без показа окна поверх.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        qWarning() << "[RIOT] UIA CoCreateInstance fail" << Qt::hex << quint32(hr);
        return false;
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        qWarning() << "[RIOT] UIA ElementFromHandle fail";
        automation->Release();
        return false;
    }

    IUIAutomationElement *best = nullptr;
    int bestScore = 0;
    POINT bestClick{};
    bool haveClick = false;
    QString bestName;

    auto consider = [&](IUIAutomationElement *el) {
        if (!el)
            return;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name) : QString();
        if (name)
            SysFreeString(name);
        const QString nt = n.trimmed();
        if (!riotNameLooksPlay(n)) {
            el->Release();
            return;
        }

        BOOL off = FALSE;
        el->get_CurrentIsOffscreen(&off);
        RECT r{};
        el->get_CurrentBoundingRectangle(&r);
        const int area = riotRectArea(r);

        POINT cp{};
        BOOL gotCp = FALSE;
        el->GetClickablePoint(&cp, &gotCp);

        qWarning().noquote() << "[RIOT] UIA play-cand:\"" << nt << "\" area" << area
                             << "off" << bool(off)
                             << "rect" << r.left << r.top
                             << (r.right - r.left) << "x" << (r.bottom - r.top)
                             << "clickable" << bool(gotCp) << cp.x << cp.y;

        if (off || area < 200) {
            el->Release();
            return;
        }

        // Точное «Играть» + крупная жёлтая кнопка (~200×60)
        int score = area;
        const QString nl = nt.toLower();
        if (nl == QStringLiteral("играть") || nl == QStringLiteral("play"))
            score += 500000;
        if ((r.right - r.left) >= 150 && (r.bottom - r.top) >= 40)
            score += 100000;

        if (score >= bestScore) {
            if (best)
                best->Release();
            best = el;
            bestScore = score;
            bestName = nt;
            if (gotCp) {
                bestClick = cp;
                haveClick = true;
            } else {
                bestClick.x = (r.left + r.right) / 2;
                bestClick.y = (r.top + r.bottom) / 2;
                haveClick = true;
            }
            return;
        }
        el->Release();
    };

    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (trueCond)
        root->FindAll(TreeScope_Descendants, trueCond, &all);
    int len = 0;
    if (all)
        all->get_Length(&len);
    qWarning() << "[RIOT] UIA descendants:" << len << "|" << why;
    const int limit = qMin(len, 400);
    for (int i = 0; i < limit; ++i) {
        IUIAutomationElement *el = nullptr;
        all->GetElement(i, &el);
        if (!el)
            continue;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name) : QString();
        if (name)
            SysFreeString(name);
        if (!riotNameLooksPlay(n)) {
            el->Release();
            continue;
        }
        consider(el);
    }
    if (all)
        all->Release();
    if (trueCond)
        trueCond->Release();

    bool clicked = false;
    if (best && haveClick && bestScore >= 100000) {
        qWarning().noquote() << "[RIOT] UIA Play BEST:\"" << bestName << "\" @"
                             << bestClick.x << bestClick.y
                             << "score" << bestScore << "|" << why;
        // Под оверлеем: только Invoke. Мышь — лишь если Invoke не сработал (кратко снять topmost).
        if (riotInvokeElement(best, why)) {
            clicked = true;
        } else {
            // Мышь нуждается в on-screen rect — кратко возвращаем окно
            restoreRiotOnscreen("Play mouse fallback");
            Sleep(120);
            POINT cp = bestClick;
            RECT r{};
            if (SUCCEEDED(best->get_CurrentBoundingRectangle(&r)) && riotRectArea(r) > 200) {
                cp.x = (r.left + r.right) / 2;
                cp.y = (r.top + r.bottom) / 2;
            }
            setShellTopmostFrom(this, false);
            riotForceForeground(hwnd);
            Sleep(80);
            riotClickScreen(cp.x, cp.y, why, false);
            clicked = true;
            keepShellOverlayUp();
        }
        best->Release();
        best = nullptr;
    } else {
        if (best) {
            best->Release();
            best = nullptr;
        }
        // НЕ heuristic: ранний клик в пустой UI + retry = два клиента
        qWarning() << "[RIOT] UIA Play ещё нет (desc" << len << "score" << bestScore
                   << ") — ждём кнопку, без heuristic |" << why;
    }

    root->Release();
    automation->Release();
    // Один успешный клик — больше не жмём (иначе второй клиент)
    if (clicked)
        m_playClickStop = true;
    keepOverlayOverRiot();
    return clicked;
#else
    Q_UNUSED(why);
    return false;
#endif
}

#ifdef Q_OS_WIN
static HWND findLeagueClientHwnd()
{
    struct Cand {
        HWND hwnd = nullptr;
        int area = 0;
        QString title;
        QString img;
    };
    struct Ctx {
        Cand best;
        int logged = 0;
    } ctx;

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid)
            return TRUE;
        const QString img = processImageForPid(pid);
        if (!img.contains(QStringLiteral("LeagueClient"), Qt::CaseInsensitive))
            return TRUE;

        RECT r{};
        GetWindowRect(hwnd, &r);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        // Splash 512x216 не трогаем
        if (w < 800 || h < 450)
            return TRUE;

        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 255);
        const QString t = QString::fromWCharArray(title);
        const bool visible = IsWindowVisible(hwnd);
        int score = w * h;
        if (t.contains(QStringLiteral("League"), Qt::CaseInsensitive))
            score += 5000000;
        if (img.contains(QStringLiteral("LeagueClientUx"), Qt::CaseInsensitive))
            score += 2000000;
        if (visible)
            score += 1000000;
        else
            return TRUE; // невидимые HWND не берём — иначе ложный accept + game closed

        if (c->logged < 8) {
            qWarning().noquote() << "[RIOT] League HWND cand:" << t
                                 << img << w << "x" << h
                                 << "vis" << visible << "score" << score;
            ++c->logged;
        }

        if (score > c->best.area) {
            c->best.hwnd = hwnd;
            c->best.area = score;
            c->best.title = t;
            c->best.img = img;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.best.hwnd) {
        qWarning().noquote() << "[RIOT] League HWND pick:\"" << ctx.best.title << "\""
                             << ctx.best.img << "score" << ctx.best.area;
        return ctx.best.hwnd;
    }

    HWND byTitle = FindWindowW(nullptr, L"League of Legends");
    if (byTitle && IsWindow(byTitle))
        return byTitle;
    return nullptr;
}

static bool riotNameLooksStart(const QString &raw)
{
    const QString n = raw.trimmed().toLower();
    if (n.isEmpty())
        return false;
    return n == QStringLiteral("начать")
        || n == QStringLiteral("start")
        || n.contains(QStringLiteral("начать"))
        || n.contains(QStringLiteral("продолжить"))
        || (n.contains(QStringLiteral("continue")) && !n.contains(QStringLiteral("account")));
}

static HWND findLeagueRenderHwnd(HWND root)
{
    if (!root)
        return nullptr;
    struct Ctx {
        HWND best = nullptr;
        int bestArea = 0;
    } ctx;
    EnumChildWindows(root, [](HWND child, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        wchar_t cls[256] = {};
        GetClassNameW(child, cls, 255);
        const QString cn = QString::fromWCharArray(cls);
        if (!cn.contains(QStringLiteral("Chrome_RenderWidgetHostHWND"), Qt::CaseInsensitive)
            && !cn.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
            && !cn.contains(QStringLiteral("Intermediate D3D"), Qt::CaseInsensitive))
            return TRUE;
        RECT r{};
        GetClientRect(child, &r);
        const int area = (r.right - r.left) * (r.bottom - r.top);
        if (area > c->bestArea) {
            c->bestArea = area;
            c->best = child;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best ? ctx.best : root;
}

static void riotForceForeground(HWND hwnd)
{
    if (!hwnd)
        return;
    AllowSetForegroundWindow(ASFW_ANY);
    HWND fg = GetForegroundWindow();
    DWORD pidFg = 0, pidThis = GetCurrentProcessId();
    DWORD tidFg = fg ? GetWindowThreadProcessId(fg, &pidFg) : 0;
    DWORD tidThis = GetCurrentThreadId();
    if (tidFg && tidFg != tidThis)
        AttachThreadInput(tidThis, tidFg, TRUE);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (tidFg && tidFg != tidThis)
        AttachThreadInput(tidThis, tidFg, FALSE);
}
#endif

void RiotAuth::notifyProductReady()
{
    // Аккаунт уже прошёл NPE: не кликаем туториал/хедер — только передаём управление.
    if (m_overlayDismissScheduled)
        return;
    m_overlayDismissScheduled = true;
    m_playClickStop = true;
    m_productLaunchOk = true;
    m_allowsGameDetect = true;

    const bool isValorant = m_gameTitle.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive);
    if (!isValorant)
        ensureSingleLeagueClient();

    qWarning() << "[RIOT] продукт готов — ждём окно и передаём управление игроку"
               << (isValorant ? "(Valorant)" : "(League)");

    keepOverlayOverRiot();

    if (isValorant) {
        // ProcessManager::pollForGameWindow найдёт Unreal fullscreen
        m_overlayDismissed = true;
        return;
    }

    auto tryAccept = [this]() {
        if (m_launchAborted)
            return;
        if (m_overlayDismissed)
            return;
#ifdef Q_OS_WIN
        HWND hwnd = findLeagueClientHwnd();
        if (!hwnd || !IsWindowVisible(hwnd))
            return;
        m_overlayDismissed = true;
        char cls[256] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        qWarning() << "[RIOT] League Client готов — передаём управление игроку";
        if (auto *pm = qobject_cast<ProcessManager *>(parent()))
            pm->onGameWindowFound(reinterpret_cast<quintptr>(hwnd),
                                  QString::fromLatin1(cls));
#else
        m_overlayDismissed = true;
#endif
    };

    for (int ms : {1000, 3000, 5000, 8000, 12000, 18000})
        QTimer::singleShot(ms, this, tryAccept);
}

void RiotAuth::dismissAccessDeniedDialog(const char *why)
{
#ifdef Q_OS_WIN
    // Windows: «не удается получить доступ» — заголовок = путь к LeagueClient.exe
    struct Ctx {
        HWND hwnd = nullptr;
    } ctx;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (!IsWindowVisible(hwnd))
            return TRUE;
        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, 511);
        const QString t = QString::fromWCharArray(title);
        if (t.contains(QStringLiteral("LeagueClient.exe"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("League of Legends\\LeagueClient"), Qt::CaseInsensitive)) {
            c->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.hwnd)
        return;

    qWarning() << "[RIOT] закрываем Access Denied на LeagueClient |" << why;
    m_playClickStop = true; // больше не провоцируем повторный запуск
    AllowSetForegroundWindow(ASFW_ANY);
    SetForegroundWindow(ctx.hwnd);
    // Enter / OK
    PostMessageW(ctx.hwnd, WM_CLOSE, 0, 0);
    Sleep(50);
    sendReturnKey();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::dismissAlreadyRunningDialog(const char *why)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_loginHwnd);
    if (!hwnd || !IsWindow(hwnd))
        hwnd = FindWindowW(nullptr, L"Riot Client");
    if (!hwnd || !IsWindow(hwnd))
        return;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void **>(&automation)))
        || !automation)
        return;

    IUIAutomationElement *root = nullptr;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
        automation->Release();
        return;
    }

    // Только «Понятно» / Got it — НЕ «OK»: иначе кликаем ошибку
    // «Не удалось запустить League of Legends» и спамим poll.
    const wchar_t *names[] = {L"Понятно", L"Got it"};
    IUIAutomationElement *btn = nullptr;
    for (const wchar_t *nm : names) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(nm);
        IUIAutomationCondition *cond = nullptr;
        automation->CreatePropertyCondition(UIA_NamePropertyId, v, &cond);
        if (cond) {
            root->FindFirst(TreeScope_Descendants, cond, &btn);
            cond->Release();
        }
        VariantClear(&v);
        if (btn)
            break;
    }

    if (btn) {
        RECT r{};
        btn->get_CurrentBoundingRectangle(&r);
        if (r.right > r.left && (r.right - r.left) * (r.bottom - r.top) >= 80) {
            const int cx = (r.left + r.right) / 2;
            const int cy = (r.top + r.bottom) / 2;
            AllowSetForegroundWindow(ASFW_ANY);
            SetForegroundWindow(hwnd);
            riotClickScreen(cx, cy, "already-running Понятно");
            qWarning() << "[RIOT] закрыли диалог «уже запущена» |" << why;
            m_playClickStop = true; // больше не жмём Play
        }
        btn->Release();
    }

    // Ошибка запуска LoL (OK) — стоп Play, не кликаем OK в цикле
    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (trueCond && SUCCEEDED(root->FindAll(TreeScope_Descendants, trueCond, &all)) && all) {
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement *el = nullptr;
            all->GetElement(i, &el);
            if (!el)
                continue;
            BSTR name = nullptr;
            el->get_CurrentName(&name);
            const QString nm = name ? QString::fromWCharArray(name) : QString();
            if (name)
                SysFreeString(name);
            el->Release();
            if (nm.contains(QStringLiteral("Не удалось запустить"), Qt::CaseInsensitive)
                || nm.contains(QStringLiteral("Failed to launch"), Qt::CaseInsensitive)
                || nm.contains(QStringLiteral("произошло нечто необычное"), Qt::CaseInsensitive)
                || nm.contains(QStringLiteral("VANGUARD"), Qt::CaseInsensitive)
                || nm.contains(QStringLiteral("VAN 216"), Qt::CaseInsensitive)
                || nm.contains(QStringLiteral("ОШИБКА VANGUARD"), Qt::CaseInsensitive)) {
                qWarning() << "[RIOT] диалог ошибки запуска/Vanguard — стоп Play |" << why
                           << "|" << nm.left(80);
                m_playClickStop = true;
                break;
            }
        }
        all->Release();
    }
    if (trueCond)
        trueCond->Release();

    root->Release();
    automation->Release();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::dismissClientOverlay(const char *why)
{
#ifdef Q_OS_WIN
    if (m_overlayDismissed)
        return;

    HWND hwnd = findLeagueClientHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        qWarning() << "[RIOT] overlay — нет HWND League Client |" << why;
        return;
    }

    riotForceForeground(hwnd);
    Sleep(120);

    HWND clickHwnd = findLeagueRenderHwnd(hwnd);
    qWarning() << "[RIOT] overlay click target hwnd render!=" << (clickHwnd != hwnd) << "|" << why;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        qWarning() << "[RIOT] overlay UIA CoCreate fail" << Qt::hex << quint32(hr);
        return;
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        qWarning() << "[RIOT] overlay ElementFromHandle fail |" << why;
        automation->Release();
        return;
    }

    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (trueCond)
        root->FindAll(TreeScope_Descendants, trueCond, &all);
    int len = 0;
    if (all)
        all->get_Length(&len);
    qWarning() << "[RIOT] overlay UIA descendants:" << len << "|" << why;

    IUIAutomationElement *bestStart = nullptr;
    int bestStartArea = 0;
    POINT bestStartClick{};
    bool haveStart = false;
    POINT headerPlayClick{};
    bool haveHeaderPlay = false;
    int headerPlayArea = 0;
    int logged = 0;
    bool looksHome = false;
    bool looksTutorialCarousel = false;
    bool looksLobby = false;
    bool stillLoading = false;

    RECT winRect{};
    GetWindowRect(hwnd, &winRect);
    const int winH = qMax(1, winRect.bottom - winRect.top);

    const int limit = qMin(len, 500);
    for (int i = 0; i < limit; ++i) {
        IUIAutomationElement *el = nullptr;
        all->GetElement(i, &el);
        if (!el)
            continue;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name) : QString();
        if (name)
            SysFreeString(name);
        const QString nl = n.trimmed().toLower();

        BOOL off = FALSE;
        el->get_CurrentIsOffscreen(&off);
        RECT r{};
        el->get_CurrentBoundingRectangle(&r);
        const int area = riotRectArea(r);

        if (!nl.isEmpty()) {
            if (nl.contains(QStringLiteral("путь новичка"))
                || nl.contains(QStringLiteral("обучение для новых"))
                || nl.contains(QStringLiteral("сообщество"))
                || nl.contains(QStringLiteral("список друзей пуст"))
                || nl.contains(QStringLiteral("в сети"))
                || nl.contains(QStringLiteral("награды за повышение"))
                || nl.contains(QStringLiteral("наборы для новичков")))
                looksHome = true;
            if (nl.contains(QStringLiteral("обучение"))
                || nl.contains(QStringLiteral("завершите все обучающ"))
                || nl.contains(QStringLiteral("добро пожаловать")))
                looksTutorialCarousel = true;
            if (nl.contains(QStringLiteral("загрузка")))
                stillLoading = true;
            if (nl.contains(QStringLiteral("найти игру"))
                || nl.contains(QStringLiteral("выберите режим"))
                || nl.contains(QStringLiteral("очередь"))
                || nl.contains(QStringLiteral("подтвердить"))
                || nl.contains(QStringLiteral("пригласить"))
                || nl.contains(QStringLiteral("blind pick"))
                || nl.contains(QStringLiteral("выбор чемпиона")))
                looksLobby = true;
        }

        if (logged < 12 && area >= 200 && !n.trimmed().isEmpty()) {
            qWarning().noquote() << "[RIOT] overlay el#" << i << "\"" << n.trimmed() << "\""
                                 << (r.right - r.left) << "x" << (r.bottom - r.top)
                                 << "top" << r.top;
            ++logged;
        }

        // Верхняя синяя «ИГРАТЬ»: берём САМУЮ КРУПНУЮ в верхней трети (не мелкий текст 58x18)
        const int relTop = r.top - winRect.top;
        if (!off && area >= 80 && riotNameLooksPlay(n) && relTop >= 0 && relTop < winH / 3) {
            if (area > headerPlayArea) {
                headerPlayArea = area;
                headerPlayClick.x = (r.left + r.right) / 2;
                headerPlayClick.y = (r.top + r.bottom) / 2;
                haveHeaderPlay = true;
            }
        }

        if (!riotNameLooksStart(n) || off || area < 80) {
            el->Release();
            continue;
        }

        if (area >= bestStartArea) {
            if (bestStart)
                bestStart->Release();
            bestStart = el;
            bestStartArea = area;
            bestStartClick.x = (r.left + r.right) / 2;
            bestStartClick.y = (r.top + r.bottom) / 2;
            haveStart = true;
            continue;
        }
        el->Release();
    }
    if (all)
        all->Release();
    if (trueCond)
        trueCond->Release();

    auto clickHeaderPlay = [&]() -> bool {
        if (m_headerPlayAttempts >= 5)
            return false;
        riotForceForeground(hwnd);
        ShowWindow(hwnd, SW_RESTORE);
        ++m_headerPlayAttempts;
        m_leagueHeaderPlayDone = true;
        // Всегда heuristic лево-верх — UIA «ИГРАТЬ» 58×18 мимо бирюзовой кнопки
        HWND target = clickHwnd ? clickHwnd : hwnd;
        RECT cr{};
        GetClientRect(target, &cr);
        // Несколько точек в зоне верхней бирюзовой ИГРАТЬ
        const double spots[][2] = {{0.09, 0.055}, {0.12, 0.06}, {0.15, 0.065}};
        const auto &s = spots[qMin(m_headerPlayAttempts - 1, 2)];
        POINT p;
        p.x = int(cr.right * s[0]);
        p.y = int(cr.bottom * s[1]);
        ClientToScreen(target, &p);
        qWarning() << "[RIOT] heuristic верхней ИГРАТЬ @" << p.x << p.y
                   << "try" << m_headerPlayAttempts << "|" << why;
        riotClickScreen(p.x, p.y, "header ИГРАТЬ");
        return true;
    };

    if (looksLobby) {
        qWarning() << "[RIOT] overlay: уже лобби/очередь — готово |" << why;
        m_overlayDismissed = true;
        if (bestStart)
            bestStart->Release();
        root->Release();
        automation->Release();
        return;
    }

    // «Путь новичка» / дом — LCU lobby + верхняя ИГРАТЬ (не золотая в карточке)
    if (looksHome && !looksTutorialCarousel) {
        qWarning() << "[RIOT] overlay: Путь новичка/дом — lobby + header Play |" << why;
        openLeagueLobbyViaLcu(why);
        clickHeaderPlay();
        // Не ставим dismissed по числу кликов — только lobby OK или looksLobby
        if (bestStart)
            bestStart->Release();
        root->Release();
        automation->Release();
        return;
    }

    if (bestStart && haveStart) {
        BSTR pname = nullptr;
        bestStart->get_CurrentName(&pname);
        qWarning().noquote() << "[RIOT] overlay ONE click:\""
                             << (pname ? QString::fromWCharArray(pname) : QString())
                             << "\" @" << bestStartClick.x << bestStartClick.y << "|" << why;
        if (pname)
            SysFreeString(pname);
        riotClickScreen(bestStartClick.x, bestStartClick.y, why);
        bestStart->Release();
    } else {
        if (bestStart)
            bestStart->Release();
        if (stillLoading || len < 8) {
            qWarning() << "[RIOT] overlay: ещё загрузка/мало UI — ждём |" << why
                       << "len" << len << "loading" << stillLoading;
        } else if (looksTutorialCarousel) {
            HWND target = clickHwnd ? clickHwnd : hwnd;
            RECT cr{};
            GetClientRect(target, &cr);
            const int cw = cr.right > 0 ? cr.right : 1280;
            const int ch = cr.bottom > 0 ? cr.bottom : 720;
            qWarning() << "[RIOT] overlay: rewind + ONE НАЧАТЬ |" << why
                       << "render" << cw << "x" << ch;
            riotForceForeground(hwnd);
            for (int i = 0; i < 4; ++i) {
                sendVk(VK_LEFT);
                Sleep(120);
            }
            POINT btn;
            btn.x = int(cw * 0.50);
            btn.y = int(ch * 0.90);
            ClientToScreen(target, &btn);
            riotClickScreen(btn.x, btn.y, "НАЧАТЬ bottom");
        } else if (len >= 25) {
            qWarning() << "[RIOT] overlay: UI готов без ОБУЧЕНИЕ — header Play + lobby |" << why;
            openLeagueLobbyViaLcu(why);
            clickHeaderPlay();
        } else {
            qWarning() << "[RIOT] overlay: ждём полный UI |" << why << "len" << len;
        }
    }

    root->Release();
    automation->Release();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::shellExecuteRiotProtocol(const char *why)
{
    if (m_launchAborted || isGameProcessRunning())
        return;
    QString productId;
    QString patchline;
    parseProductArgs(&productId, &patchline);
#ifdef Q_OS_WIN
    // Безопаснее прямого LeagueClient.exe — обрабатывает сам Riot Client
    const QString uri = QStringLiteral("riotclient://launch-product?product=%1&patchline=%2")
                            .arg(productId, patchline);
    const UINT_PTR r = reinterpret_cast<UINT_PTR>(
        ShellExecuteW(nullptr, L"open",
                      reinterpret_cast<LPCWSTR>(uri.utf16()),
                      nullptr, nullptr, SW_SHOWNORMAL));
    qWarning().noquote() << "[RIOT] protocol" << uri
                         << "| result:" << r << (r > 32 ? "OK" : "FAIL") << "|" << why;
#else
    Q_UNUSED(why);
#endif
}

bool RiotAuth::acceptStaySignedInModal(const char *why)
{
#ifdef Q_OS_WIN
    if (m_launchAborted)
        return false;
    HWND hwnd = reinterpret_cast<HWND>(m_loginHwnd);
    if (!hwnd || !IsWindow(hwnd))
        hwnd = FindWindowW(nullptr, L"Riot Client");
    if (!hwnd || !IsWindow(hwnd))
        return false;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void **>(&automation)))
        || !automation)
        return false;

    IUIAutomationElement *root = nullptr;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
        automation->Release();
        return false;
    }

    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (!trueCond || FAILED(root->FindAll(TreeScope_Descendants, trueCond, &all)) || !all) {
        if (trueCond) trueCond->Release();
        root->Release();
        automation->Release();
        return false;
    }
    trueCond->Release();

    int len = 0;
    all->get_Length(&len);
    bool clicked = false;
    for (int i = 0; i < len && !clicked; ++i) {
        IUIAutomationElement *el = nullptr;
        all->GetElement(i, &el);
        if (!el)
            continue;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name).trimmed() : QString();
        if (name)
            SysFreeString(name);

        const bool looksAccept =
            n.contains(QStringLiteral("Оставаться"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Не выходить"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Stay signed"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("signed in"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("longLived"), Qt::CaseInsensitive)
            || (n.contains(QStringLiteral("Accept"), Qt::CaseInsensitive)
                && n.size() < 24);

        // Не жмём «Играть» / Cancel / Нет
        const bool looksReject =
            n.contains(QStringLiteral("Играть"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Play"), Qt::CaseInsensitive)
            || n.compare(QStringLiteral("Нет"), Qt::CaseInsensitive) == 0
            || n.compare(QStringLiteral("No"), Qt::CaseInsensitive) == 0
            || n.contains(QStringLiteral("Cancel"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Отмена"), Qt::CaseInsensitive);

        if (!looksAccept || looksReject) {
            el->Release();
            continue;
        }

        RECT r{};
        el->get_CurrentBoundingRectangle(&r);
        IUnknown *patUnk = nullptr;
        if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &patUnk)) && patUnk) {
            IUIAutomationInvokePattern *inv = nullptr;
            if (SUCCEEDED(patUnk->QueryInterface(IID_IUIAutomationInvokePattern,
                                                 reinterpret_cast<void **>(&inv)))
                && inv) {
                inv->Invoke();
                clicked = true;
                qWarning().noquote() << "[RIOT] stay-signed ACCEPT Invoke:\"" << n << "\" |" << why;
                inv->Release();
            }
            patUnk->Release();
        }
        if (!clicked && r.right > r.left) {
            const int cx = (r.left + r.right) / 2;
            const int cy = (r.top + r.bottom) / 2;
            AllowSetForegroundWindow(ASFW_ANY);
            SetForegroundWindow(hwnd);
            riotClickScreen(cx, cy, "stay-signed accept");
            clicked = true;
            qWarning().noquote() << "[RIOT] stay-signed ACCEPT click:\"" << n << "\" |" << why;
        }
        el->Release();
    }

    all->Release();
    root->Release();
    automation->Release();
    if (!clicked)
        qWarning() << "[RIOT] stay-signed modal: Accept не найдена (OK на CEF) |" << why;
    return clicked;
#else
    Q_UNUSED(why);
    return false;
#endif
}

void RiotAuth::dismissRiotModalsSoft(const char *why)
{
#ifdef Q_OS_WIN
    if (m_launchAborted)
        return;
    // Не Esc: на этой сборке long-lived session всё равно не пишется
    // (keypairSupported=false). Esc только мешает продуктовой странице.
    acceptStaySignedInModal(why);
    keepShellOverlayUp();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::shellExecuteProductOnce(const char *why)
{
    if (isGameProcessRunning()) {
        qWarning() << "[RIOT] ShellExecute skip — игра есть |" << why;
        return;
    }
    if (m_launcherExe.isEmpty() || m_productArgs.trimmed().isEmpty()) {
        qWarning() << "[RIOT] ShellExecute skip — нет exe/args |" << why;
        return;
    }
#ifdef Q_OS_WIN
    const QString work = QFileInfo(m_launcherExe).absolutePath();
    const UINT_PTR r = reinterpret_cast<UINT_PTR>(
        ShellExecuteW(nullptr, L"open",
                      reinterpret_cast<LPCWSTR>(m_launcherExe.utf16()),
                      reinterpret_cast<LPCWSTR>(m_productArgs.utf16()),
                      reinterpret_cast<LPCWSTR>(work.utf16()),
                      SW_SHOWNORMAL));
    qWarning().noquote() << "[RIOT] ShellExecute product (" << why << "):"
                         << m_launcherExe << m_productArgs
                         << "| result:" << r << (r > 32 ? "OK" : "FAIL");
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::launchProductViaApi(const char *why)
{
    if (m_launchAborted) {
        qWarning() << "[RIOT] launchProductViaApi blocked (abort) |" << why;
        return;
    }
    if (isGameProcessRunning()) {
        qWarning() << "[RIOT] API skip — игра уже есть |" << why;
        return;
    }

    QString productId;
    QString patchline;
    parseProductArgs(&productId, &patchline);

    int port = 0;
    QString password;
    QString protocol;
    if (!readRiotLockfile(&port, &password, &protocol)) {
        qWarning() << "[RIOT] lockfile нет |" << why;
        return;
    }

    // 1) Проверяем RSO — без активной сессии launch даёт пустой 204
    QUrl sessionUrl(QStringLiteral("%1://127.0.0.1:%2/rso-auth/v1/session")
                        .arg(protocol).arg(port));
    QNetworkRequest sessionReq(sessionUrl);
    sessionReq.setRawHeader("Authorization",
                            QByteArrayLiteral("Basic ")
                                + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64());
    sessionReq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    qWarning() << "[RIOT] GET /rso-auth/v1/session |" << why;
    QNetworkReply *sessionReply = ensureNam()->get(sessionReq);
    connect(sessionReply, &QNetworkReply::finished, this,
            [this, sessionReply, why, productId, patchline, gen = m_launchGen]() {
        if (gen != m_launchGen) {
            sessionReply->deleteLater();
            return;
        }
        const int code = sessionReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = sessionReply->readAll();
        qWarning().noquote() << "[RIOT] RSO session HTTP" << code
                             << "| body:" << QString::fromUtf8(body.left(300))
                             << "|" << why;
        sessionReply->deleteLater();

        if (m_launchAborted)
            return;

        const QByteArray bodyLower = body.toLower();
        const bool rsoReady = (code >= 200 && code < 300)
            && bodyLower.contains("authenticated");
        const bool persistOn = bodyLower.contains("\"persistlogin\":true");
        if (rsoReady)
            qWarning() << "[RIOT] RSO authenticated | persistLogin:" << persistOn;
        if (rsoReady && !persistOn)
            qWarning() << "[RIOT] WARN: persistLogin=false — галка «не выходить» не сработала;"
                       << "silent cache после сессии маловероятен (ручной вход пишет persist)";

        if (rsoReady && !persistOn)
            tryEnablePersistLogin(why);

        // Не жмём Play пока RSO 404/не готов — иначе гонка с --launch-product → 2 клиента
        if (!rsoReady) {
            const int maxRso = 20;
            // UI-логин: повторный Enter; API-логин — только ждём
            if (m_credentialsSent && !m_apiLoginAttempted
                && (m_rsoRetryCount == 2 || m_rsoRetryCount == 7)) {
#ifdef Q_OS_WIN
                HWND h = reinterpret_cast<HWND>(m_loginHwnd);
                if (!h || !IsWindow(h))
                    h = FindWindowW(nullptr, L"Riot Client");
                if (h && IsWindow(h)) {
                    restoreRiotOnscreen("before resubmit Enter");
                    AllowSetForegroundWindow(ASFW_ANY);
                    SetForegroundWindow(h);
                    Sleep(150);
                    sendReturnKey();
                    keepShellOverlayUp();
                    qWarning() << "[RIOT] повторный Enter на Войти (RSO ещё не готов)";
                }
#endif
            }
            if (m_rsoRetryCount < maxRso) {
                ++m_rsoRetryCount;
                qWarning() << "[RIOT] RSO ещё не готов — retry" << m_rsoRetryCount
                           << "/" << maxRso << "через 2.5s";
                QTimer::singleShot(2500, this, [this, gen]() {
                    if (gen != m_launchGen || m_launchAborted || isGameProcessRunning())
                        return;
                    launchProductViaApi("retry after RSO wait");
                });
            } else {
                qWarning() << "[RIOT] RSO так и не готов — finishLaunchNudge";
                finishLaunchNudge("RSO never ready");
            }
            return;
        }

        Q_UNUSED(patchline);
        // RSO OK — страница продукта грузится ON-SCREEN под оверлеем (не park: иначе UIA пустой)
        restoreRiotOnscreen("RSO authenticated");
        keepShellOverlayUp();

        // Этап 2–3: /product-launcher/.../launch на этой сборке 404.
        // Уже стартовали с --launch-product → страница LoL после логина.
        // НЕ зовём riotclient://launch-product снова: вместе с Play = 2 клиента.
        discoverRiotLaunchPaths(why);
        postRiotApi(QStringLiteral("/product-launcher/v1/products/%1/patchlines/%2/launch")
                        .arg(productId, patchline.isEmpty() ? QStringLiteral("live") : patchline),
                    QByteArrayLiteral("{}"), "try product-launcher (may 404)");
        postRiotApi(QStringLiteral("/eula/v1/agreements/accept"), QByteArrayLiteral("{}"),
                    "try eula accept (may 404)");

        // Сначала принять stay-signed (Esc её убивает → persist:null → кэш не в БД)
        QTimer::singleShot(800, this, [this, gen]() {
            if (gen != m_launchGen || m_launchAborted || isGameProcessRunning())
                return;
            acceptStaySignedInModal("after RSO @0.8s");
        });
        QTimer::singleShot(1500, this, [this, gen]() {
            if (gen != m_launchGen || m_launchAborted || isGameProcessRunning())
                return;
            dismissRiotModalsSoft("before Play");
        });
        QTimer::singleShot(2500, this, [this, gen]() {
            if (gen != m_launchGen || m_launchAborted || isGameProcessRunning())
                return;
            acceptStaySignedInModal("after RSO @2.5s");
        });
        QTimer::singleShot(3500, this, [this, why, gen]() {
            if (gen != m_launchGen || m_launchAborted)
                return;
            if (isGameProcessRunning()) {
                qWarning() << "[RIOT] игра уже после логина — UIA не нужен";
                m_playClickStop = true;
                m_productLaunchOk = true;
                ensureSingleLeagueClient();
                notifyProductReady();
                return;
            }
            finishLaunchNudge(why);
        });
    });
}

void RiotAuth::stopScout()
{
    // Гасим отложенные Play/LCU/overlay — иначе после закрытия окна снова «пинают» Riot
    ++m_launchGen;
    m_launchAborted = true;
    m_playClickStop = true;
    m_overlayDismissed = true;
    m_productLaunchScheduled = false;
    if (m_sessionPollTimer) {
        m_sessionPollTimer->stop();
        m_sessionPollTimer->deleteLater();
        m_sessionPollTimer = nullptr;
    }
    if (!m_scoutTimer)
        return;
    m_scoutTimer->stop();
    m_scoutTimer->deleteLater();
    m_scoutTimer = nullptr;
}

void RiotAuth::startScout(const QString &login, const QString &password)
{
#ifdef Q_OS_WIN
    stopScout();
    // stopScout() ставит abort — для НОВОГО запуска сбрасываем
    m_launchAborted = false;
    m_playClickStop = false;
    m_overlayDismissed = false;
    m_ticks = 0;
    m_phaseTick = 0;
    m_stableCount = 0;
    m_lastW = 0;
    m_lastH = 0;
    m_loginHwnd = 0;
    m_riotParkedOffscreen = false;
    m_riotParkedHwnd = 0;
    m_riotSavedW = 0;
    m_riotSavedH = 0;
    m_phase = Phase::WaitLoginWindow;
    m_credentialsSent = false;
    m_apiLoginAttempted = false;
    m_apiLoginInFlight = false;
    m_persistEnableAttempted = false;
    m_login = login;
    m_password = password;

    if (!m_expectInteractive || login.isEmpty() || password.isEmpty()) {
        qWarning() << "[RIOT] Scout пропущен (personal / silent)";
        m_allowsGameDetect = true;
        // Клубный тихий вход: yaml уже на диске → ждём RSO (дольше — клиент долго init)
        if (!m_expectInteractive && !login.isEmpty() && !password.isEmpty()) {
            const int silentGen = m_launchGen;
            QTimer::singleShot(4000, this, [this, silentGen]() {
                if (silentGen != m_launchGen || m_launchAborted || isGameProcessRunning())
                    return;
                qWarning() << "[RIOT] silent: scheduleProductLaunch после cache";
                scheduleProductLaunch();
            });
            // Fallback scout только если RSO так и не поднялся (~45с)
            QTimer::singleShot(45000, this, [this, login, password, silentGen]() {
                if (silentGen != m_launchGen || m_launchAborted || isGameProcessRunning()
                    || m_credentialsSent || m_productLaunchOk)
                    return;
                qWarning() << "[RIOT] silent cache не сработал — fallback scout (API persist)";
                m_expectInteractive = true;
                m_needBackup = true;
                startScout(login, password);
            });
        }
        return;
    }

    m_allowsGameDetect = false;
    // Shell stays visible with loading overlay; hideShellForGame runs at normal game-hide timing.
    keepOverlayOverRiot();
    qWarning() << "[RIOT] Scout START (phase machine), login:" << login
               << "| pass len" << password.size();

    m_scoutTimer = new QTimer(this);
    m_scoutTimer->setInterval(300);

    connect(m_scoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_scoutTimer)
            return;

        ++m_ticks;
        if (m_ticks > 300) {
            qWarning() << "[RIOT] Scout TIMEOUT phase" << int(m_phase);
            m_allowsGameDetect = true;
            keepOverlayOverRiot();
            stopScout();
            return;
        }

        if (m_phase == Phase::Done) {
            stopScout();
            return;
        }

        RiotLoginEnumCtx ctx;
        ctx.riotPids = collectRiotPids();
        ctx.doDump = (m_ticks % 8 == 0
                      && (m_phase == Phase::WaitLoginWindow
                          || m_phase == Phase::WaitStableSize));
        EnumWindows(enumRiotLoginProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.best) {
            HWND byTitle = FindWindowW(nullptr, L"Riot Client");
            if (byTitle && IsWindowVisible(byTitle)) {
                ctx.best = byTitle;
                ctx.title = QStringLiteral("FindWindow(Riot Client)");
            }
        }

        HWND hwnd = ctx.best ? ctx.best : reinterpret_cast<HWND>(m_loginHwnd);
        int w = 0, hgt = 0;
        if (hwnd && IsWindow(hwnd)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            w = rc.right - rc.left;
            hgt = rc.bottom - rc.top;
        }

        // До Enter: Riot ДОЛЖЕН быть на экране (CEF/RSO ломаются за -32000).
        // Оверлей TOPMOST закрывает его от игрока. Park — только после Submit.
        const bool loginPhases =
            m_phase == Phase::WaitLoginWindow
            || m_phase == Phase::WaitStableSize
            || m_phase == Phase::TryApiLogin
            || m_phase == Phase::TypeUsername
            || m_phase == Phase::TabToPassword
            || m_phase == Phase::TypePassword
            || m_phase == Phase::EnsurePersist
            || m_phase == Phase::TabToSubmit
            || m_phase == Phase::PressEnter;
        if (loginPhases) {
            if (m_riotParkedOffscreen)
                restoreRiotOnscreen("login on-screen");
            keepShellOverlayUp();
        } else {
            keepShellOverlayUp();
        }

        if (ctx.doDump) {
            qWarning() << "[RIOT] tick" << m_ticks << "| phase" << int(m_phase)
                       << "| best:" << (ctx.best ? ctx.title : QStringLiteral("(none)"))
                       << "| size" << w << "x" << hgt
                       << "| parked" << m_riotParkedOffscreen;
        }

        switch (m_phase) {
        case Phase::WaitLoginWindow:
            if (ctx.best && m_ticks >= 10) {
                m_loginHwnd = reinterpret_cast<quintptr>(ctx.best);
                m_lastW = w;
                m_lastH = hgt;
                m_stableCount = 0;
                m_phase = Phase::WaitStableSize;
                m_phaseTick = m_ticks;
                qWarning() << "[RIOT] окно найдено, ждём стабильный размер:" << ctx.title;
            }
            break;

        case Phase::WaitStableSize: {
            // Промежуточный 1300×600 — ещё не форма; клики по нему открывают браузер (Cloudflare).
            // Ждём финальный размер (~1536×864) и стабильность, без мыши.
            const bool largeEnough = (w >= 1400 && hgt >= 700);
            if (largeEnough && w == m_lastW && hgt == m_lastH)
                ++m_stableCount;
            else {
                m_stableCount = 0;
                m_lastW = w;
                m_lastH = hgt;
                if (ctx.best)
                    m_loginHwnd = reinterpret_cast<quintptr>(ctx.best);
                if (!largeEnough && (m_ticks % 8 == 0))
                    qWarning() << "[RIOT] ждём большой UI, сейчас" << w << "x" << hgt;
            }
            if (largeEnough && m_stableCount >= 6 && m_ticks >= m_phaseTick + 10) {
                // API до полного UI-логина на этой сборке всегда «RSO not yet initialized».
                // Сразу username→password→Enter (рабочий путь). Без ~12с пустого API-wait.
                m_phase = Phase::TypeUsername;
                m_phaseTick = m_ticks;
                qWarning() << "[RIOT] UI готов" << m_lastW << "x" << m_lastH
                           << "→ username (UI scout)";
            }
            break;
        }

        case Phase::TryApiLogin: {
            // Оставлен для совместимости enum; сразу в UI password
            m_phase = Phase::TypePassword;
            m_phaseTick = m_ticks;
            break;
        }

        case Phase::TypeUsername: {
            // На экране под TOPMOST-оверлеем; SendInput в foreground Riot.
            if (m_ticks < m_phaseTick + 5)
                break;
            HWND h = reinterpret_cast<HWND>(m_loginHwnd);
            if (!h || !IsWindow(h)) {
                qWarning() << "[RIOT] HWND потерян на username";
                m_phase = Phase::WaitLoginWindow;
                break;
            }
            if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                pm->requestClearGameSearch();
            if (m_riotParkedOffscreen)
                restoreRiotOnscreen("before username");
            AllowSetForegroundWindow(ASFW_ANY);
            SetForegroundWindow(h);
            Sleep(200);
            for (int i = 0; i < 48; ++i)
                sendVk(VK_BACK);
            Sleep(100);
            qWarning() << "[RIOT] type username len" << m_login.size() << m_login;
            typeUnicode(m_login);
            m_credentialsSent = true;
            keepShellOverlayUp();
            m_phase = Phase::TabToPassword;
            m_phaseTick = m_ticks;
            break;
        }

        case Phase::TabToPassword:
            if (m_ticks < m_phaseTick + 2)
                break;
            if (HWND h = reinterpret_cast<HWND>(m_loginHwnd)) {
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(h);
            }
            sendTabs(1, "→ password");
            keepShellOverlayUp();
            m_phase = Phase::TypePassword;
            m_phaseTick = m_ticks;
            break;

        case Phase::TypePassword:
            if (m_ticks < m_phaseTick + 2)
                break;
            if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                pm->requestClearGameSearch();
            if (HWND h = reinterpret_cast<HWND>(m_loginHwnd)) {
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(h);
            }
            Sleep(80);
            for (int i = 0; i < 40; ++i)
                sendVk(VK_BACK);
            Sleep(60);
            qWarning() << "[RIOT] type password len" << m_password.size();
            typeUnicode(m_password);
            m_credentialsSent = true;
            keepShellOverlayUp();
            // password → Tab×4 «Не выходить» → Space → Tab×1 → Enter (full login).
            m_phase = Phase::EnsurePersist;
            m_phaseTick = m_ticks;
            break;

        case Phase::EnsurePersist: {
            // Tab×4 = «Не выходить». Space toggles. No mouse. (Tab×5 would be Submit.)
            if (m_ticks < m_phaseTick + 3)
                break;
            sendTabs(4, "→ «Не выходить» checkbox");
            Sleep(150);
            sendVk(VK_SPACE);
            qWarning() << "[RIOT] Tab×4 «Не выходить» → Space (persist ON) → Tab×1 Submit";
            m_phase = Phase::TabToSubmit;
            m_phaseTick = m_ticks;
            break;
        }

        case Phase::TabToSubmit:
            // Focus leaves checkbox → red arrow / Войти. Then Enter submits.
            if (m_ticks < m_phaseTick + 1)
                break;
            sendTabs(1, "→ Submit (red arrow / Войти)");
            m_phase = Phase::PressEnter;
            m_phaseTick = m_ticks;
            break;

        case Phase::PressEnter:
            if (m_ticks < m_phaseTick + 1)
                break;
            if (HWND h = reinterpret_cast<HWND>(m_loginHwnd)) {
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(h);
            }
            Sleep(80);
            sendReturnKey();
            qWarning() << "[RIOT] Enter на red arrow submit";
            Sleep(200);
            closeRiotRecoveryBrowsers();
            m_allowsGameDetect = true;
            m_needBackup = true;
            m_phase = Phase::Done;
            qWarning() << "[RIOT] login submitted — ждём session yaml → ShellExecute product";
            // Не park: страница продукта (--launch-product) должна грузиться on-screen под оверлеем
            keepShellOverlayUp();
            stopScout();
            scheduleProductLaunch();
            break;

        case Phase::Done:
            break;
        }
    });

    m_scoutTimer->start();
#else
    Q_UNUSED(login);
    Q_UNUSED(password);
    m_allowsGameDetect = true;
#endif
}
