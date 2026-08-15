#include "eaauth.h"
#include "processmanager.h"
#include "networkmanager.h"
#include "pathresolver.h"

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
#include <QThread>
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
    QStringList candidates;
    if (PathResolver *paths = PathResolver::instance()) {
        if (!paths->eaPath().isEmpty())
            candidates << paths->eaPath();
        candidates = paths->expandLauncherCandidates({
            QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe"),
            QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EADesktop.exe"),
        });
    } else {
        candidates = {
            QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe"),
            QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EADesktop.exe"),
        };
    }
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

// Club interactive scout: login остаётся on-screen (CEF Submit жив), оверлей TOPMOST его кроет.
// Off-screen park (-20000) ломает CEF focus — Enter на Войти не регистрируется.
// Важно: во время typing/Tab/Enter НЕ поднимать shell TOPMOST сразу — иначе Enter не доходит до CEF.

void EaAuth::keepOverlayUp(bool force)
{
    if (!force) {
        if (m_sendInputBusy)
            return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now < m_overlayHoldOffUntilMs)
            return;
        // Idle waiting: rate-limit 1.5s — не спамить SetForeground/TOPMOST каждый tick.
        if (m_lastOverlayRaiseMs > 0 && (now - m_lastOverlayRaiseMs) < 1500)
            return;
        m_lastOverlayRaiseMs = now;
    } else {
        m_lastOverlayRaiseMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->setShellTopmost(true);
}

void EaAuth::beginInjectBurst()
{
    m_sendInputBusy = true;
    // Держим hold-off пока burst не закончится endInjectBurstRestoreOverlay.
    m_overlayHoldOffUntilMs = QDateTime::currentMSecsSinceEpoch() + 120000;
}

void EaAuth::endInjectBurstRestoreOverlay(int delayMs)
{
    const int ms = qMax(300, delayMs);
    m_overlayHoldOffUntilMs = QDateTime::currentMSecsSinceEpoch() + ms;
    QTimer::singleShot(ms, this, [this]() {
        m_sendInputBusy = false;
        keepOverlayUp(true);
        qWarning() << "[EA] overlay TOPMOST restored after Enter settle";
    });
}

// Foreground для SendInput без HWND_TOPMOST / SW_RESTORE — иначе login всплывает поверх loading.
// НЕ поднимаем shell TOPMOST здесь: во время inject burst фокус должен остаться на EA CEF.
static void placeWindowForScoutInput(HWND hwnd, QObject *auth, bool setFocus)
{
    Q_UNUSED(auth);
    if (!hwnd || !IsWindow(hwnd))
        return;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    AllowSetForegroundWindow(pid);
    const DWORD ourTid = GetCurrentThreadId();
    const DWORD foreTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, TRUE);
    if (tid && tid != ourTid)
        AttachThreadInput(ourTid, tid, TRUE);
    SetForegroundWindow(hwnd);
    if (setFocus) {
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
    }
    if (tid && tid != ourTid)
        AttachThreadInput(ourTid, tid, FALSE);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, FALSE);
}

static void placeWindowForInput(HWND hwnd, QObject *auth)
{
    placeWindowForScoutInput(hwnd, auth, true);
}

// Нельзя SetFocus на HWND EA — сбивает фокус с CEF-поля (пароль уже сфокусирован).
static void placeWindowForegroundOnly(HWND hwnd, QObject *auth)
{
    placeWindowForScoutInput(hwnd, auth, false);
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

static void clearAndType(HWND hwnd, QObject *auth, const QString &text)
{
    placeWindowForInput(hwnd, auth);
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

static void clickFieldPercent(HWND hwnd, QObject *auth, double xp, double yp, const char *why)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int x = int((rc.right - rc.left) * xp);
    const int y = int((rc.bottom - rc.top) * yp);
    qWarning() << "[EA] click" << why << "at" << x << y << "pct" << xp << yp;
    // Только foreground EA — без немедленного TOPMOST shell (клик/фокус уходят в CEF).
    placeWindowForInput(hwnd, auth);
    clickClient(hwnd, x, y);
}

static void typeIntoFocusedField(HWND hwnd, QObject *auth, const QString &text, const char *label)
{
    placeWindowForInput(hwnd, auth);
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
    stopLibraryReadyWatch();
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
    qWarning() << "[EA] killLauncher: EADesktop + helpers (taskkill /F /T)";
    const QStringList images = {
        QStringLiteral("EADesktop.exe"),
        QStringLiteral("EABackgroundService.exe"),
        QStringLiteral("EALauncher.exe"),
        QStringLiteral("EALaunchHelper.exe"),
        QStringLiteral("Link2EA.exe"),
        QStringLiteral("EACefSubProcess.exe"),
        QStringLiteral("EALocalHostSvc.exe"),
        QStringLiteral("EAConnect_microsoft.exe"),
        QStringLiteral("Origin.exe"),
        QStringLiteral("OriginWebHelperService.exe"),
        QStringLiteral("OriginThinSetupInternal.exe"),
        QStringLiteral("IGOProxy32.exe"),
    };
    for (const QString &image : images)
        silentKill(image);
}

static bool eaImageRunning(const QString &image)
{
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (QString::fromWCharArray(pe.szExeFile).compare(image, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    Q_UNUSED(image);
    return false;
#endif
}

static bool eaAnyProcessAlive()
{
    const QStringList images = {
        QStringLiteral("EADesktop.exe"),
        QStringLiteral("EABackgroundService.exe"),
        QStringLiteral("EALauncher.exe"),
        QStringLiteral("Link2EA.exe"),
        QStringLiteral("EACefSubProcess.exe"),
        QStringLiteral("Origin.exe"),
        QStringLiteral("OriginWebHelperService.exe"),
    };
    for (const QString &image : images) {
        if (eaImageRunning(image))
            return true;
    }
    return false;
}

static void killEaAndWait(EaAuth *self, int timeoutMs)
{
    if (!self)
        return;
    self->killLauncher();
#ifdef Q_OS_WIN
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    int rounds = 0;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (!eaAnyProcessAlive()) {
            qWarning() << "[EA] killAndWait: процессы EA/Origin завершены после" << rounds
                       << "раундов";
            QThread::msleep(500);
            // Повторная проверка — сервисы иногда поднимаются снова
            if (!eaAnyProcessAlive())
                return;
            qWarning() << "[EA] killAndWait: процесс снова жив — ещё taskkill";
        }
        self->killLauncher();
        ++rounds;
        QThread::msleep(400);
    }
    qWarning() << "[EA] WARN: killAndWait timeout" << timeoutMs << "ms — wipe всё равно";
#else
    Q_UNUSED(timeoutMs);
#endif
}

static void wipePathLogged(const QString &path, const QString &kind, QStringList *cleared)
{
    if (!cleared)
        return;
    const QFileInfo fi(path);
    if (!fi.exists())
        return;
    bool ok = false;
    if (fi.isDir())
        ok = QDir(path).removeRecursively();
    else
        ok = QFile::remove(path);
    if (ok) {
        *cleared << (kind + QLatin1Char(':') + path);
        qWarning().noquote() << "[EA] personal wipe: removed" << kind << path;
    } else {
        *cleared << (kind + QStringLiteral(":FAILED:") + path);
        qWarning().noquote() << "[EA] personal wipe: FAILED" << kind << path;
    }
}

static QString eaOriginRoot()
{
    return eaLocalAppData() + QStringLiteral("/Origin");
}

static QStringList wipeEaPersonalSession()
{
    QStringList cleared;
    const QString root = eaDesktopRoot();
    QDir rootDir(root);
    if (rootDir.exists()) {
        // Жёсткий logout: почти всё под EA Desktop, Logs оставляем для диагностики
        const QFileInfoList entries = rootDir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &fi : entries) {
            if (fi.fileName().compare(QStringLiteral("Logs"), Qt::CaseInsensitive) == 0)
                continue;
            wipePathLogged(fi.absoluteFilePath(),
                           fi.isDir() ? QStringLiteral("ea-dir") : QStringLiteral("ea-file"),
                           &cleared);
        }
        // Явно добить типичные session/token пути (если parent wipe частично FAIL)
        // IGOCache / content ownership — только personal wipe (не club applyCache)
        const QStringList extra = {
            QStringLiteral("SEC"),
            QStringLiteral("IGOCache"),
            QStringLiteral("OfflineCache"),
            QStringLiteral("CEF"),
            QStringLiteral("CEF/BrowserCache"),
            QStringLiteral("CEF/BrowserCache/EADesktop"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Network"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Local Storage"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Cookies"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Sessions"),
            QStringLiteral("CEF/BrowserCache/EADesktop/IndexedDB"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Service Worker"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Cache"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Code Cache"),
            QStringLiteral("CEF/BrowserCache/EADesktop/GPUCache"),
            QStringLiteral("CEF/BrowserCache/EADesktop/Session Storage"),
        };
        for (const QString &sub : extra)
            wipePathLogged(root + QLatin1Char('/') + sub, QStringLiteral("ea-dir"), &cleared);

        const QFileInfoList users = rootDir.entryInfoList(
            QStringList{QStringLiteral("user_*.ini"), QStringLiteral("*.db"),
                        QStringLiteral("*.sqlite"), QStringLiteral("*.sqlite3")},
            QDir::Files | QDir::Hidden | QDir::System);
        for (const QFileInfo &fi : users)
            wipePathLogged(fi.absoluteFilePath(), QStringLiteral("ea-file"), &cleared);
    }

    // Origin legacy cache (если ещё есть на машине)
    const QString origin = eaOriginRoot();
    if (QDir(origin).exists()) {
        const QStringList originKill = {
            QStringLiteral("local.xml"),
            QStringLiteral("login.json"),
            QStringLiteral(".local"),
            QStringLiteral("SSO"),
            QStringLiteral("CachedData"),
            QStringLiteral("Cache"),
            QStringLiteral("Cookies"),
            QStringLiteral("Sessions"),
            QStringLiteral("Local Storage"),
            QStringLiteral("Session Storage"),
            QStringLiteral("IndexedDB"),
            QStringLiteral("Service Worker"),
            QStringLiteral("GPUCache"),
            QStringLiteral("Code Cache"),
        };
        for (const QString &sub : originKill) {
            const QString abs = origin + QLatin1Char('/') + sub;
            const QFileInfo fi(abs);
            wipePathLogged(abs, fi.isDir() || (!fi.exists() && !sub.contains(QLatin1Char('.')))
                                    ? QStringLiteral("origin-dir")
                                    : QStringLiteral("origin-file"),
                           &cleared);
        }
        QDir originDir(origin);
        const QFileInfoList oUsers = originDir.entryInfoList(
            QStringList{QStringLiteral("user_*.ini"), QStringLiteral("*.db")},
            QDir::Files | QDir::Hidden | QDir::System);
        for (const QFileInfo &fi : oUsers)
            wipePathLogged(fi.absoluteFilePath(), QStringLiteral("origin-file"), &cleared);
    }

    return cleared;
}

static void clearEaLocalSession()
{
    const QStringList cleared = wipeEaPersonalSession();
    for (const QString &item : cleared)
        qWarning() << "[EA] cleared session:" << item;
}

bool EaAuth::applyCache(const QJsonObject &authData)
{
    const QString login = authData.value(QStringLiteral("login")).toString();
    const QString password = authData.value(QStringLiteral("password")).toString();
    const QString mode = authData.value(QStringLiteral("auth")).toObject()
                             .value(QStringLiteral("mode")).toString();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    m_gameTitle = authData.value(QStringLiteral("game_title")).toString().trimmed();
    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }
    if (exe.isEmpty())
        exe = defaultEaDesktopExe();
    m_launcherExe = exe;

    const bool personal = (mode == QLatin1String("personal"))
        || (platformSource.compare(QStringLiteral("personal_account"), Qt::CaseInsensitive) == 0)
        || login.trimmed().isEmpty() || password.isEmpty();
    m_personalLaunch = personal;
    m_personalEarlyAuthWarned = false;
    m_gameUriDeferred = false;

    if (personal) {
        qWarning() << "[EA] applyCache: personal — aggressive kill + full logout wipe"
                   << "| title:" << (m_gameTitle.isEmpty() ? QStringLiteral("(none)") : m_gameTitle);
        killEaAndWait(this, 20000);
        QStringList cleared = wipeEaPersonalSession();
        // user_*.ini ещё на месте → процессы держали файл; kill+wipe ещё раз
        const QString userIniGlob = eaDesktopRoot();
        const bool userIniLeft = QDir(userIniGlob).exists()
            && !QDir(userIniGlob).entryList(QStringList{QStringLiteral("user_*.ini")},
                                            QDir::Files).isEmpty();
        const bool secLeft = QDir(eaDesktopRoot() + QStringLiteral("/SEC")).exists();
        if (userIniLeft || secLeft) {
            qWarning() << "[EA] personal: session remnants after wipe — kill+wipe retry"
                       << "user_ini:" << userIniLeft << "SEC:" << secLeft;
            killEaAndWait(this, 12000);
            cleared += wipeEaPersonalSession();
        }
        qWarning().noquote() << "[EA] personal: full logout wipe |"
                             << (cleared.isEmpty() ? QStringLiteral("(nothing found)")
                                                   : cleared.join(QStringLiteral(", ")));
        // Не soft-silent: credentials нет, scout/URI/DIRECT запрещены политикой personal
        m_expectInteractive = false;
        m_allowsGameDetect = true;
        m_needBackup = false;
        return true;
    }

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

    const QString mode = authData.value(QStringLiteral("auth")).toObject()
                             .value(QStringLiteral("mode")).toString();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    const QString login = authData.value(QStringLiteral("login")).toString().trimmed();
    if (!m_personalLaunch) {
        m_personalLaunch = (mode == QLatin1String("personal"))
            || (platformSource.compare(QStringLiteral("personal_account"),
                                       Qt::CaseInsensitive) == 0)
            || login.isEmpty();
    }
    if (m_gameTitle.isEmpty())
        m_gameTitle = authData.value(QStringLiteral("game_title")).toString().trimmed();

    m_launcherExe = exe;
    // Полные args, в т.ч. "...||C:\\...\\game.exe" — не режем || здесь
    m_launchArgs = argsStr.trimmed();
    m_gameUriDeferred = false;

    const QFileInfo fi(exe);
    process->setWorkingDirectory(fi.absolutePath());
    resetLogWatch();

    // Personal: только EADesktop.exe без args — пользователь логинится вручную.
    // Никакого soft-silent scout / origin2 / DIRECT.
    if (m_personalLaunch) {
        qWarning().noquote() << "[EA] personal launch: EADesktop empty args (no game URI/DIRECT):"
                             << exe;
        qWarning().noquote() << "[EA] personal: stored launch args ignored until manual play:"
                             << (m_launchArgs.isEmpty() ? QStringLiteral("(none)") : m_launchArgs);
        process->start(exe, QStringList{});
        return;
    }

    const QString uriPart = normalizeEaGameUri(m_launchArgs);
    const bool isUri = uriPart.startsWith(QStringLiteral("origin2://"), Qt::CaseInsensitive)
                       || uriPart.startsWith(QStringLiteral("origin://"), Qt::CaseInsensitive)
                       || uriPart.startsWith(QStringLiteral("eadm://"), Qt::CaseInsensitive)
                       || !resolveDirectGameExe(m_launchArgs).isEmpty();

    // Cache: тоже EADesktop без URI — проверяем FSM; если сессия мёртвая — scout залогинит.
    // LaunchHelper+URI сразу при битом cache открывает login и scout раньше был выключен.
    if (!m_expectInteractive && isUri) {
        m_gameUriDeferred = true;
        qWarning().noquote() << "[EA] cache launch: EADesktop, URI после FSM-проверки:" << exe;
        process->start(exe, QStringList{});
        return;
    }

    // Interactive: сначала только EADesktop, URI после логина командой.
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
    if (m_scoutTimer) {
        m_scoutTimer->stop();
        m_scoutTimer->deleteLater();
        m_scoutTimer = nullptr;
    }
    m_sendInputBusy = false;
    m_overlayHoldOffUntilMs = 0;
    // Конец сессии / accept game — гасим и ready-watch (finishScoutSuccess стартует его заново)
    stopLibraryReadyWatch();
}

void EaAuth::stopLibraryReadyWatch()
{
    if (!m_libraryReadyTimer)
        return;
    m_libraryReadyTimer->stop();
    m_libraryReadyTimer->deleteLater();
    m_libraryReadyTimer = nullptr;
}

bool EaAuth::isEaLibraryReadyUi() const
{
#ifdef Q_OS_WIN
    EaLoginEnumCtx ctx;
    ctx.eaPids = collectEaPids();
    EnumWindows(enumEaLoginProc, reinterpret_cast<LPARAM>(&ctx));
    // Библиотека готова: главное окно есть, формы логина нет
    return ctx.bestMain != nullptr && ctx.bestLogin == nullptr;
#else
    return false;
#endif
}

bool EaAuth::isClubGameLikelyRunning() const
{
#ifdef Q_OS_WIN
    // Полноэкранное окно с игровым классом — не EADesktop/Origin
    struct Ctx {
        bool found = false;
    } ctx;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (!IsWindowVisible(h))
            return TRUE;
        RECT rc{};
        GetWindowRect(h, &rc);
        const int w = rc.right - rc.left;
        const int hgt = rc.bottom - rc.top;
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        if (w < sw - 16 || hgt < sh - 16)
            return TRUE;
        char name[256];
        if (GetClassNameA(h, name, sizeof(name)) <= 0)
            return TRUE;
        const QString cls = QString::fromLatin1(name);
        const bool gameCls = cls == QLatin1String("UnrealWindow")
                             || cls == QLatin1String("UnityWndClass")
                             || cls == QLatin1String("Valve001")
                             || cls.startsWith(QLatin1String("CryENGINE"), Qt::CaseInsensitive);
        if (!gameCls)
            return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return TRUE;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        QString img;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == pid) {
                    img = QString::fromWCharArray(pe.szExeFile);
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (img.compare(QStringLiteral("EADesktop.exe"), Qt::CaseInsensitive) == 0
            || img.compare(QStringLiteral("Origin.exe"), Qt::CaseInsensitive) == 0
            || img.contains(QStringLiteral("EACef"), Qt::CaseInsensitive)
            || img.contains(QStringLiteral("EALauncher"), Qt::CaseInsensitive))
            return TRUE;
        c->found = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
#else
    return false;
#endif
}

void EaAuth::warnTitleOfferMismatch() const
{
    const QString title = m_gameTitle.toLower();
    const QString args = m_launchArgs.toLower();
    if (title.isEmpty() || args.isEmpty())
        return;

    const bool titleDiablo = title.contains(QStringLiteral("diablo"));
    const bool titleSwgoh = title.contains(QStringLiteral("galaxy of heroes"))
                            || title.contains(QStringLiteral("swgoh"))
                            || title.contains(QStringLiteral("star wars"));
    const bool argsSwgoh = args.contains(QStringLiteral("swgoh"))
                           || args.contains(QStringLiteral("galaxy-of-heroes"));
    const bool argsHasOffer = args.contains(QStringLiteral("offerids="));
    const bool pathSwgoh = args.contains(QStringLiteral("swgoh.exe"))
                           || args.contains(QStringLiteral("/swgoh/"));

    // Diablo IV title + SWGoH path/exe, или наоборот — битые данные в БД
    if (titleDiablo && (argsSwgoh || pathSwgoh)) {
        qWarning().noquote() << "[EA] WARN: title/offer inconsistent — title looks Diablo,"
                             << "args/path look SWGoH; launching origin2 as-is (fix DB if wrong game)."
                             << "title:" << m_gameTitle << "| args:" << m_launchArgs;
    } else if (titleSwgoh && argsHasOffer && !argsSwgoh && !pathSwgoh
               && args.contains(QStringLiteral("origin.ofr"))) {
        qWarning().noquote() << "[EA] WARN: title looks SWGoH / Star Wars, but offerId path"
                             << "has no SWGoH marker — verify DB offerIds."
                             << "title:" << m_gameTitle << "| args:" << m_launchArgs;
    } else if (!title.isEmpty() && argsHasOffer) {
        // Мягкий WARN: ни один значимый токен title не встречается в args
        static const QRegularExpression splitter(QStringLiteral("[^a-z0-9]+"));
        bool any = false;
        const QStringList parts = title.split(splitter, Qt::SkipEmptyParts);
        for (const QString &p : parts) {
            if (p.size() < 4)
                continue;
            if (args.contains(p)) {
                any = true;
                break;
            }
        }
        if (!any && (titleDiablo || titleSwgoh || title.contains(QStringLiteral("battlefield"))
                     || title.contains(QStringLiteral("apex")))) {
            qWarning().noquote() << "[EA] WARN: game_title tokens not found in launch args/offer —"
                                 << "possible DB mismatch. title:" << m_gameTitle
                                 << "| args:" << m_launchArgs;
        }
    }
}

void EaAuth::scheduleLibraryReadyLaunch(const QString &why)
{
#ifdef Q_OS_WIN
    stopLibraryReadyWatch();
    m_libraryReadyTicks = 0;
    m_libraryReadySinceTick = -1;
    m_origin2Fired = false;
    m_origin2Retried = false;
    m_origin2FiredAtMs = 0;

    warnTitleOfferMismatch();

    qWarning() << "[EA] library-ready wait before origin2:" << why
               << "| logFSM:" << logAuthStateName(m_logAuth)
               << "readyExt:" << m_logReadyForActions
               << "| same club account — no ownership wipe, no UI OK/Play";

    m_libraryReadyTimer = new QTimer(this);
    m_libraryReadyTimer->setInterval(400);
    connect(m_libraryReadyTimer, &QTimer::timeout, this, [this]() {
        if (!m_libraryReadyTimer || m_personalLaunch)
            return;

        ++m_libraryReadyTicks;
        pollEaLogs();

        const bool uiReady = isEaLibraryReadyUi();
        const bool logReady = m_logReadyForActions
                              || m_logAuth == LogAuthState::Authenticated;
        const bool readyNow = (logReady && uiReady)
                              || (m_logReadyForActions && m_libraryReadyTicks >= 8)
                              || (uiReady && m_libraryReadyTicks >= 20);

        if (readyNow && m_libraryReadySinceTick < 0) {
            m_libraryReadySinceTick = m_libraryReadyTicks;
            qWarning() << "[EA] library ready signal at tick" << m_libraryReadyTicks
                       << "| readyExt:" << m_logReadyForActions
                       << "| uiReady:" << uiReady
                       << "— settle ~5s then origin2";
        }

        // Settle 5s (~12 ticks) after ready; hard cap ~25s
        const bool settleDone = m_libraryReadySinceTick >= 0
                                && (m_libraryReadyTicks - m_libraryReadySinceTick) >= 12;
        const bool hardCap = m_libraryReadyTicks >= 62; // ~25s

        if (!m_origin2Fired && (settleDone || hardCap)) {
            if (hardCap && !settleDone)
                qWarning() << "[EA] library-ready hard cap — fire origin2 anyway";
            m_origin2Fired = true;
            m_allowsGameDetect = true;
            fireGameUri(false);
            m_origin2FiredAtMs = QDateTime::currentMSecsSinceEpoch();
            return;
        }

        // origin2 fired — if no game in ~12s, protocol retry once (not UI clicks)
        if (m_origin2Fired && !m_origin2Retried && m_origin2FiredAtMs > 0) {
            const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_origin2FiredAtMs;
            if (elapsed >= 12000) {
                if (isClubGameLikelyRunning()) {
                    qWarning() << "[EA] game window detected after origin2 — stop ready-watch";
                    stopLibraryReadyWatch();
                    return;
                }
                m_origin2Retried = true;
                qWarning() << "[EA] no game ~12s after origin2 — protocol retry once";
                fireGameUri(true);
                m_origin2FiredAtMs = QDateTime::currentMSecsSinceEpoch();
            }
        }

        if (m_origin2Retried && m_origin2FiredAtMs > 0) {
            const qint64 afterRetry = QDateTime::currentMSecsSinceEpoch() - m_origin2FiredAtMs;
            if (afterRetry >= 15000 || isClubGameLikelyRunning()) {
                if (isClubGameLikelyRunning())
                    qWarning() << "[EA] game detected after origin2 retry";
                else
                    qWarning() << "[EA] origin2 retry done — leave EA; user/Play or detect later";
                stopLibraryReadyWatch();
            }
        }

        // Absolute stop ~45s
        if (m_libraryReadyTicks >= 112)
            stopLibraryReadyWatch();
    });
    m_libraryReadyTimer->start();
#else
    Q_UNUSED(why);
    fireGameUri(false);
#endif
}

void EaAuth::finishScoutSuccess(const QString &why)
{
    qWarning() << "[EA] Scout OK:" << why;
    m_phase = Phase::Done;
    m_allowsGameDetect = false;
    m_needBackup = true;
    // Только scout-timer; library-ready стартует ниже
    if (m_scoutTimer) {
        m_scoutTimer->stop();
        m_scoutTimer->deleteLater();
        m_scoutTimer = nullptr;
    }

    QTimer::singleShot(120000, this, [this]() {
        if (m_phase == Phase::Done)
            m_allowsGameDetect = true;
    });

    if (m_personalLaunch) {
        qWarning() << "[EA] personal policy: scout success ignored — игру НЕ запускаем:" << why;
        m_gameUriDeferred = false;
        m_allowsGameDetect = true;
        return;
    }

    if (!m_gameUriDeferred)
        return;
    m_gameUriDeferred = false;

    // DIRECT exe (как раньше) — короткий delay; origin2-only — library-ready + retry.
    const QString directExe = resolveDirectGameExe(m_launchArgs);
    if (!directExe.isEmpty()) {
        const int uriDelayMs = m_passwordSent ? 6000 : 3000;
        qWarning() << "[EA] старт DIRECT через" << uriDelayMs << "мс:" << directExe
                   << "| title:" << (m_gameTitle.isEmpty() ? QStringLiteral("(none)") : m_gameTitle);
        warnTitleOfferMismatch();
        QTimer::singleShot(uriDelayMs, this, [this]() {
            m_allowsGameDetect = true;
            fireGameUri(false);
        });
        return;
    }

    // Нет локального exe — origin2 после library-ready (не UI OK/Play)
    scheduleLibraryReadyLaunch(why);
}

QString EaAuth::normalizeEaGameUri(const QString &args)
{
    // Поддержка: "origin2://...?offerIds=...||C:\\...\\game.exe"
    const int sep = args.indexOf(QStringLiteral("||"));
    if (sep >= 0)
        return args.left(sep).trimmed();
    return args.trimmed();
}

static QString resolveSwgohInstallExe()
{
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/EA Games/SWGoH/SWGoH.exe"),
        QStringLiteral("C:/Program Files (x86)/EA Games/SWGoH/SWGoH.exe"),
        QStringLiteral("D:/Program Files/EA Games/SWGoH/SWGoH.exe"),
        QStringLiteral("D:/EA Games/SWGoH/SWGoH.exe"),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return QDir::toNativeSeparators(c);
    }
    return {};
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

    const bool looksSwgoh = trimmed.contains(QStringLiteral("SWGoH"), Qt::CaseInsensitive)
                            || trimmed.contains(QStringLiteral("galaxy-of-heroes"), Qt::CaseInsensitive);

    // Club DB: offerIds=Origin.OFR.50.0005369 часто помечен как «Diablo IV», но на ПК
    // стоит SWGoH — раньше DIRECT по этому offer работал. Не блокируем: WARN + SWGoH.exe.
    if (trimmed.contains(QStringLiteral("Origin.OFR.50.0005369"), Qt::CaseInsensitive)
        || trimmed.contains(QStringLiteral("OFR.50.0005369"), Qt::CaseInsensitive)) {
        const QString swgoh = resolveSwgohInstallExe();
        if (!swgoh.isEmpty()) {
            qWarning().noquote() << "[EA] offer OFR.50.0005369 → DIRECT"
                                 << swgoh
                                 << "(DB title may say Diablo IV; install on disk is SWGoH)";
            return swgoh;
        }
    }

    if (looksSwgoh) {
        const QString swgoh = resolveSwgohInstallExe();
        if (!swgoh.isEmpty())
            return swgoh;
    }

    const bool hasOriginUri = trimmed.contains(QStringLiteral("origin2://"), Qt::CaseInsensitive)
                              || trimmed.contains(QStringLiteral("origin://"), Qt::CaseInsensitive)
                              || trimmed.contains(QStringLiteral("eadm://"), Qt::CaseInsensitive);
    // origin2 без известного offer/map — не угадываем первый EA Games exe
    if (hasOriginUri)
        return {};

    // Без URI: общий поиск C:\Program Files\EA Games\<Game>\<Game>.exe
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
            if (!QFileInfo::exists(guess))
                continue;
            if (looksSwgoh
                && !guess.contains(QStringLiteral("SWGoH"), Qt::CaseInsensitive))
                continue;
            return QDir::toNativeSeparators(guess);
        }
    }
    return {};
}

bool EaAuth::directExeMatchesGame(const QString &gameExe,
                                  const QString &launchArgs,
                                  const QString &gameTitle)
{
    // Совместимость: раньше title-mismatch блокировал DIRECT → origin2 modal.
    // Теперь только WARN, всегда разрешаем найденный exe.
    if (gameExe.isEmpty())
        return false;

    const QString path = QDir::fromNativeSeparators(gameExe).toLower();
    const QString base = QFileInfo(gameExe).completeBaseName().toLower();
    const QString title = gameTitle.toLower();

    if (title.isEmpty())
        return true;

    const bool exeIsSwgoh = base.contains(QStringLiteral("swgoh"))
                            || path.contains(QStringLiteral("/swgoh/"));
    const bool titleDiablo = title.contains(QStringLiteral("diablo"));
    if (exeIsSwgoh && titleDiablo) {
        qWarning().noquote() << "[EA] WARN: title/exe mismatch (DB?) — DIRECT всё равно:"
                             << QFileInfo(gameExe).fileName() << "vs title:" << gameTitle
                             << "| args:" << launchArgs;
        return true;
    }

    static const QRegularExpression splitter(QStringLiteral("[^a-z0-9]+"));
    bool any = false;
    const QStringList parts = title.split(splitter, Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (p.size() < 4)
            continue;
        if (path.contains(p) || base.contains(p)) {
            any = true;
            break;
        }
    }
    if (!any) {
        qWarning().noquote() << "[EA] WARN: DIRECT exe basename не совпадает с title —"
                             << QFileInfo(gameExe).fileName() << "vs" << gameTitle
                             << "(не блокируем DIRECT)";
    }
    return true;
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

void EaAuth::fireOrigin2Protocol(const QString &uri, bool isRetry)
{
#ifdef Q_OS_WIN
    const HINSTANCE r = ShellExecuteW(
        nullptr, L"open",
        reinterpret_cast<LPCWSTR>(uri.utf16()),
        nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = int(reinterpret_cast<quintptr>(r));
    qWarning() << "[EA] origin2 ShellExecute" << (isRetry ? "(retry)" : "(first)")
               << code << uri;
    if (code > 32)
        return;

    const QString launcher = eaLauncherBeside(m_launcherExe);
    if (!launcher.isEmpty()) {
        const bool ok = QProcess::startDetached(
            launcher, QStringList{uri}, QFileInfo(launcher).absolutePath());
        qWarning() << "[EA] origin2 via EALauncher.exe" << (isRetry ? "(retry)" : "(first)")
                   << ok << launcher << uri;
        return;
    }
    qWarning() << "[EA] fireOrigin2 FAIL: ShellExecute" << code << "EALauncher не найден";
#else
    Q_UNUSED(uri);
    Q_UNUSED(isRetry);
#endif
}

void EaAuth::fireGameUri(bool protocolRetry)
{
#ifdef Q_OS_WIN
    if (m_personalLaunch) {
        qWarning() << "[EA] personal policy: fireGameUri blocked (no auto origin2/DIRECT)";
        return;
    }
    if (m_launchArgs.trimmed().isEmpty()) {
        qWarning() << "[EA] fireGameUri: args пусты";
        return;
    }

    QString uri = normalizeEaGameUri(m_launchArgs);
    const bool hasUri = uri.startsWith(QStringLiteral("origin2://"), Qt::CaseInsensitive)
                        || uri.startsWith(QStringLiteral("origin://"), Qt::CaseInsensitive)
                        || uri.startsWith(QStringLiteral("eadm://"), Qt::CaseInsensitive);

    // Как до регрессии: DIRECT exe если резолвится; origin2 только без exe.
    // Title≠exe (Diablo vs SWGoH) — WARN, не блок.
    if (!protocolRetry) {
        warnTitleOfferMismatch();
        const QString gameExe = resolveDirectGameExe(m_launchArgs);
        if (!gameExe.isEmpty() && directExeMatchesGame(gameExe, m_launchArgs, m_gameTitle)) {
            const QString workDir = QFileInfo(gameExe).absolutePath();
            const QStringList gargs = directGameExtraArgs(gameExe);
            const bool ok = QProcess::startDetached(gameExe, gargs, workDir);
            qWarning().noquote() << "[EA] DIRECT game exe" << ok
                                 << gameExe << gargs.join(QLatin1Char(' '))
                                 << "| cwd:" << workDir
                                 << "| title:" << (m_gameTitle.isEmpty()
                                                       ? QStringLiteral("(none)") : m_gameTitle);
            return;
        }
    }

    if (hasUri) {
        fireOrigin2Protocol(uri, protocolRetry);
        return;
    }

    if (protocolRetry) {
        qWarning() << "[EA] origin2 retry skipped — no protocol URI / DIRECT already tried";
        return;
    }

    qWarning() << "[EA] нет подходящего DIRECT exe и нет origin2 URI" << m_launchArgs;
#else
    Q_UNUSED(protocolRetry);
    Q_UNUSED(m_launchArgs);
#endif
}

void EaAuth::relaunchGameArgs()
{
#ifdef Q_OS_WIN
    if (m_personalLaunch) {
        qWarning() << "[EA] personal policy: relaunchGameArgs blocked";
        return;
    }
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

    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->requestClearGameSearch();

    beginInjectBurst();
    AllowSetForegroundWindow(ASFW_ANY);
    // Club scout: foreground EA для SendInput; TOPMOST shell — только ~400ms после Enter на Далее.
    placeWindowForegroundOnly(hwnd, this);
    // Клик в шапку формы → фокус в CEF (без SetFocus на HWND, иначе Tab мимо поля)
    clickFieldPercent(hwnd, this, 0.50, 0.12, "activate CEF (не поле)");

    // Tab: 6 = email, 11 = Далее
    QTimer::singleShot(900, this, [this, hwndVal, email]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;
        AllowSetForegroundWindow(ASFW_ANY);
        placeWindowForegroundOnly(h, this);
        sendTabs(6, "→ поле email (Tab #6)");
        QTimer::singleShot(400, this, [this, hwndVal, email]() {
            HWND h2 = reinterpret_cast<HWND>(hwndVal);
            if (!h2 || !IsWindow(h2) || !m_scoutTimer)
                return;
            placeWindowForegroundOnly(h2, this);
            qWarning() << "[EA] type email unicode, len" << email.size();
            typeUnicode(email);
            QTimer::singleShot(600, this, [this, hwndVal]() {
                HWND h3 = reinterpret_cast<HWND>(hwndVal);
                if (!h3 || !IsWindow(h3) || !m_scoutTimer)
                    return;
                placeWindowForegroundOnly(h3, this);
                sendTabs(5, "→ Далее (Tab #11)");
                Sleep(200);
                sendReturnKey();
                qWarning() << "[EA] Enter на Далее — hold overlay ~400ms, затем TOPMOST; ждём пароль";
                m_phase = Phase::WaitPassword;
                m_phaseTick = m_ticks;
                endInjectBurstRestoreOverlay(400);
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

    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->requestClearGameSearch();

    beginInjectBurst();
    AllowSetForegroundWindow(ASFW_ANY);
    placeWindowForegroundOnly(hwnd, this);

    // Второе окно: фокус уже в пароле — не кликаем и не Tab до поля
    QTimer::singleShot(700, this, [this, hwndVal, password]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;
        if (auto *pm = qobject_cast<ProcessManager *>(parent()))
            pm->requestClearGameSearch();
        AllowSetForegroundWindow(ASFW_ANY);
        placeWindowForegroundOnly(h, this);
        qWarning() << "[EA] type password unicode, len" << password.size();
        typeUnicode(password);

        QTimer::singleShot(500, this, [this, hwndVal]() {
            HWND h2 = reinterpret_cast<HWND>(hwndVal);
            if (!h2 || !IsWindow(h2) || !m_scoutTimer)
                return;
            placeWindowForegroundOnly(h2, this);
            sendTabs(2, "→ Войти (Tab #2 с поля)");
            Sleep(200);
            sendReturnKey();
            m_needBackup = true;
            m_phase = Phase::PasswordSubmitted;
            m_phaseTick = m_ticks;
            m_errorBackClicked = false;
            qWarning() << "[EA] Enter на Войти — hold overlay ~450ms, затем TOPMOST";
            endInjectBurstRestoreOverlay(450);
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
    beginInjectBurst();
    placeWindowForInput(hwnd, this);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // Синяя кнопка «НАЗАД» внизу диалога ошибки
    const int x = (rc.right - rc.left) / 2;
    const int y = int((rc.bottom - rc.top) * 0.88);
    qWarning() << "[EA] click НАЗАД (ошибка IP/входа)" << x << y;
    clickClient(hwnd, x, y);
    endInjectBurstRestoreOverlay(350);
#else
    Q_UNUSED(hwndVal);
#endif
}

void EaAuth::startPersonalLoginWatch()
{
#ifdef Q_OS_WIN
    stopScout();
    m_ticks = 0;
    m_personalEarlyAuthWarned = false;
    m_phase = Phase::Done;
    m_allowsGameDetect = true;
    m_gameUriDeferred = false;
    if (m_logPath.isEmpty())
        resetLogWatch();

    qWarning() << "[EA] personal login-watch: FSM только WARN, без URI/DIRECT";

    m_scoutTimer = new QTimer(this);
    m_scoutTimer->setInterval(500);
    connect(m_scoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_scoutTimer || !m_personalLaunch)
            return;
        ++m_ticks;
        pollEaLogs();

        // После wipe authenticated за первые ~15с = wipe неполный; игру всё равно не трогаем
        if (m_logAuth == LogAuthState::Authenticated && m_ticks <= 30) {
            if (!m_personalEarlyAuthWarned) {
                m_personalEarlyAuthWarned = true;
                qWarning() << "[EA] WARN: personal — FSM authenticated слишком рано после wipe;"
                           << "auto-launch запрещён (ручной логин / Play в EA)";
            }
        }

        if (m_ticks > 80) { // ~40с достаточно для WARN
            stopScout();
        }
    });
    m_scoutTimer->start();
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
    m_sendInputBusy = false;
    m_overlayHoldOffUntilMs = 0;
    m_lastOverlayRaiseMs = 0;
    // offset уже выставлен в startLauncher; повторный seek EOF срежет ранний FSM
    if (m_logPath.isEmpty())
        resetLogWatch();

    // Personal: никогда soft-silent scout и никогда auto-URI по FSM authenticated
    if (m_personalLaunch) {
        qWarning() << "[EA] Scout пропущен (personal) — kill/wipe уже сделан,"
                   << "EADesktop empty args, ждём ручной логин; без auto-URI/DIRECT";
        startPersonalLoginWatch();
        return;
    }

    // Клубный cache без пароля — только ждать FSM/URI
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

        // Club interactive / soft-silent: idle — throttle TOPMOST; never during SendInput burst.
        if (hasLogin && !m_personalLaunch) {
            keepOverlayUp(false);
            if (m_ticks == 1 || m_ticks == 5 || m_ticks % 25 == 0)
                qWarning() << "[EA] Интерактивный логин (under overlay):" << ctx.loginTitle
                           << ctx.loginW << "x" << ctx.loginH;
        }
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

void EaAuth::backupCache(NetworkManager *net, int terminalId, const QString &login,
                         int accountId, int gameId)
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
    if (accountId > 0)
        rootPayload.insert(QStringLiteral("account_id"), accountId);
    if (gameId > 0)
        rootPayload.insert(QStringLiteral("game_id"), gameId);
    rootPayload.insert(QStringLiteral("platform"), QStringLiteral("ea"));
    rootPayload.insert(QStringLiteral("local_vdf"), blob);
    rootPayload.insert(QStringLiteral("config_vdf"), blob);

    QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qWarning() << "[EA] cache backup → server, bytes:" << jsonData.size()
               << "login:" << login << "account_id:" << accountId << "game_id:" << gameId
               << "terminal:" << terminalId;

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
