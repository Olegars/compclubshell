#include "processmanager.h"
#include "iplatformauth.h"
#include "steamauth.h"
#include "epicauth.h"
#include "directlaunchauth.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

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

static bool isEpicLauncherProcess(DWORD pid)
{
    const QString img = processImageForPid(pid);
    // Win64-Shipping / WebHelper / Portal — всё лаунчер, не игра
    return img.contains(QStringLiteral("EpicGames"), Qt::CaseInsensitive)
           || img.contains(QStringLiteral("EpicWebHelper"), Qt::CaseInsensitive);
}

static bool isShellProcess(DWORD pid);
static bool isSystemNoiseProcess(DWORD pid);
static bool isSystemNoiseClass(const QString &cls);
static bool isKnownGameClass(const QString &cls);
static bool isGameStubProcess(DWORD pid);

HWINEVENTHOOK g_pGameHook = nullptr;
ProcessManager *g_pProcessManagerInstance = nullptr;

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    Q_UNUSED(hWinEventHook);
    Q_UNUSED(idObject);
    Q_UNUSED(idChild);
    Q_UNUSED(dwEventThread);
    Q_UNUSED(dwmsEventTime);

    if (event != EVENT_SYSTEM_FOREGROUND || !hwnd || !g_pProcessManagerInstance)
        return;

    char className[256];
    GetClassNameA(hwnd, className, sizeof(className));
    QString clsStr(className);

    if (clsStr.startsWith("Qt", Qt::CaseInsensitive)
        || clsStr == "Progman"
        || clsStr == "WorkerW"
        || clsStr == "vguiPopupWindow"
        || clsStr == "SDL_app"
        || clsStr.contains("Chrome_WidgetWin", Qt::CaseInsensitive)
        || isSystemNoiseClass(clsStr)
        || !isKnownGameClass(clsStr)) {
        return;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (isEpicLauncherProcess(pid) || isShellProcess(pid) || isSystemNoiseProcess(pid)
        || isGameStubProcess(pid))
        return;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect))
        return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (width < screenWidth - 16 || height < screenHeight - 16)
        return;

    g_pProcessManagerInstance->onGameWindowFound(
        reinterpret_cast<quintptr>(hwnd), clsStr);
}

struct EnumGameClassCtx {
    QString needle;
    bool found = false;
};

static BOOL CALLBACK enumGameClassProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<EnumGameClassCtx *>(lp);
    char name[256];
    if (GetClassNameA(h, name, sizeof(name)) > 0
        && QString::fromLatin1(name) == c->needle
        && IsWindowVisible(h)) {
        c->found = true;
        return FALSE;
    }
    return TRUE;
}

static bool enumHasGameClass(const QString &className)
{
    if (className.isEmpty())
        return false;
    EnumGameClassCtx ctx;
    ctx.needle = className;
    EnumWindows(enumGameClassProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

static bool isShellProcess(DWORD pid)
{
    const QString img = processImageForPid(pid);
    return img.contains(QStringLiteral("appsector"), Qt::CaseInsensitive)
           || img.contains(QStringLiteral("sector0451"), Qt::CaseInsensitive);
}

// Windows UI / IME / Start — часто «на весь экран», но это не игра
static bool isSystemNoiseProcess(DWORD pid)
{
    const QString img = processImageForPid(pid).toLower();
    if (img.isEmpty())
        return true;
    static const char *noise[] = {
        "textinputhost.exe",
        "tabtip.exe",
        "ctfmon.exe",
        "shellexperiencehost.exe",
        "startmenuexperiencehost.exe",
        "searchhost.exe",
        "searchui.exe",
        "applicationframehost.exe",
        "systemsettings.exe",
        "lockapp.exe",
        "explorer.exe",
        "dwm.exe",
        "sihost.exe",
        "runtimebroker.exe",
        "widgetservice.exe",
        "widgets.exe",
        "phonexperiencehost.exe",
        "video.ui.exe",
        nullptr
    };
    for (int i = 0; noise[i]; ++i) {
        if (img == QLatin1String(noise[i]))
            return true;
    }
    return false;
}

static bool isSystemNoiseClass(const QString &cls)
{
    return cls == QLatin1String("Windows.UI.Core.CoreWindow")
           || cls == QLatin1String("ApplicationFrameWindow")
           || cls == QLatin1String("Windows.Internal.Shell.TabProxyWindow")
           || cls.startsWith(QLatin1String("Windows.UI."), Qt::CaseInsensitive);
}

static bool isNearFullscreenWindow(HWND h)
{
    if (!h || !IsWindowVisible(h))
        return false;
    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    // Допуск на рамки / exclusive fullscreen. Заставка ~1/6 экрана НЕ проходит.
    return w >= sw - 16 && hgt >= sh - 16;
}

// EAC / Epic Protected Game / мелкие splash — не считаем игрой
static bool isGameStubProcess(DWORD pid)
{
    const QString img = processImageForPid(pid).toLower();
    if (img.isEmpty())
        return true;
    return img.contains(QStringLiteral("start_protected_game"))
           || img.contains(QStringLiteral("easyanticheat"))
           || img.contains(QStringLiteral("easy anti-cheat"))
           || img.startsWith(QStringLiteral("eac_"))
           || img.contains(QStringLiteral("battleye"))
           || img.contains(QStringLiteral("bebroadway"))
           || img.contains(QStringLiteral("beoffline"))
           || img == QStringLiteral("splashscreen.exe")
           || img.contains(QStringLiteral("crashreporter"));
}

static bool isKnownGameClass(const QString &cls)
{
    // SDL_app специально НЕ включаем — у Epic/EAC заставка часто SDL и ~1/6 экрана
    return cls == QLatin1String("Valve001")
           || cls == QLatin1String("UnityWndClass")
           || cls == QLatin1String("UnrealWindow")
           || cls.startsWith(QLatin1String("CryENGINE"), Qt::CaseInsensitive);
}

struct FindGameWindowCtx {
    HWND hwnd = nullptr;
    QString className;
};

static BOOL CALLBACK enumFindGameWindowProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<FindGameWindowCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    char name[256];
    if (GetClassNameA(h, name, sizeof(name)) <= 0)
        return TRUE;
    const QString cls = QString::fromLatin1(name);

    if (cls.startsWith(QLatin1String("Qt"), Qt::CaseInsensitive)
        || cls == QLatin1String("Progman")
        || cls == QLatin1String("WorkerW")
        || cls == QLatin1String("Shell_TrayWnd")
        || cls.contains(QLatin1String("Chrome_WidgetWin"), Qt::CaseInsensitive)
        || isSystemNoiseClass(cls))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (isEpicLauncherProcess(pid) || isShellProcess(pid) || isSystemNoiseProcess(pid)
        || isGameStubProcess(pid))
        return TRUE;

    // Только почти fullscreen + игровой класс (заставка 1/6 экрана игнорируется)
    if (!isNearFullscreenWindow(h) || !isKnownGameClass(cls))
        return TRUE;

    c->hwnd = h;
    c->className = cls;
    return FALSE;
}
#endif

ProcessManager::ProcessManager(NetworkManager *netManager, QObject *parent)
    : QObject(parent)
    , m_process(nullptr)
    , m_mainWindow(nullptr)
    , m_netManager(netManager)
    , m_platformAuth(nullptr)
    , m_netWatchTimer(new QTimer(this))
    , m_gameExitTimer(new QTimer(this))
    , m_gameFindTimer(new QTimer(this))
    , m_alertActive(false)
    , m_offendingPid(0)
    , m_highActivityCounter(0)
    , m_currentTerminalId(1)
    , m_currentGameId(0)
    , m_gameSessionActive(false)
    , m_gameHwnd(0)
    , m_pendingGameHwnd(0)
    , m_gameGoneTicks(0)
{
#ifdef Q_OS_WIN
    g_pProcessManagerInstance = this;
#endif
    connect(m_netWatchTimer, &QTimer::timeout, this, &ProcessManager::monitorNetworkTraffic);
    connect(m_gameExitTimer, &QTimer::timeout, this, &ProcessManager::checkGameExit);
    connect(m_gameFindTimer, &QTimer::timeout, this, &ProcessManager::pollForGameWindow);
    m_gameFindTimer->setInterval(500);
    m_netWatchTimer->start(5000);
}

ProcessManager::~ProcessManager()
{
#ifdef Q_OS_WIN
    if (g_pGameHook) UnhookWinEvent(g_pGameHook);
#endif
    if (m_platformAuth) {
        m_platformAuth->stopScout();
        m_platformAuth->deleteLater();
        m_platformAuth = nullptr;
    }
}

void ProcessManager::setMainWindow(QWindow *window)
{
    m_mainWindow = window;
}

void ProcessManager::setShellTopmost(bool enabled)
{
#ifdef Q_OS_WIN
    if (!m_mainWindow)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(m_mainWindow->winId());
    if (!hwnd)
        return;
    if (enabled) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        m_mainWindow->show();
        m_mainWindow->raise();
        m_mainWindow->requestActivate();
    } else {
        // НЕ SWP_SHOWWINDOW — иначе после hide() шелл снова всплывает поверх игры
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#else
    Q_UNUSED(enabled);
#endif
}

void ProcessManager::hideShellForGame()
{
    if (!m_mainWindow)
        return;
    m_shellHiddenForGame = true;
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(m_mainWindow->winId());
    qWarning() << "[SESSION] hideShellForGame hwnd:"
               << QString::number(reinterpret_cast<quintptr>(hwnd), 16);
    setShellTopmost(false);
    m_mainWindow->setVisibility(QWindow::Hidden);
    m_mainWindow->hide();
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW | SWP_NOACTIVATE);
    }
#else
    m_mainWindow->hide();
#endif
}

void ProcessManager::showShellAfterGame()
{
    if (!m_mainWindow)
        return;
    ++m_hideShellGeneration; // отменить отложенные hideShellForGame
    m_shellHiddenForGame = false;
    qWarning() << "[SESSION] showShellAfterGame — fullscreen shell (frameless)";
    // Без рамки/меню; иначе после hide() Windows возвращает оконный режим
    m_mainWindow->setFlags(Qt::Window | Qt::FramelessWindowHint);
    m_mainWindow->setVisibility(QWindow::FullScreen);
    m_mainWindow->showFullScreen();
    setShellTopmost(true);
    m_mainWindow->raise();
    m_mainWindow->requestActivate();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(m_mainWindow->winId());
    if (hwnd) {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongW(hwnd, GWL_STYLE, style);
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, sw, sh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
    }
#endif
}

IPlatformAuth *ProcessManager::createPlatformAuth(const QString &platform)
{
    const QString p = platform.trimmed().toLower();
    if (p.isEmpty()
        || p == QLatin1String("steam")
        || p == QLatin1String("valve"))
        return new SteamAuth(this);

    if (p == QLatin1String("epic") || p == QLatin1String("epicgames"))
        return new EpicAuth(this);

    // Riot / EA / Battle.net / VK — пока direct exe
    return new DirectLaunchAuth(p, this);
}

void ProcessManager::launchGameWithSmartAuth(const QJsonObject &authData, const QString &steamAppId)
{
    launchPlatformSession(authData, steamAppId);
}

void ProcessManager::launchGameWithSmartAuthString(const QString &jsonString, const QString &steamAppId)
{
    launchPlatformSessionString(jsonString, steamAppId);
}

void ProcessManager::launchPlatformSessionString(const QString &jsonString, const QString &appIdHint)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCritical() << "[SESSION] JSON parse error:" << parseError.errorString();
        return;
    }
    launchPlatformSession(doc.object(), appIdHint);
}

void ProcessManager::launch(const QString &exePath, const QString &args,
                            const QString &, const QString &, const QString &)
{
    QJsonObject auth;
    auth.insert(QStringLiteral("platform"), QStringLiteral("direct"));
    auth.insert(QStringLiteral("exe_path"), exePath);
    auth.insert(QStringLiteral("args"), args);
    launchPlatformSession(auth);
}

void ProcessManager::launchPlatformSession(const QJsonObject &authData, const QString &appIdHint)
{
    const QString platformRaw = authData.value(QStringLiteral("platform")).toString().trimmed();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    const QString exePath = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    const QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();
    const QString gameTitle = authData.value(QStringLiteral("game_title")).toString();
    const QString authMode = authData.value(QStringLiteral("auth")).toObject()
                                 .value(QStringLiteral("mode")).toString();

    QString platform = platformRaw.toLower();
    QString resolveNote = QStringLiteral("from_payload");

    const bool looksEpic =
        argsStr.contains(QStringLiteral("com.epicgames.launcher"), Qt::CaseInsensitive)
        || exePath.contains(QStringLiteral("EpicGamesLauncher"), Qt::CaseInsensitive)
        || exePath.contains(QStringLiteral("Epic Games"), Qt::CaseInsensitive);

    const QFileInfo exeFi(exePath);
    const bool looksSteam =
        exeFi.fileName().compare(QStringLiteral("steam.exe"), Qt::CaseInsensitive) == 0
        || argsStr.contains(QStringLiteral("steam://"), Qt::CaseInsensitive)
        || argsStr.contains(QStringLiteral("-applaunch"), Qt::CaseInsensitive);

    if (looksEpic && (platform.isEmpty()
                      || platform == QLatin1String("steam")
                      || platform == QLatin1String("pc")
                      || platform == QLatin1String("direct")
                      || platform == QLatin1String("lesta"))) {
        resolveNote = QStringLiteral("override_to_epic_from_exe_args (was '%1')").arg(platformRaw);
        platform = QStringLiteral("epic");
    } else if (looksSteam && platform != QLatin1String("steam")
               && platform != QLatin1String("epic")) {
        // Lesta/Wargaming в клубе часто крутятся через Steam.exe — нужен SteamAuth + VDF
        resolveNote = QStringLiteral("override_to_steam_from_exe (was '%1')").arg(platformRaw);
        platform = QStringLiteral("steam");
    } else if (platform.isEmpty()) {
        resolveNote = QStringLiteral("default_steam_empty_platform");
        platform = QStringLiteral("steam");
    }

    if (authData.contains(QStringLiteral("terminal_id")))
        m_currentTerminalId = authData.value(QStringLiteral("terminal_id")).toInt();
    if (authData.contains(QStringLiteral("game_id")))
        m_currentGameId = authData.value(QStringLiteral("game_id")).toInt();
    m_currentLogin = authData.value(QStringLiteral("login")).toString();
    m_currentPlatform = platform;

    qWarning().noquote() << "[SESSION] ========== LAUNCH DEBUG ==========";
    qWarning().noquote() << "[SESSION] game_id:" << m_currentGameId
                         << "| title:" << (gameTitle.isEmpty() ? QStringLiteral("(n/a)") : gameTitle);
    qWarning().noquote() << "[SESSION] platform_raw:" << (platformRaw.isEmpty() ? QStringLiteral("(empty)") : platformRaw)
                         << "| api_source:" << (platformSource.isEmpty() ? QStringLiteral("(n/a)") : platformSource);
    qWarning().noquote() << "[SESSION] platform_final:" << platform
                         << "| resolve:" << resolveNote;
    qWarning().noquote() << "[SESSION] login:" << m_currentLogin
                         << "| pc:" << m_currentTerminalId
                         << "| account_id:" << authData.value(QStringLiteral("account_id")).toInt();
    qWarning().noquote() << "[SESSION] exe_path:" << (exePath.isEmpty() ? QStringLiteral("(empty)") : exePath);
    qWarning().noquote() << "[SESSION] args:" << (argsStr.isEmpty() ? QStringLiteral("(empty)") : argsStr);
    qWarning().noquote() << "[SESSION] appIdHint:" << (appIdHint.isEmpty() ? QStringLiteral("(empty)") : appIdHint);
    qWarning().noquote() << "[SESSION] auth.mode:" << (authMode.isEmpty() ? QStringLiteral("(n/a)") : authMode)
                         << "| looksEpic:" << looksEpic << "| looksSteam:" << looksSteam;
    qWarning().noquote() << "[SESSION] ==================================";

    if (m_platformAuth) {
        m_platformAuth->stopScout();
        m_platformAuth->deleteLater();
        m_platformAuth = nullptr;
    }
    m_platformAuth = createPlatformAuth(platform);
    qWarning() << "[SESSION] auth handler:" << m_platformAuth->platformId();

#ifdef Q_OS_WIN
    m_gameSessionActive = true;
    m_gameHwnd = 0;
    m_gameWindowClass.clear();
    m_gamePid = 0;
    m_gameProcessImage.clear();
    m_gameAcceptedAtMs = 0;
    m_gameGoneTicks = 0;
    m_pendingGameHwnd = 0;
    m_pendingGameClass.clear();
    m_gameExitTimer->stop();
    m_netWatchTimer->stop();

    m_platformAuth->killLauncher();

    const int killDelayMs = (platform == QLatin1String("steam")
                             || platform == QLatin1String("epic")) ? 700 : 100;

    QTimer::singleShot(killDelayMs, this, [this, authData, appIdHint, platform]() {
        if (!m_gameSessionActive || !m_platformAuth)
            return;

        const bool cacheOk = m_platformAuth->applyCache(authData);
        m_platformAuth->setNeedsCacheBackup(!cacheOk);
        qWarning() << "[SESSION]" << platform.toUpper()
                   << (cacheOk ? "тихий вход (cache)" : "без cache → scout + backup после входа");

        if (g_pGameHook) UnhookWinEvent(g_pGameHook);
        g_pGameHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
        );

        if (m_process) {
            m_process->disconnect();
            m_process->deleteLater();
            m_process = nullptr;
        }
        m_process = new QProcess(this);
        connect(m_process, &QProcess::errorOccurred, this, &ProcessManager::onProcessError);
        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &ProcessManager::onProcessFinished);

        const QString login = authData.value(QStringLiteral("login")).toString();
        const QString password = authData.value(QStringLiteral("password")).toString();
        connect(m_process, &QProcess::started, this, [this, login, password, platform]() {
            qWarning() << "[SESSION] process started:" << (m_process ? m_process->program() : QString())
                       << (m_process ? m_process->arguments() : QStringList());
            emit gameStarted();
            if (m_platformAuth)
                m_platformAuth->startScout(login, password);
            startGameFindPoll();
        });

        m_platformAuth->startLauncher(m_process, authData, appIdHint);
        qWarning() << "[SESSION] startLauncher queued:"
                   << (m_process ? m_process->program() : QString())
                   << (m_process ? m_process->arguments() : QStringList())
                   << "| state:" << (m_process ? int(m_process->state()) : -1);

        // Epic/Steam часто сразу завершают parent QProcess, лаунчер живёт отдельно.
        // Нельзя по NotRunning рвать scout через 8с.
        QTimer::singleShot(20000, this, [this, platform]() {
            if (!m_gameSessionActive || m_gameHwnd != 0)
                return;

            if (m_platformAuth && !m_platformAuth->allowsGameDetect()) {
                qWarning() << "[SESSION]" << platform.toUpper()
                           << "QProcess мог завершиться — scout/логин ещё идёт, не фейлим";
                return;
            }

            const QString image = m_platformAuth
                ? m_platformAuth->launcherProcessName()
                : QString();
            if (!image.isEmpty() && isProcessRunning(image)) {
                qWarning() << "[SESSION]" << platform.toUpper()
                           << "parent QProcess не активен, но" << image << "жив — OK";
                return;
            }

            if (m_process && m_process->state() != QProcess::NotRunning)
                return;

            qCritical() << "[SESSION]" << platform.toUpper()
                        << "лаунчер не поднялся:" << (m_process ? m_process->errorString() : QString());
            finishGameSession(QStringLiteral("launcher failed to start"));
        });
    });
#else
    Q_UNUSED(authData);
    Q_UNUSED(appIdHint);
#endif
}

void ProcessManager::startGameFindPoll()
{
    if (!m_gameFindTimer->isActive())
        m_gameFindTimer->start();
}

void ProcessManager::pollForGameWindow()
{
#ifdef Q_OS_WIN
    if (!m_gameSessionActive || m_gameHwnd != 0) {
        m_gameFindTimer->stop();
        return;
    }

    // Только почти fullscreen Unreal/Unity/Valve — не SDL-заставка и не start_protected_game
    FindGameWindowCtx fs;
    EnumWindows(enumFindGameWindowProc, reinterpret_cast<LPARAM>(&fs));
    if (!fs.hwnd)
        return;

    DWORD pid = 0;
    GetWindowThreadProcessId(fs.hwnd, &pid);
    qWarning() << "[SESSION] fullscreen game:" << fs.className
               << "| exe:" << processImageForPid(pid);
    m_pendingGameHwnd = 0;
    m_pendingGameClass.clear();
    acceptGameWindow(reinterpret_cast<quintptr>(fs.hwnd), fs.className);
#else
    m_gameFindTimer->stop();
#endif
}

void ProcessManager::acceptGameWindow(quintptr hwnd, const QString &className)
{
    if (m_gameHwnd != 0)
        return;

    qWarning() << "[SESSION] Игра запущена:" << className
               << "| platform:" << m_currentPlatform;
    m_pendingGameHwnd = 0;
    m_pendingGameClass.clear();
    m_gameFindTimer->stop();
    if (m_platformAuth) {
        m_platformAuth->stopScout();
    }
#ifdef Q_OS_WIN
    if (g_pGameHook) {
        UnhookWinEvent(g_pGameHook);
        g_pGameHook = nullptr;
    }

    // Игру сразу наверх под оверлеем; шелл прячем с задержкой — без мигания рабочего стола
    HWND gameHwnd = reinterpret_cast<HWND>(hwnd);
    if (gameHwnd && IsWindow(gameHwnd)) {
        ShowWindow(gameHwnd, SW_RESTORE);
        SetWindowPos(gameHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(gameHwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(gameHwnd);
        BringWindowToTop(gameHwnd);
    }
#endif

    emit gameStartedSuccessfully();
    startGameExitWatch(hwnd, className);

    const int hideGen = m_hideShellGeneration;
    QTimer::singleShot(2500, this, [this, hwnd, hideGen]() {
        if (hideGen != m_hideShellGeneration || !m_gameSessionActive)
            return;
        qWarning() << "[SESSION] delayed hide shell (игра уже на экране)";
#ifdef Q_OS_WIN
        HWND gameHwnd = reinterpret_cast<HWND>(hwnd);
        if (gameHwnd && IsWindow(gameHwnd)) {
            SetForegroundWindow(gameHwnd);
            BringWindowToTop(gameHwnd);
        }
#endif
        hideShellForGame();
    });

    // Epic: бэкап всегда после детекта игры (даже если scout не нашёл окно логина —
    // на ПК уже была сессия, ini всё равно нужно сохранить в БД).
    const bool epicAlwaysBackup = (m_currentPlatform == QLatin1String("epic"));
    if (m_platformAuth && (m_platformAuth->needsCacheBackup() || epicAlwaysBackup)) {
        const int delayMs = 3000;
        QTimer::singleShot(delayMs, this, [this, epicAlwaysBackup]() {
            if (!m_platformAuth)
                return;
            if (!m_gameSessionActive && !epicAlwaysBackup)
                return;
            qWarning() << "[" << m_currentPlatform.toUpper()
                       << "] cache backup → сервер";
            m_platformAuth->backupCache(m_netManager, m_currentTerminalId, m_currentLogin);
            m_platformAuth->setNeedsCacheBackup(false);
        });
    }
}

void ProcessManager::onGameWindowFound(quintptr hwnd, const QString &className)
{
    if (m_gameHwnd != 0)
        return;

#ifdef Q_OS_WIN
    HWND h = reinterpret_cast<HWND>(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    const QString img = processImageForPid(pid);
    if (isEpicLauncherProcess(pid) || isShellProcess(pid) || isGameStubProcess(pid)) {
        qWarning() << "[SESSION] skip stub/launcher:" << className << "| exe:" << img;
        return;
    }

    if (isSystemNoiseClass(className) || isSystemNoiseProcess(pid)) {
        qWarning() << "[SESSION] skip system UI:" << className << "| exe:" << img;
        return;
    }

    if (!isKnownGameClass(className) || !isNearFullscreenWindow(h)) {
        qWarning() << "[SESSION] ждём почти fullscreen игры, сейчас:" << className
                   << "| exe:" << img << "| (заставка/окно мало — игнор)";
        return;
    }

    qWarning() << "[SESSION] fullscreen game → accept:" << className << "| exe:" << img;
    acceptGameWindow(hwnd, className);
#else
    Q_UNUSED(hwnd);
    Q_UNUSED(className);
#endif
}

static bool isPidAlive(quint32 pid)
{
#ifdef Q_OS_WIN
    if (!pid)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return false;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

void ProcessManager::startGameExitWatch(quintptr hwnd, const QString &className)
{
    m_gameHwnd = hwnd;
    m_gameWindowClass = className;
    m_gameGoneTicks = 0;
    m_gameSessionActive = true;
    m_gameAcceptedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_gamePid = 0;
    m_gameProcessImage.clear();
#ifdef Q_OS_WIN
    DWORD pid = 0;
    GetWindowThreadProcessId(reinterpret_cast<HWND>(hwnd), &pid);
    m_gamePid = pid;
    m_gameProcessImage = processImageForPid(pid);
    qWarning() << "[SESSION] watch game pid:" << m_gamePid << m_gameProcessImage;
#endif
    if (!m_gameExitTimer->isActive())
        m_gameExitTimer->start(2000);
}

bool ProcessManager::findAliveGameWindow(quintptr *outHwnd) const
{
#ifdef Q_OS_WIN
    FindGameWindowCtx ctx;
    EnumWindows(enumFindGameWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.hwnd)
        return false;
    if (outHwnd)
        *outHwnd = reinterpret_cast<quintptr>(ctx.hwnd);
    return true;
#else
    Q_UNUSED(outHwnd);
    return false;
#endif
}

bool ProcessManager::isGameWindowAlive() const
{
#ifdef Q_OS_WIN
    if (m_gameHwnd && IsWindow(reinterpret_cast<HWND>(m_gameHwnd)))
        return true;
    if (isPidAlive(m_gamePid))
        return true;
    return findAliveGameWindow(nullptr);
#else
    return false;
#endif
}

void ProcessManager::checkGameExit()
{
    if (!m_gameSessionActive)
        return;

#ifdef Q_OS_WIN
    // Живое окно игры = сессия продолжается (PID при закрытии UE долго «висит» — не ждём его)
    if (m_gameHwnd && IsWindow(reinterpret_cast<HWND>(m_gameHwnd))) {
        m_gameGoneTicks = 0;
        return;
    }
    quintptr fresh = 0;
    if (findAliveGameWindow(&fresh)) {
        if (fresh != m_gameHwnd) {
            m_gameHwnd = fresh;
            DWORD pid = 0;
            GetWindowThreadProcessId(reinterpret_cast<HWND>(fresh), &pid);
            if (pid) {
                m_gamePid = pid;
                m_gameProcessImage = processImageForPid(pid);
            }
        }
        m_gameGoneTicks = 0;
        return;
    }

    // Grace только для splash→main (окно пропало, процесс ещё грузит).
    // Раньше 45с игнорировали любой выход — Steam/TF2 закрывались «долго», шелл ждал таймер.
    const qint64 aliveForMs = QDateTime::currentMSecsSinceEpoch() - m_gameAcceptedAtMs;
    if (aliveForMs < 45000 && isPidAlive(m_gamePid)) {
        m_gameGoneTicks = 0;
        return;
    }
#else
    if (isGameWindowAlive()) {
        m_gameGoneTicks = 0;
        return;
    }
#endif

    ++m_gameGoneTicks;
    qWarning() << "[SESSION] game window gone tick" << m_gameGoneTicks
               << "| pid:" << m_gamePid << m_gameProcessImage
               << "(шелл вернём по окну, не ждём смерть процесса)";
    // ~4с без окна — достаточно; killLauncher добьёт хвосты
    if (m_gameGoneTicks >= 2)
        finishGameSession(QStringLiteral("game window closed"));
}

void ProcessManager::finishGameSession(const QString &reason)
{
    if (!m_gameSessionActive)
        return;

    m_gameSessionActive = false;
    m_gameExitTimer->stop();
    m_gameFindTimer->stop();
    m_gameHwnd = 0;
    m_gamePid = 0;
    m_gameProcessImage.clear();
    m_gameAcceptedAtMs = 0;
    m_gameGoneTicks = 0;
    m_pendingGameHwnd = 0;
    m_pendingGameClass.clear();
    if (m_platformAuth)
        m_platformAuth->stopScout();

    qWarning() << "[SESSION] Сессия завершена:" << reason
               << "| platform:" << m_currentPlatform;

    // Сначала шелл на экран, потом taskkill; Epic ini часто дописывается при выходе лаунчера
    showShellAfterGame();
    emit gameFinished();

    if (m_platformAuth)
        m_platformAuth->killLauncher();

    if (m_platformAuth
        && (m_platformAuth->needsCacheBackup()
            || m_currentPlatform == QLatin1String("epic"))) {
        const QString login = m_currentLogin;
        const int termId = m_currentTerminalId;
        IPlatformAuth *auth = m_platformAuth;
        NetworkManager *net = m_netManager;
        qWarning() << "[SESSION] cache backup после killLauncher (отложенный)";
        QTimer::singleShot(2500, this, [auth, net, termId, login]() {
            if (!auth || !net)
                return;
            auth->backupCache(net, termId, login);
            auth->setNeedsCacheBackup(false);
        });
    }

    m_netWatchTimer->start(5000);
}

void ProcessManager::backupAndSendVdfPayload()
{
    if (m_platformAuth)
        m_platformAuth->backupCache(m_netManager, m_currentTerminalId, m_currentLogin);
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    Q_UNUSED(exitCode);
    if (m_gameSessionActive) {
        if (m_gameHwnd != 0)
            return;

        // Parent часто умирает сразу (Epic/Steam bootstrap) — не трогаем scout
        if (m_platformAuth && !m_platformAuth->allowsGameDetect()) {
            qWarning() << "[SESSION] parent QProcess finished во время логина — игнор"
                       << "| platform:" << m_currentPlatform;
            return;
        }

        QTimer::singleShot(5000, this, [this]() {
            if (!m_gameSessionActive || m_gameHwnd != 0)
                return;
            if (m_platformAuth && !m_platformAuth->allowsGameDetect())
                return;
            const QString image = m_platformAuth
                ? m_platformAuth->launcherProcessName()
                : QStringLiteral("steam.exe");
            if (!image.isEmpty() && isProcessRunning(image)) {
                qWarning() << "[SESSION] parent exited, but" << image << "still running";
                return;
            }
            finishGameSession(QStringLiteral("launcher exited before game window"));
        });
        return;
    }
    showShellAfterGame();
    emit gameFinished();
}

void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    qCritical() << "[SESSION] Process error:" << error;
    if (m_gameSessionActive)
        return;
    emit gameFinished();
}

bool ProcessManager::isProcessRunning(const QString &processName)
{
#ifdef Q_OS_WIN
    QProcess proc;
    proc.start(QStringLiteral("tasklist"),
               {QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq %1").arg(processName)});
    proc.waitForFinished(1000);
    return QString::fromUtf8(proc.readAllStandardOutput()).contains(processName, Qt::CaseInsensitive);
#else
    Q_UNUSED(processName);
    return false;
#endif
}

unsigned long ProcessManager::getProcessIdByName(const QString &processName)
{
#ifdef Q_OS_WIN
    QProcess proc;
    proc.start(QStringLiteral("powershell"), {
        QStringLiteral("-Command"),
        QStringLiteral("Get-Process -Name ") + processName.split(QLatin1Char('.')).first()
            + QStringLiteral(" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id")
    });
    proc.waitForFinished(1000);
    bool ok = false;
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toULong(&ok);
#else
    Q_UNUSED(processName);
    return 0;
#endif
}

void ProcessManager::monitorNetworkTraffic()
{
    if (m_alertActive || m_gameSessionActive)
        return;
#ifdef Q_OS_WIN
    auto *probe = new QProcess(this);
    connect(probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, probe](int, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(probe->readAllStandardOutput());
        probe->deleteLater();
        if (m_alertActive || m_gameSessionActive)
            return;
        if (!out.contains(QStringLiteral("steam.exe"), Qt::CaseInsensitive)) {
            m_highActivityCounter = 0;
            return;
        }

        auto *netCheck = new QProcess(this);
        connect(netCheck, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, netCheck](int, QProcess::ExitStatus) {
            const QByteArray raw = netCheck->readAllStandardOutput();
            netCheck->deleteLater();
            if (m_alertActive || m_gameSessionActive)
                return;
            bool ok = false;
            const double bytesPerSec = QString::fromUtf8(raw).trimmed().toDouble(&ok);
            if (ok && bytesPerSec > 40000000) {
                m_highActivityCounter++;
                if (m_highActivityCounter >= 3) {
                    m_alertActive = true;
                    m_offendingPid = getProcessIdByName(QStringLiteral("steam.exe"));
                    emit heavyDownloadDetected(QStringLiteral("Steam Update"));
                }
            } else {
                m_highActivityCounter = 0;
            }
        });
        netCheck->start(QStringLiteral("powershell"), {
            QStringLiteral("-Command"),
            QStringLiteral("Get-Counter '\\Network Interface(*)\\Bytes Received/sec' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty CounterSamples | Select-Object -ExpandProperty CookedValue")
        });
    });
    probe->start(QStringLiteral("tasklist"), {
        QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq steam.exe")
    });
#endif
}

void ProcessManager::applyQosPolicies(bool enable)
{
#ifdef Q_OS_WIN
    QProcess qosProc;
    if (enable) {
        qosProc.start(QStringLiteral("powershell"), {
            QStringLiteral("-Command"),
            QStringLiteral("New-NetQosPolicy -Name 'ReactorGameTraffic' -AppPathNameMatchCondition 'steam.exe' -DSCPAction 46 -Confirm:$false")
        });
    } else {
        qosProc.start(QStringLiteral("powershell"), {
            QStringLiteral("-Command"),
            QStringLiteral("Remove-NetQosPolicy -Name 'ReactorGameTraffic' -Confirm:$false")
        });
    }
    qosProc.waitForFinished(3000);
#else
    Q_UNUSED(enable);
#endif
}

void ProcessManager::setSystemVolume(int level)
{
#ifdef Q_OS_WIN
    HMODULE hWinmm = GetModuleHandleA("winmm.dll");
    if (!hWinmm) hWinmm = LoadLibraryA("winmm.dll");
    if (hWinmm) {
        typedef MMRESULT (WINAPI *WaveOutSetVolumeProto)(HWAVEOUT, DWORD);
        auto pWaveOutSetVolume = reinterpret_cast<WaveOutSetVolumeProto>(
            GetProcAddress(hWinmm, "waveOutSetVolume"));
        if (pWaveOutSetVolume) {
            DWORD winVol = (0xFFFF * level) / 100;
            pWaveOutSetVolume(nullptr, DWORD(winVol | (winVol << 16)));
        }
    }
#else
    Q_UNUSED(level);
#endif
}

void ProcessManager::toggleSystemLanguage()
{
#ifdef Q_OS_WIN
    HWND activeWnd = GetForegroundWindow();
    if (activeWnd)
        PostMessageA(activeWnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)HKL_NEXT);
#endif
}

void ProcessManager::handleDownloadDecision(bool continueDownload)
{
    if (!continueDownload && m_offendingPid != 0) {
#ifdef Q_OS_WIN
        QProcess killProc;
        killProc.start(QStringLiteral("taskkill"), {
            QStringLiteral("/F"), QStringLiteral("/PID"), QString::number(m_offendingPid)
        });
        killProc.waitForFinished(2000);
#endif
    }
    m_alertActive = false;
    m_offendingPid = 0;
    m_highActivityCounter = 0;
}

void ProcessManager::applyEnterprisePolicies(bool enable)
{
#ifdef Q_OS_WIN
    QSettings systemReg(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"),
        QSettings::NativeFormat);
    if (enable) {
        systemReg.setValue(QStringLiteral("DisableTaskMgr"), 1);
        systemReg.setValue(QStringLiteral("DisableRegistryTools"), 1);
    } else {
        systemReg.remove(QStringLiteral("DisableTaskMgr"));
        systemReg.remove(QStringLiteral("DisableRegistryTools"));
    }
#else
    Q_UNUSED(enable);
#endif
}
