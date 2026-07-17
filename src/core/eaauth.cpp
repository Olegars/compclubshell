#include "eaauth.h"
#include "processmanager.h"
#include "networkmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#endif

static QString eaLocalAppData()
{
    QString local = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
    if (local.isEmpty())
        local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir::toNativeSeparators(local);
}

static QString eaDesktopRoot()
{
    return eaLocalAppData() + QStringLiteral("/Electronic Arts/EA Desktop");
}

static QString defaultEaDesktopExe()
{
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe"),
        QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EADesktop.exe"),
    };
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p))
            return QDir::toNativeSeparators(p);
    }
    return QDir::toNativeSeparators(candidates.first());
}

static QJsonObject extractVdfFiles(const QJsonObject &authData)
{
    QJsonObject vdf = authData.value(QStringLiteral("vdf_files")).toObject();
    if (vdf.isEmpty()) {
        const QJsonObject auth = authData.value(QStringLiteral("auth")).toObject();
        const QJsonObject cache = auth.value(QStringLiteral("cache")).toObject();
        if (cache.contains(QStringLiteral("vdf_files")))
            vdf = cache.value(QStringLiteral("vdf_files")).toObject();
        else
            vdf = cache;
    }
    return vdf;
}

static bool writeBinaryFile(const QString &path, const QByteArray &data)
{
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[EA] write fail:" << path << f.errorString();
        return false;
    }
    f.write(data);
    f.close();
    return true;
}

static QByteArray readBinaryFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// Собираем компактный machine-cache (без огромного CEF Cache_Data).
static QStringList eaCacheRelativePaths()
{
    QStringList rels;
    const QString root = eaDesktopRoot();
    QDir rootDir(root);
    if (!rootDir.exists())
        return rels;

    const QFileInfoList users = rootDir.entryInfoList(
        QStringList{QStringLiteral("user_*.ini")}, QDir::Files);
    for (const QFileInfo &fi : users)
        rels << fi.fileName();

    if (QFileInfo::exists(root + QStringLiteral("/telemetry.ini")))
        rels << QStringLiteral("telemetry.ini");

    const QStringList dirRoots = {
        QStringLiteral("SEC"),
        QStringLiteral("OfflineCache"),
        QStringLiteral("CEF/BrowserCache/EADesktop/Network"),
        QStringLiteral("CEF/BrowserCache/EADesktop/Local Storage"),
    };
    for (const QString &sub : dirRoots) {
        const QString abs = root + QLatin1Char('/') + sub;
        if (!QDir(abs).exists())
            continue;
        QDirIterator it(abs, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QString rel = rootDir.relativeFilePath(it.filePath());
            // Cookies / Local Storage / SEC — без Cache_Data и логов
            if (rel.contains(QStringLiteral("Cache_Data"), Qt::CaseInsensitive)
                || rel.contains(QStringLiteral("Code Cache"), Qt::CaseInsensitive)
                || rel.contains(QStringLiteral("GPUCache"), Qt::CaseInsensitive)
                || rel.contains(QStringLiteral("DawnCache"), Qt::CaseInsensitive)
                || rel.contains(QStringLiteral("/Logs/"), Qt::CaseInsensitive))
                continue;
            if (it.fileInfo().size() > 2 * 1024 * 1024)
                continue;
            rels << QDir::fromNativeSeparators(rel);
        }
    }
    rels.removeDuplicates();
    return rels;
}

static QString packEaCacheBlob()
{
    const QString root = eaDesktopRoot();
    QJsonObject files;
    qint64 total = 0;
    constexpr qint64 kMaxTotal = 4 * 1024 * 1024;

    for (const QString &rel : eaCacheRelativePaths()) {
        const QString abs = root + QLatin1Char('/') + rel;
        const QByteArray raw = readBinaryFile(abs);
        if (raw.isEmpty())
            continue;
        if (total + raw.size() > kMaxTotal) {
            qWarning() << "[EA] cache pack: лимит размера, skip" << rel;
            continue;
        }
        files.insert(rel, QString::fromLatin1(raw.toBase64()));
        total += raw.size();
    }

    if (files.isEmpty())
        return {};

    QJsonObject rootObj;
    rootObj.insert(QStringLiteral("ea_cache_version"), 1);
    rootObj.insert(QStringLiteral("files"), files);
    const QByteArray json = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);
    qWarning() << "[EA] cache pack: files" << files.size() << "raw bytes" << total
               << "json" << json.size();
    return QString::fromUtf8(json);
}

static bool unpackEaCacheBlob(const QString &blob)
{
    const QString trimmed = blob.trimmed();
    if (trimmed.isEmpty())
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[EA] cache unpack: не JSON" << err.errorString();
        return false;
    }
    const QJsonObject rootObj = doc.object();
    if (rootObj.value(QStringLiteral("ea_cache_version")).toInt() < 1) {
        qWarning() << "[EA] cache unpack: нет ea_cache_version";
        return false;
    }
    const QJsonObject files = rootObj.value(QStringLiteral("files")).toObject();
    if (files.isEmpty())
        return false;

    const QString root = eaDesktopRoot();
    int ok = 0;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString rel = it.key();
        if (rel.contains(QLatin1String("..")))
            continue;
        const QByteArray raw = QByteArray::fromBase64(it.value().toString().toLatin1());
        if (raw.isEmpty())
            continue;
        if (writeBinaryFile(root + QLatin1Char('/') + rel, raw))
            ++ok;
    }
    qWarning() << "[EA] cache unpack: восстановлено файлов" << ok << "/" << files.size()
               << "→" << root;
    return ok > 0;
}

#ifdef Q_OS_WIN
static QString processImageForPid(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return {};
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

static QSet<DWORD> collectEaPids()
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
            if (name.compare(QStringLiteral("EADesktop.exe"), Qt::CaseInsensitive) == 0
                || name.compare(QStringLiteral("EABackgroundService.exe"), Qt::CaseInsensitive) == 0
                || name.compare(QStringLiteral("EALauncher.exe"), Qt::CaseInsensitive) == 0)
                pids.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static void placeWindowForInput(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    AllowSetForegroundWindow(pid);
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    const DWORD ourTid = GetCurrentThreadId();
    const DWORD foreTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, TRUE);
    if (tid && tid != ourTid)
        AttachThreadInput(ourTid, tid, TRUE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (tid && tid != ourTid)
        AttachThreadInput(ourTid, tid, FALSE);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, FALSE);
}

// Нельзя SetFocus на HWND EA — сбивает фокус с CEF-поля (пароль уже сфокусирован).
static void placeWindowForegroundOnly(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    AllowSetForegroundWindow(pid);
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    const DWORD ourTid = GetCurrentThreadId();
    const DWORD foreTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, TRUE);
    if (tid && tid != ourTid)
        AttachThreadInput(ourTid, tid, TRUE);
    SetForegroundWindow(hwnd);
    if (tid && tid != ourTid)
        AttachThreadInput(ourTid, tid, FALSE);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, FALSE);
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

static void sendCtrlA()
{
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = 'A';
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = 'A';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static void sendTabs(int count, const char *why)
{
    qWarning() << "[EA] Tab x" << count << why;
    for (int i = 0; i < count; ++i) {
        sendVk(VK_TAB);
        Sleep(200);
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
        Sleep(12);
    }
}

static void clearAndType(HWND hwnd, const QString &text)
{
    placeWindowForInput(hwnd);
    sendCtrlA();
    Sleep(40);
    sendVk(VK_DELETE);
    Sleep(50);

    bool pasted = false;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        const std::wstring wstr = text.toStdWString();
        const size_t bytes = (wstr.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            memcpy(GlobalLock(hMem), wstr.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
            CloseClipboard();
            Sleep(40);
            INPUT in[4] = {};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_CONTROL;
            in[1].type = INPUT_KEYBOARD;
            in[1].ki.wVk = 'V';
            in[2].type = INPUT_KEYBOARD;
            in[2].ki.wVk = 'V';
            in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[3].type = INPUT_KEYBOARD;
            in[3].ki.wVk = VK_CONTROL;
            in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, in, sizeof(INPUT));
            pasted = true;
            qWarning() << "[EA] input via clipboard paste, len" << text.size();
        } else {
            CloseClipboard();
        }
    }
    if (!pasted) {
        qWarning() << "[EA] input via unicode type, len" << text.size();
        typeUnicode(text);
    }
}

static void clickClient(HWND hwnd, int clientX, int clientY)
{
    POINT pt{ clientX, clientY };
    ClientToScreen(hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
    Sleep(30);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

static void clickFieldPercent(HWND hwnd, double xp, double yp, const char *why)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int x = int((rc.right - rc.left) * xp);
    const int y = int((rc.bottom - rc.top) * yp);
    qWarning() << "[EA] click" << why << "at" << x << y << "pct" << xp << yp;
    placeWindowForInput(hwnd);
    clickClient(hwnd, x, y);
}

static void typeIntoFocusedField(HWND hwnd, const QString &text, const char *label)
{
    placeWindowForInput(hwnd);
    Sleep(80);
    // Сначала клик уже сделан снаружи — чистим поле и печатаем unicode (надёжнее CEF, чем только Ctrl+V)
    sendCtrlA();
    Sleep(40);
    sendVk(VK_DELETE);
    Sleep(60);
    qWarning() << "[EA] type" << label << "unicode, len" << text.size();
    typeUnicode(text);
    Sleep(80);
    // Дубль через clipboard на случай, если unicode не попал в CEF
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        const std::wstring wstr = text.toStdWString();
        const size_t bytes = (wstr.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            memcpy(GlobalLock(hMem), wstr.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
            CloseClipboard();
            Sleep(40);
            sendCtrlA();
            Sleep(30);
            sendVk(VK_DELETE);
            Sleep(40);
            INPUT in[4] = {};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_CONTROL;
            in[1].type = INPUT_KEYBOARD;
            in[1].ki.wVk = 'V';
            in[2].type = INPUT_KEYBOARD;
            in[2].ki.wVk = 'V';
            in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[3].type = INPUT_KEYBOARD;
            in[3].ki.wVk = VK_CONTROL;
            in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, in, sizeof(INPUT));
            qWarning() << "[EA] also paste" << label << "via clipboard";
        } else {
            CloseClipboard();
        }
    }
}

static void setShellTopmostFrom(QObject *auth, bool enabled)
{
    if (auto *pm = qobject_cast<ProcessManager *>(auth->parent()))
        pm->setShellTopmost(enabled);
}

struct EaLoginEnumCtx {
    QSet<DWORD> eaPids;
    HWND bestLogin = nullptr;
    QString loginTitle;
    int loginScore = 0;
    int loginW = 0;
    int loginH = 0;
    HWND bestMain = nullptr;
    QString mainTitle;
    int mainW = 0;
    int mainH = 0;
    bool doDump = false;
};

static BOOL CALLBACK enumEaLoginProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<EaLoginEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!c->eaPids.contains(pid))
        return TRUE;

    wchar_t titleW[512] = {};
    GetWindowTextW(h, titleW, 511);
    const QString title = QString::fromWCharArray(titleW);
    wchar_t clsW[128] = {};
    GetClassNameW(h, clsW, 127);
    const QString cls = QString::fromWCharArray(clsW);

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    if (w < 200 || hgt < 180)
        return TRUE;

    const bool isEaUi = cls.contains(QStringLiteral("Qt"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("EADesktop"), Qt::CaseInsensitive)
                        || title.trimmed().compare(QStringLiteral("EA"), Qt::CaseInsensitive) == 0;
    if (!isEaUi)
        return TRUE;

    // Форма логина ~520×867. Главное окно библиотеки ~1536×832.
    const bool loginSized = (w >= 400 && w <= 700 && hgt >= 600 && hgt <= 1000);
    const bool mainSized = (w >= 900 && hgt >= 500);

    int score = 10;
    const QString tLow = title.toLower();
    if (tLow.contains(QStringLiteral("sign in"))
        || tLow.contains(QStringLiteral("log in"))
        || tLow.contains(QStringLiteral("вход"))
        || tLow.contains(QStringLiteral("login")))
        score += 40;
    if (loginSized)
        score += 80;

    if (c->doDump) {
        qWarning().nospace() << "[EA] candidate hwnd=0x" << Qt::hex << quintptr(h) << Qt::dec
                             << " size=" << w << "x" << hgt
                             << " loginSized=" << loginSized
                             << " mainSized=" << mainSized
                             << " score=" << score;
    }

    if (loginSized && score > c->loginScore) {
        c->loginScore = score;
        c->bestLogin = h;
        c->loginTitle = title;
        c->loginW = w;
        c->loginH = hgt;
    }
    if (mainSized) {
        // Берём самое большое главное окно
        const int area = w * hgt;
        const int prev = c->mainW * c->mainH;
        if (!c->bestMain || area >= prev) {
            c->bestMain = h;
            c->mainTitle = title;
            c->mainW = w;
            c->mainH = hgt;
        }
    }
    return TRUE;
}
#endif // Q_OS_WIN

EaAuth::EaAuth(QObject *parent)
    : IPlatformAuth(parent)
{
}

EaAuth::~EaAuth()
{
    stopScout();
}

void EaAuth::silentKill(const QString &image)
{
#ifdef Q_OS_WIN
    QProcess p;
    p.setProgram(QStringLiteral("taskkill"));
    p.setArguments({QStringLiteral("/F"), QStringLiteral("/T"),
                    QStringLiteral("/IM"), image});
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    p.start();
    p.waitForFinished(8000);
#else
    Q_UNUSED(image);
#endif
}

void EaAuth::killLauncher()
{
    qWarning() << "[EA] killLauncher: EADesktop + helpers";
    silentKill(QStringLiteral("EADesktop.exe"));
    silentKill(QStringLiteral("EABackgroundService.exe"));
    silentKill(QStringLiteral("EALauncher.exe"));
    silentKill(QStringLiteral("EALaunchHelper.exe"));
    silentKill(QStringLiteral("Link2EA.exe"));
    silentKill(QStringLiteral("EACefSubProcess.exe"));
    silentKill(QStringLiteral("Origin.exe"));
    silentKill(QStringLiteral("OriginWebHelperService.exe"));
}

bool EaAuth::applyCache(const QJsonObject &authData)
{
    const QString login = authData.value(QStringLiteral("login")).toString();
    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }
    if (exe.isEmpty())
        exe = defaultEaDesktopExe();
    m_launcherExe = exe;

    const QJsonObject vdf = extractVdfFiles(authData);
    QString blob = vdf.value(QStringLiteral("local_vdf")).toString();
    if (blob.trimmed().isEmpty())
        blob = vdf.value(QStringLiteral("config_vdf")).toString();

    if (blob.contains(QStringLiteral("ea_cache_version")) && unpackEaCacheBlob(blob)) {
        qWarning() << "[EA] applyCache: machine-cache OK для" << login;
        m_expectInteractive = false;
        m_allowsGameDetect = true;
        m_needBackup = false;
        return true;
    }

    qWarning() << "[EA] applyCache: нет machine-cache для" << login
               << "| local_vdf chars:" << blob.size();
    m_expectInteractive = true;
    m_allowsGameDetect = false;
    m_needBackup = true;
    return false;
}

void EaAuth::startLauncher(QProcess *process,
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
    if (exe.isEmpty())
        exe = defaultEaDesktopExe();

    QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();
    if (argsStr.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        argsStr = launcher.value(QStringLiteral("args")).toString().trimmed();
    }

    m_launcherExe = exe;
    // Полные args, в т.ч. "...||C:\\...\\game.exe" — не режем || здесь
    m_launchArgs = argsStr.trimmed();
    m_gameUriDeferred = false;

    const QString uriPart = normalizeEaGameUri(m_launchArgs);
    const bool isUri = uriPart.startsWith(QStringLiteral("origin2://"), Qt::CaseInsensitive)
                       || uriPart.startsWith(QStringLiteral("origin://"), Qt::CaseInsensitive)
                       || uriPart.startsWith(QStringLiteral("eadm://"), Qt::CaseInsensitive)
                       || !resolveDirectGameExe(m_launchArgs).isEmpty();

    // Cache: тоже EADesktop без URI — проверяем FSM; если сессия мёртвая — scout залогинит.
    // LaunchHelper+URI сразу при битом cache открывает login и scout раньше был выключен.
    if (!m_expectInteractive && isUri) {
        m_gameUriDeferred = true;
        const QFileInfo fi(exe);
        process->setWorkingDirectory(fi.absolutePath());
        resetLogWatch();
        qWarning().noquote() << "[EA] cache launch: EADesktop, URI после FSM-проверки:" << exe;
        process->start(exe, QStringList{});
        return;
    }

    // Interactive: сначала только EADesktop, URI после логина командой.
    const QFileInfo fi(exe);
    process->setWorkingDirectory(fi.absolutePath());
    QStringList args;
    if (m_expectInteractive && isUri) {
        m_gameUriDeferred = true;
        qWarning() << "[EA] interactive: старт без game URI, URI отложим после логина";
    } else if (isUri
               || uriPart.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
               || uriPart.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        // В process args URI не кладём — игра стартует после auth
        if (!m_gameUriDeferred && !uriPart.isEmpty())
            args << uriPart;
    } else if (!uriPart.isEmpty()) {
        args = QProcess::splitCommand(uriPart);
    }

    qWarning().noquote() << "[EA] Launch exe:" << exe;
    qWarning().noquote() << "[EA] Launch args:" << (args.isEmpty() ? QStringLiteral("(none)") : args.join(QLatin1Char(' ')));
    resetLogWatch();
    process->start(exe, args);
}

QString EaAuth::findEaDesktopLog()
{
    const QStringList dirs = {
        eaDesktopRoot() + QStringLiteral("/Logs"),
        QStringLiteral("C:/ProgramData/EA Desktop/Logs"),
        QStringLiteral("C:/ProgramData/Electronic Arts/EA Desktop/Logs"),
    };
    QString best;
    QDateTime bestMt;
    for (const QString &dir : dirs) {
        QDir d(dir);
        if (!d.exists())
            continue;
        // Только EADesktop.log / EADesktop.YYYY-MM-DD.log — без Verbose (шум + гигабайты)
        const QFileInfoList files = d.entryInfoList(
            QStringList{QStringLiteral("EADesktop.log"), QStringLiteral("EADesktop.*.log")},
            QDir::Files, QDir::Time);
        for (const QFileInfo &fi : files) {
            const QString name = fi.fileName();
            if (name.contains(QStringLiteral("Verbose"), Qt::CaseInsensitive))
                continue;
            if (!best.isEmpty() && fi.lastModified() <= bestMt)
                continue;
            best = fi.absoluteFilePath();
            bestMt = fi.lastModified();
        }
    }
    return QDir::toNativeSeparators(best);
}

const char *EaAuth::logAuthStateName(LogAuthState s)
{
    switch (s) {
    case LogAuthState::AwaitingAuth: return "awaitingAuthentication";
    case LogAuthState::Authenticated: return "authenticated";
    default: return "unknown";
    }
}

void EaAuth::applyEaLogLine(const QString &line)
{
    if (line.contains(QStringLiteral("-> awaitingAuthentication"))
        || line.contains(QStringLiteral("entering state awaitingAuthentication"))
        || line.contains(QStringLiteral("Requesting Auth Code for InitialUserLogin"))) {
        m_logAuth = LogAuthState::AwaitingAuth;
        m_logReadyForActions = false;
        m_sawAwaitingAuth = true;
        return;
    }
    if (line.contains(QStringLiteral("-> authenticated"))
        || line.contains(QStringLiteral("entering state authenticated"))) {
        m_logAuth = LogAuthState::Authenticated;
        return;
    }
    if (line.contains(QStringLiteral("DesktopFSM[awaitingAuthentication]"))) {
        m_logAuth = LogAuthState::AwaitingAuth;
        m_sawAwaitingAuth = true;
        return;
    }
    if (line.contains(QStringLiteral("DesktopFSM[authenticated]"))) {
        m_logAuth = LogAuthState::Authenticated;
    }
    if (m_logAuth == LogAuthState::Authenticated
        && line.contains(QStringLiteral("EventReadyForExternalActions"))) {
        m_logReadyForActions = true;
    }
}

void EaAuth::ingestEaLogChunk(const QByteArray &chunkIn)
{
    if (chunkIn.isEmpty() && m_logCarry.isEmpty())
        return;

    QByteArray chunk = m_logCarry;
    chunk.append(chunkIn);
    m_logCarry.clear();

    int from = 0;
    while (from < chunk.size()) {
        const int nl = chunk.indexOf('\n', from);
        if (nl < 0) {
            m_logCarry = chunk.mid(from);
            break;
        }
        QByteArray raw = chunk.mid(from, nl - from);
        if (!raw.isEmpty() && raw.endsWith('\r'))
            raw.chop(1);
        if (!raw.isEmpty())
            applyEaLogLine(QString::fromUtf8(raw));
        from = nl + 1;
    }
}

void EaAuth::resetLogWatch()
{
    m_logAuth = LogAuthState::Unknown;
    m_logReadyForActions = false;
    m_sawAwaitingAuth = false;
    m_logOffset = 0;
    m_logCarry.clear();
    m_logPath = findEaDesktopLog();
    if (m_logPath.isEmpty()) {
        qWarning() << "[EA] log watch: EADesktop.log не найден";
        return;
    }

    QFile f(m_logPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[EA] log watch: не открыть" << m_logPath << f.errorString();
        return;
    }

    // Только новые строки: хвост файла содержит FSM прошлой сессии EA и врёт.
    m_logOffset = f.size();
    f.close();

    qWarning() << "[EA] log watch: live-tail" << m_logPath << "from offset" << m_logOffset;
}

void EaAuth::pollEaLogs()
{
    if (m_logPath.isEmpty()) {
        m_logPath = findEaDesktopLog();
        if (m_logPath.isEmpty())
            return;
        QFile f0(m_logPath);
        if (f0.open(QIODevice::ReadOnly)) {
            m_logOffset = f0.size();
            f0.close();
        }
        qWarning() << "[EA] log watch: подключили" << m_logPath << "offset" << m_logOffset;
    }

    QFile f(m_logPath);
    if (!f.open(QIODevice::ReadOnly))
        return;

    const qint64 sz = f.size();
    if (sz < m_logOffset) {
        m_logOffset = 0;
        m_logCarry.clear();
    }
    if (sz == m_logOffset) {
        f.close();
        return;
    }

    f.seek(m_logOffset);
    const QByteArray chunk = f.readAll();
    m_logOffset = f.pos();
    f.close();
    if (chunk.isEmpty())
        return;

    const LogAuthState prev = m_logAuth;
    const bool prevReady = m_logReadyForActions;
    ingestEaLogChunk(chunk);
    if (m_logAuth != prev || m_logReadyForActions != prevReady) {
        qWarning() << "[EA] log FSM →" << logAuthStateName(m_logAuth)
                   << "readyExt:" << m_logReadyForActions;
    }
}

void EaAuth::stopScout()
{
    if (!m_scoutTimer)
        return;
    m_scoutTimer->stop();
    m_scoutTimer->deleteLater();
    m_scoutTimer = nullptr;
}

void EaAuth::finishScoutSuccess(const QString &why)
{
    qWarning() << "[EA] Scout OK:" << why;
    m_phase = Phase::Done;
    m_allowsGameDetect = false;
    m_needBackup = true;
    stopScout();

    QTimer::singleShot(120000, this, [this]() {
        if (m_phase == Phase::Done)
            m_allowsGameDetect = true;
    });

    if (!m_gameUriDeferred)
        return;
    m_gameUriDeferred = false;

    // Живой EA залогинен → игра напрямую .exe (origin2:// врёт EntitlementNotFound).
    const int uriDelayMs = m_passwordSent ? 6000 : 3000;
    qWarning() << "[EA] старт игры через" << uriDelayMs << "мс (прямой exe предпочтительнее URI)"
               << m_launchArgs;
    QTimer::singleShot(uriDelayMs, this, [this]() {
        fireGameUri();
    });
}

QString EaAuth::normalizeEaGameUri(const QString &args)
{
    // Поддержка: "origin2://...?offerIds=...||C:\\...\\game.exe"
    const int sep = args.indexOf(QStringLiteral("||"));
    if (sep >= 0)
        return args.left(sep).trimmed();
    return args.trimmed();
}

QString EaAuth::resolveDirectGameExe(const QString &args)
{
    const QString trimmed = args.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Явный путь: ...||C:\Program Files\EA Games\SWGoH\SWGoH.exe
    const int sep = trimmed.indexOf(QStringLiteral("||"));
    if (sep >= 0) {
        const QString path = QDir::toNativeSeparators(trimmed.mid(sep + 2).trimmed());
        if (QFileInfo::exists(path) && path.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
            return path;
        qWarning() << "[EA] прямой exe после || не найден:" << path;
    }

    // Args сами по себе — путь к игре
    if (trimmed.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
        && !trimmed.contains(QStringLiteral("://"))
        && QFileInfo::exists(trimmed)) {
        return QDir::toNativeSeparators(trimmed);
    }

    // Авто: известные установки под EA Games (обход сломанного origin2 → contentIds)
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/EA Games/SWGoH/SWGoH.exe"),
        QStringLiteral("C:/Program Files (x86)/EA Games/SWGoH/SWGoH.exe"),
        QStringLiteral("D:/Program Files/EA Games/SWGoH/SWGoH.exe"),
        QStringLiteral("D:/EA Games/SWGoH/SWGoH.exe"),
    };
    const bool looksSwgoh = trimmed.contains(QStringLiteral("OFR.50.0005369"), Qt::CaseInsensitive)
                            || trimmed.contains(QStringLiteral("1012586"))
                            || trimmed.contains(QStringLiteral("SWGoH"), Qt::CaseInsensitive)
                            || trimmed.contains(QStringLiteral("galaxy-of-heroes"), Qt::CaseInsensitive);
    if (looksSwgoh) {
        for (const QString &c : candidates) {
            if (QFileInfo::exists(c))
                return QDir::toNativeSeparators(c);
        }
    }

    // Общий поиск: C:\Program Files\EA Games\<Game>\<Game>.exe
    const QStringList roots = {
        QStringLiteral("C:/Program Files/EA Games"),
        QStringLiteral("C:/Program Files (x86)/EA Games"),
        QStringLiteral("D:/Program Files/EA Games"),
    };
    for (const QString &root : roots) {
        QDir d(root);
        if (!d.exists())
            continue;
        const QFileInfoList dirs = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &di : dirs) {
            const QString guess = di.absoluteFilePath() + QLatin1Char('/') + di.fileName()
                                  + QStringLiteral(".exe");
            if (QFileInfo::exists(guess)) {
                // Если в URI один конкретный OFR/slug — не берём первый попавшийся
                if (looksSwgoh
                    && !guess.contains(QStringLiteral("SWGoH"), Qt::CaseInsensitive))
                    continue;
                if (looksSwgoh || !trimmed.contains(QStringLiteral("origin2://"), Qt::CaseInsensitive))
                    return QDir::toNativeSeparators(guess);
            }
        }
    }
    return {};
}

QStringList EaAuth::directGameExtraArgs(const QString &gameExe)
{
    // Из лога EA при Play: SWGoH.exe args=[ -test]
    if (gameExe.contains(QStringLiteral("SWGoH.exe"), Qt::CaseInsensitive))
        return {QStringLiteral("-test")};
    return {};
}

QString EaAuth::eaLauncherBeside(const QString &eaDesktopExe)
{
    if (eaDesktopExe.isEmpty())
        return {};
    QString p = eaDesktopExe;
    p.replace(QStringLiteral("EADesktop.exe"), QStringLiteral("EALauncher.exe"),
              Qt::CaseInsensitive);
    if (QFileInfo::exists(p))
        return QDir::toNativeSeparators(p);
    const QString beside = QFileInfo(eaDesktopExe).absolutePath()
                           + QStringLiteral("/EALauncher.exe");
    if (QFileInfo::exists(beside))
        return QDir::toNativeSeparators(beside);
    return {};
}

void EaAuth::fireGameUri()
{
#ifdef Q_OS_WIN
    if (m_launchArgs.trimmed().isEmpty()) {
        qWarning() << "[EA] fireGameUri: args пусты";
        return;
    }

    // 1) Прямой .exe — как ручной Play, без кривого origin2/contentIds
    const QString gameExe = resolveDirectGameExe(m_launchArgs);
    if (!gameExe.isEmpty()) {
        const QString workDir = QFileInfo(gameExe).absolutePath();
        const QStringList gargs = directGameExtraArgs(gameExe);
        const bool ok = QProcess::startDetached(gameExe, gargs, workDir);
        qWarning().noquote() << "[EA] DIRECT game exe (обход origin2):" << ok
                             << gameExe << gargs.join(QLatin1Char(' '))
                             << "| cwd:" << workDir;
        return;
    }

    QString uri = normalizeEaGameUri(m_launchArgs);
    qWarning() << "[EA] прямой exe не найден — fallback origin2:// (может быть EntitlementNotFound)"
               << uri;

    const HINSTANCE r = ShellExecuteW(
        nullptr, L"open",
        reinterpret_cast<LPCWSTR>(uri.utf16()),
        nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = int(reinterpret_cast<quintptr>(r));
    qWarning() << "[EA] game URI ShellExecute:" << code << uri;
    if (code > 32)
        return;

    const QString launcher = eaLauncherBeside(m_launcherExe);
    if (!launcher.isEmpty()) {
        const bool ok = QProcess::startDetached(
            launcher, QStringList{uri}, QFileInfo(launcher).absolutePath());
        qWarning() << "[EA] game URI через EALauncher.exe:" << ok << launcher << uri;
        return;
    }

    qWarning() << "[EA] fireGameUri FAIL: нет прямого exe, ShellExecute" << code
               << "EALauncher не найден";
#else
    Q_UNUSED(m_launchArgs);
#endif
}

void EaAuth::relaunchGameArgs()
{
#ifdef Q_OS_WIN
    if (m_launchArgs.isEmpty()) {
        qWarning() << "[EA] relaunch: args пусты";
        return;
    }

    const QString uriPart = normalizeEaGameUri(m_launchArgs);
    const bool isUri = uriPart.startsWith(QStringLiteral("origin2://"), Qt::CaseInsensitive)
                       || uriPart.startsWith(QStringLiteral("origin://"), Qt::CaseInsensitive)
                       || uriPart.startsWith(QStringLiteral("eadm://"), Qt::CaseInsensitive)
                       || !resolveDirectGameExe(m_launchArgs).isEmpty();

    if (!isUri && !uriPart.contains(QStringLiteral("://"))) {
        if (!m_launcherExe.isEmpty()) {
            auto *p = new QProcess;
            p->setProgram(m_launcherExe);
            p->setArguments(QProcess::splitCommand(uriPart));
            p->setWorkingDirectory(QFileInfo(m_launcherExe).absolutePath());
            QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                             p, &QObject::deleteLater);
            p->start();
            qWarning() << "[EA] relaunch exe+args";
        }
        return;
    }

    fireGameUri();
#else
    Q_UNUSED(m_launchArgs);
#endif
}

void EaAuth::injectEmail(quintptr hwndVal, const QString &email)
{
#ifdef Q_OS_WIN
    if (!m_scoutTimer || m_phase == Phase::Done)
        return;
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return;

    AllowSetForegroundWindow(ASFW_ANY);
    setShellTopmostFrom(this, false);
    placeWindowForegroundOnly(hwnd);
    // Клик в шапку формы → фокус в CEF (без SetFocus на HWND, иначе Tab мимо поля)
    clickFieldPercent(hwnd, 0.50, 0.12, "activate CEF (не поле)");

    // Tab: 6 = email, 11 = Далее
    QTimer::singleShot(900, this, [this, hwndVal, email]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;
        AllowSetForegroundWindow(ASFW_ANY);
        placeWindowForegroundOnly(h);
        sendTabs(6, "→ поле email (Tab #6)");
        QTimer::singleShot(400, this, [this, hwndVal, email]() {
            HWND h2 = reinterpret_cast<HWND>(hwndVal);
            if (!h2 || !IsWindow(h2) || !m_scoutTimer)
                return;
            placeWindowForegroundOnly(h2);
            qWarning() << "[EA] type email unicode, len" << email.size();
            typeUnicode(email);
            QTimer::singleShot(600, this, [this, hwndVal]() {
                HWND h3 = reinterpret_cast<HWND>(hwndVal);
                if (!h3 || !IsWindow(h3) || !m_scoutTimer)
                    return;
                placeWindowForegroundOnly(h3);
                sendTabs(5, "→ Далее (Tab #11)");
                Sleep(200);
                sendReturnKey();
                qWarning() << "[EA] Enter на Далее — ждём пароль";
                m_phase = Phase::WaitPassword;
                m_phaseTick = m_ticks;
            });
        });
    });
#else
    Q_UNUSED(hwndVal);
    Q_UNUSED(email);
#endif
}

void EaAuth::injectPassword(quintptr hwndVal, const QString &password)
{
#ifdef Q_OS_WIN
    if (!m_scoutTimer || m_phase == Phase::Done)
        return;
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return;

    AllowSetForegroundWindow(ASFW_ANY);
    setShellTopmostFrom(this, false);
    placeWindowForegroundOnly(hwnd);

    // Второе окно: фокус уже в пароле — не кликаем и не Tab до поля
    QTimer::singleShot(700, this, [this, hwndVal, password]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;
        AllowSetForegroundWindow(ASFW_ANY);
        placeWindowForegroundOnly(h);
        qWarning() << "[EA] type password unicode, len" << password.size();
        typeUnicode(password);

        QTimer::singleShot(500, this, [this, hwndVal]() {
            HWND h2 = reinterpret_cast<HWND>(hwndVal);
            if (!h2 || !IsWindow(h2) || !m_scoutTimer)
                return;
            placeWindowForegroundOnly(h2);
            sendTabs(2, "→ Войти (Tab #2 с поля)");
            Sleep(200);
            sendReturnKey();
            m_needBackup = true;
            m_phase = Phase::PasswordSubmitted;
            m_phaseTick = m_ticks;
            m_errorBackClicked = false;
            qWarning() << "[EA] Enter на Войти — ждём закрытия формы";
        });
    });
#else
    Q_UNUSED(hwndVal);
    Q_UNUSED(password);
#endif
}

void EaAuth::clickBackOnError(quintptr hwndVal)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return;
    setShellTopmostFrom(this, false);
    placeWindowForInput(hwnd);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // Синяя кнопка «НАЗАД» внизу диалога ошибки
    const int x = (rc.right - rc.left) / 2;
    const int y = int((rc.bottom - rc.top) * 0.88);
    qWarning() << "[EA] click НАЗАД (ошибка IP/входа)" << x << y;
    clickClient(hwnd, x, y);
#else
    Q_UNUSED(hwndVal);
#endif
}

void EaAuth::startScout(const QString &login, const QString &password)
{
#ifdef Q_OS_WIN
    stopScout();
    m_ticks = 0;
    m_phaseTick = 0;
    m_loginRetries = 0;
    m_phase = Phase::WaitLoginWindow;
    m_emailSent = false;
    m_passwordSent = false;
    m_errorBackClicked = false;
    // offset уже выставлен в startLauncher; повторный seek EOF срежет ранний FSM
    if (m_logPath.isEmpty())
        resetLogWatch();

    // Без пароля — только ждать FSM/URI (редко)
    if (login.isEmpty() || password.isEmpty()) {
        qWarning() << "[EA] Scout: нет login/password — только URI по FSM";
        if (!m_expectInteractive && m_gameUriDeferred) {
            // cache без пароля: если уже authenticated — URI
            m_allowsGameDetect = false;
        } else {
            m_allowsGameDetect = true;
            m_phase = Phase::Done;
            stopScout();
            return;
        }
    }

    m_allowsGameDetect = false;
    m_needBackup = false;
    qWarning() << "[EA] Scout START"
               << (m_expectInteractive ? "(interactive)" : "(soft-silent: cache verify + login fallback)")
               << "login:" << login;

    m_scoutTimer = new QTimer(this);
    m_scoutTimer->setInterval(400);

    connect(m_scoutTimer, &QTimer::timeout, this, [this, login, password]() {
        if (!m_scoutTimer)
            return;

        ++m_ticks;
        pollEaLogs();

        if (m_ticks > 450) {
            qWarning() << "[EA] Scout TIMEOUT — fallback relaunch"
                       << "logFSM:" << logAuthStateName(m_logAuth);
            relaunchGameArgs();
            m_allowsGameDetect = true;
            if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                pm->showShellAfterGame();
            stopScout();
            return;
        }

        if (m_phase == Phase::Done) {
            stopScout();
            return;
        }

        EaLoginEnumCtx ctx;
        ctx.eaPids = collectEaPids();
        ctx.doDump = (m_ticks == 5 || m_ticks == 20 || m_ticks == 40 || m_ticks == 80);
        EnumWindows(enumEaLoginProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.doDump) {
            qWarning() << "[EA] scout tick" << m_ticks
                       << "| phase:" << int(m_phase)
                       << "| logFSM:" << logAuthStateName(m_logAuth)
                       << "readyExt:" << m_logReadyForActions
                       << "| login:" << (ctx.bestLogin
                                             ? QStringLiteral("%1x%2").arg(ctx.loginW).arg(ctx.loginH)
                                             : QStringLiteral("(none)"))
                       << "| main:" << (ctx.bestMain
                                            ? QStringLiteral("%1x%2").arg(ctx.mainW).arg(ctx.mainH)
                                            : QStringLiteral("(none)"));
        }

        const bool hasLogin = ctx.bestLogin != nullptr;
        const bool hasMain = ctx.bestMain != nullptr;
        const bool logAuthOk = (m_logAuth == LogAuthState::Authenticated);
        const bool logNeedsLogin = (m_logAuth == LogAuthState::AwaitingAuth);
        const bool canType = !login.isEmpty() && !password.isEmpty();

        // Cache живой / уже вошли — только URI
        if (m_phase == Phase::WaitLoginWindow && !m_emailSent && logAuthOk) {
            if (m_logReadyForActions || hasMain || m_ticks >= 40) {
                m_emailSent = true;
                finishScoutSuccess(m_expectInteractive
                    ? QStringLiteral("log: authenticated — URI")
                    : QStringLiteral("log: cache OK (authenticated) — URI"));
                return;
            }
        }

        // После пароля — не шлём URI, пока login dialog ещё на экране
        if (m_phase == Phase::PasswordSubmitted && logAuthOk) {
            if (hasLogin) {
                if (m_ticks == m_phaseTick + 5)
                    qWarning() << "[EA] authenticated, ждём закрытия login dialog…";
                return;
            }
            if (hasMain || m_logReadyForActions || m_ticks >= m_phaseTick + 40) {
                finishScoutSuccess(QStringLiteral("log: authenticated, login dialog закрыт"));
                return;
            }
            return;
        }

        if (m_phase == Phase::WaitLoginWindow && !m_emailSent && canType) {
            // Cache мёртвый / нужен логин — Tab на login dialog
            if ((logNeedsLogin || (!m_expectInteractive && hasLogin))
                && hasLogin && m_ticks >= 25) {
                m_emailSent = true;
                m_phase = Phase::EmailSubmitted;
                m_phaseTick = m_ticks;
                qWarning() << "[EA] login fallback (dialog):" << ctx.loginTitle
                           << "size" << ctx.loginW << "x" << ctx.loginH
                           << "logFSM:" << logAuthStateName(m_logAuth)
                           << (m_expectInteractive ? "interactive" : "soft-silent");
                injectEmail(reinterpret_cast<quintptr>(ctx.bestLogin), login);
                return;
            }
            if (m_logAuth == LogAuthState::Unknown && hasMain && !hasLogin && m_ticks >= 60) {
                m_emailSent = true;
                finishScoutSuccess(QStringLiteral("UI: main без login — URI"));
                return;
            }
            return;
        }

        if (m_phase == Phase::WaitPassword && hasLogin && !m_passwordSent && canType
            && m_ticks >= m_phaseTick + 15) {
            m_passwordSent = true;
            m_phaseTick = m_ticks;
            qWarning() << "[EA] экран пароля (login dialog)";
            injectPassword(reinterpret_cast<quintptr>(ctx.bestLogin), password);
            return;
        }

        if (m_phase == Phase::PasswordSubmitted) {
            if (hasLogin)
                return;
            if (hasMain && m_ticks >= m_phaseTick + 25) {
                finishScoutSuccess(QStringLiteral("главное окно EA после логина"));
                return;
            }
            if (!hasLogin && m_ticks >= m_phaseTick + 50) {
                finishScoutSuccess(QStringLiteral("форма логина исчезла"));
                return;
            }
        }

        if (m_phase == Phase::WaitLoginGone && m_ticks >= m_phaseTick + 8) {
            ++m_loginRetries;
            m_emailSent = false;
            m_passwordSent = false;
            m_errorBackClicked = false;
            m_phase = Phase::WaitLoginWindow;
            m_phaseTick = m_ticks;
            qWarning() << "[EA] retry логина #" << m_loginRetries;
            return;
        }
    });

    m_scoutTimer->start();
#else
    Q_UNUSED(login);
    Q_UNUSED(password);
    m_allowsGameDetect = true;
#endif
}

void EaAuth::backupCache(NetworkManager *net, int terminalId, const QString &login)
{
    if (login.isEmpty()) {
        qWarning() << "[EA] cache backup: login пуст";
        return;
    }
    if (!net || net->serverUrl().isEmpty()) {
        qWarning() << "[EA] cache backup: serverUrl пуст";
        return;
    }

    const QString blob = packEaCacheBlob();
    if (blob.trimmed().isEmpty()) {
        qWarning() << "[EA] cache backup: сессия не найдена в" << eaDesktopRoot();
        return;
    }

    QJsonObject rootPayload;
    rootPayload.insert(QStringLiteral("login"), login);
    rootPayload.insert(QStringLiteral("terminal_id"), terminalId);
    rootPayload.insert(QStringLiteral("platform"), QStringLiteral("ea"));
    rootPayload.insert(QStringLiteral("local_vdf"), blob);
    rootPayload.insert(QStringLiteral("config_vdf"), blob);

    QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qWarning() << "[EA] cache backup → server, bytes:" << jsonData.size()
               << "login:" << login << "terminal:" << terminalId;

    QNetworkReply *reply = net->networkAccessManager()->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            qWarning() << "[EA] cache backup OK" << body.left(200);
        else
            qWarning() << "[EA] cache backup fail:" << reply->errorString() << body.left(200);
    });
}
