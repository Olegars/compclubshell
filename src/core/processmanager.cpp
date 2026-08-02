#include "processmanager.h"
#include "audiomanager_win.h"
#include "iplatformauth.h"
#include "steamauth.h"
#include "epicauth.h"
#include "eaauth.h"
#include "riotauth.h"
#include "directlaunchauth.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRasterWindow>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QWindow>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

/*
 * Windowed / fullscreen note (club shell ↔ game toggle):
 * Games often run exclusive or borderless fullscreen — we do NOT force windowed mode.
 * QuickMenu (left-edge strip) is a separate Qt::Tool + WindowStaysOnTopHint HWND
 * with WS_EX_NOACTIVATE so clicks do not steal focus / minimize the game.
 * Exclusive fullscreen can still cover TOPMOST overlays on some GPUs; borderless
 * usually leaves the strip visible.
 */

// Legacy right-edge tab (unused: SHELL lives in QuickMenu). Kept so rebuilds stay simple.
class ShellToggleWindow : public QRasterWindow
{
public:
    std::function<void()> onClicked;

    ShellToggleWindow()
        : QRasterWindow()
    {
        // Independent tool HWND (not a child of the shell) so it stays visible while shell is Hidden.
        setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        resize(kTabW, kTabH);
        setTitle(QStringLiteral("REACTOR Shell Toggle"));
    }

    void repositionSideEdge()
    {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen)
            return;
        const QRect geo = screen->availableGeometry();
        // Flush to right edge; slightly above vertical center (game HUDs rarely place controls here).
        const int x = geo.x() + geo.width() - width();
        const int y = geo.y() + geo.height() / 2 - height() / 2 - geo.height() / 12;
        setPosition(x, y);
    }

    void raiseTopmostNative()
    {
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd)
            return;
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
#endif
    }

protected:
    bool event(QEvent *ev) override
    {
        if (ev->type() == QEvent::Leave && m_hover) {
            m_hover = false;
            update();
        }
        return QRasterWindow::event(ev);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect area(0, 0, width(), height());
        p.fillRect(area, Qt::transparent);

        // Inset left/top/bottom for AA stroke; right edge flush to screen (no outer radius).
        const QRectF r = QRectF(area).adjusted(1.0, 1.0, 0.0, -1.0);
        const qreal radius = 10.0;

        QPainterPath path;
        path.moveTo(r.right(), r.top());
        path.lineTo(r.left() + radius, r.top());
        path.quadTo(r.left(), r.top(), r.left(), r.top() + radius);
        path.lineTo(r.left(), r.bottom() - radius);
        path.quadTo(r.left(), r.bottom(), r.left() + radius, r.bottom());
        path.lineTo(r.right(), r.bottom());
        path.closeSubpath();

        p.setBrush(QColor(3, 7, 4, 235));
        p.setPen(QPen(QColor(0x22, 0xc5, 0x5e, m_hover ? 255 : 190), 1.5));
        p.drawPath(path);

        p.setPen(QColor(0x22, 0xc5, 0x5e));
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(11);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
        p.setFont(f);

        // Vertical label: rotate around tab center.
        p.save();
        p.translate(area.center());
        p.rotate(-90);
        p.drawText(QRectF(-area.height() / 2.0, -area.width() / 2.0,
                          area.height(), area.width()),
                   Qt::AlignCenter, QStringLiteral("☰ SHELL"));
        p.restore();
    }

    void mousePressEvent(QMouseEvent *ev) override
    {
        if (ev->button() == Qt::LeftButton && onClicked)
            onClicked();
    }

    void mouseMoveEvent(QMouseEvent *) override
    {
        if (!m_hover) {
            m_hover = true;
            update();
        }
    }

private:
    static constexpr int kTabW = 28;
    static constexpr int kTabH = 96;
    bool m_hover = false;
};

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

static bool isLeagueClientSized(HWND h)
{
    if (!h)
        return false;
    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    // Splash ~512×216; полноценный клиент обычно ≥800×450 (часто 1280×720)
    // Visible не требуем: окно может быть ещё SW_HIDE в момент первого детекта
    return w >= 800 && hgt >= 450;
}

static bool isRiotLeagueProcess(DWORD pid)
{
    const QString img = processImageForPid(pid);
    return img.contains(QStringLiteral("LeagueClient"), Qt::CaseInsensitive);
}

static bool isRiotValorantProcess(DWORD pid)
{
    const QString img = processImageForPid(pid).toLower();
    return img.contains(QStringLiteral("valorant"))
           || img.contains(QStringLiteral("valorant-win64-shipping"));
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

// League Client = CEF (Chrome_WidgetWin / RCLIENT), не fullscreen — отдельный enum для Riot
static BOOL CALLBACK enumFindLeagueClientProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<FindGameWindowCtx *>(lp);
    if (!IsWindowVisible(h) || !isLeagueClientSized(h))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!isRiotLeagueProcess(pid) || isShellProcess(pid))
        return TRUE;

    char name[256];
    if (GetClassNameA(h, name, sizeof(name)) <= 0)
        return TRUE;

    wchar_t title[256] = {};
    GetWindowTextW(h, title, 255);
    const QString t = QString::fromWCharArray(title);
    const QString img = processImageForPid(pid);

    // Предпочитаем окно с заголовком League / LeagueClientUx
    const bool titled = t.contains(QStringLiteral("League"), Qt::CaseInsensitive);
    const bool ux = img.contains(QStringLiteral("LeagueClientUx"), Qt::CaseInsensitive);
    if (!titled && !ux)
        return TRUE;

    c->hwnd = h;
    c->className = QString::fromLatin1(name);
    // Если уже нашли titled Ux — можно остановиться; иначе берём первый подходящий
    if (titled && ux)
        return FALSE;
    return titled ? FALSE : TRUE;
}

static bool findRiotProductWindow(HWND *outHwnd, QString *outClass)
{
    if (!outHwnd || !outClass)
        return false;
    *outHwnd = nullptr;
    outClass->clear();

    FindGameWindowCtx league;
    EnumWindows(enumFindLeagueClientProc, reinterpret_cast<LPARAM>(&league));
    if (league.hwnd) {
        *outHwnd = league.hwnd;
        *outClass = league.className;
        return true;
    }

    // Valorant: почти fullscreen Unreal
    FindGameWindowCtx fs;
    EnumWindows(enumFindGameWindowProc, reinterpret_cast<LPARAM>(&fs));
    if (fs.hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fs.hwnd, &pid);
        if (isRiotValorantProcess(pid)) {
            *outHwnd = fs.hwnd;
            *outClass = fs.className;
            return true;
        }
    }
    return false;
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
    , m_shellToggleTopmostTimer(new QTimer(this))
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
    connect(m_shellToggleTopmostTimer, &QTimer::timeout, this, &ProcessManager::reassertShellToggleTopmost);
    if (m_netManager) {
        connect(m_netManager, &NetworkManager::powerActionRequested,
                this, &ProcessManager::applyPowerAction);
    }
    m_gameFindTimer->setInterval(500);
    m_shellToggleTopmostTimer->setInterval(1500);
    m_netWatchTimer->start(5000);

    m_shellToggle = new ShellToggleWindow();
    m_shellToggle->onClicked = [this]() { switchToShell(); };
    m_shellToggle->repositionSideEdge();

#ifdef Q_OS_WIN
    win32_start_headphones_guard();
#endif
}

ProcessManager::~ProcessManager()
{
#ifdef Q_OS_WIN
    win32_stop_headphones_guard();
    if (g_pGameHook) UnhookWinEvent(g_pGameHook);
#endif
    showShellToggle(false);
    delete m_shellToggle;
    m_shellToggle = nullptr;
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

bool ProcessManager::hasActiveGame() const
{
    return m_hasActiveGame;
}

QString ProcessManager::gameTitle() const
{
    return m_gameTitle;
}

bool ProcessManager::shellHiddenForGame() const
{
    return m_shellHiddenForGame;
}

void ProcessManager::setHasActiveGame(bool active)
{
    if (m_hasActiveGame == active)
        return;
    m_hasActiveGame = active;
    emit hasActiveGameChanged();
}

void ProcessManager::setGameTitle(const QString &title)
{
    if (m_gameTitle == title)
        return;
    m_gameTitle = title;
    emit gameTitleChanged();
}

void ProcessManager::setShellHiddenForGame(bool hidden)
{
    if (m_shellHiddenForGame == hidden)
        return;
    m_shellHiddenForGame = hidden;
    emit shellHiddenForGameChanged();
}

void ProcessManager::showShellToggle(bool show)
{
    if (!m_shellToggle)
        return;
    if (show) {
        m_shellToggle->repositionSideEdge();
        m_shellToggle->setVisible(true);
        m_shellToggle->show();
        m_shellToggle->raise();
        m_shellToggle->raiseTopmostNative();
        if (!m_shellToggleTopmostTimer->isActive())
            m_shellToggleTopmostTimer->start();
    } else {
        m_shellToggleTopmostTimer->stop();
        m_shellToggle->hide();
    }
}

void ProcessManager::reassertShellToggleTopmost()
{
    if (!m_shellToggle || !m_shellToggle->isVisible())
        return;
    m_shellToggle->raiseTopmostNative();
}

void ProcessManager::focusGameWindow()
{
#ifdef Q_OS_WIN
    HWND gameHwnd = reinterpret_cast<HWND>(m_gameHwnd);
    if ((!gameHwnd || !IsWindow(gameHwnd)) && findAliveGameWindow(&m_gameHwnd))
        gameHwnd = reinterpret_cast<HWND>(m_gameHwnd);
    if (!gameHwnd || !IsWindow(gameHwnd))
        return;
    ShowWindow(gameHwnd, SW_RESTORE);
    AllowSetForegroundWindow(ASFW_ANY);
    SetWindowPos(gameHwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(gameHwnd);
    BringWindowToTop(gameHwnd);
#else
    Q_UNUSED(this);
#endif
}

void ProcessManager::requestClearGameSearch()
{
    // Model filter first; QML TextField clears on clearGameSearchRequested (no password flash).
    if (m_netManager)
        m_netManager->clearGamesSearch();
    emit clearGameSearchRequested();
}

void ProcessManager::restoreShellUi(bool /*endSessionPath*/)
{
    if (!m_mainWindow)
        return;
    // Belt-and-suspenders: drop any leaked credential text before shell is visible again.
    requestClearGameSearch();
    qWarning() << "[SESSION] restoreShellUi — fullscreen shell (frameless)";
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

bool ProcessManager::isSessionBusy() const
{
    // Busy while session is active and shell is on screen (launching OR paused-over-game).
    return m_gameSessionActive && !m_shellHiddenForGame;
}

void ProcessManager::hideShellForGame()
{
    if (!m_mainWindow)
        return;
    setShellHiddenForGame(true);
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
    // SHELL перенесён в QuickMenu (левая полоска) — отдельную вкладку справа не показываем.
}

void ProcessManager::showShellAfterGame()
{
    if (!m_mainWindow)
        return;
    ++m_hideShellGeneration; // отменить отложенные hideShellForGame
    showShellToggle(false);
    setShellHiddenForGame(false);
    qWarning() << "[SESSION] showShellAfterGame — fullscreen shell (frameless)";
    restoreShellUi(true);
}

void ProcessManager::showShellKeepGame()
{
    // Mid-session overlay: restore shell, keep watching game. Do NOT finishGameSession /
    // backup / forceKill. Game may stay running under the shell (not minimized).
    if (!m_mainWindow)
        return;
    if (!m_gameSessionActive && !m_hasActiveGame) {
        qWarning() << "[SESSION] showShellKeepGame ignored — no active game session";
        return;
    }
    ++m_hideShellGeneration; // cancel delayed hideShellForGame if still pending
    showShellToggle(false);
    setShellHiddenForGame(false);
    qWarning() << "[SESSION] showShellKeepGame — shell overlay, game session kept alive"
               << "| title:" << (m_gameTitle.isEmpty() ? QStringLiteral("(n/a)") : m_gameTitle);
    restoreShellUi(false);
}

void ProcessManager::switchToShell()
{
    showShellKeepGame();
}

void ProcessManager::switchToGame()
{
    if (!m_hasActiveGame && !m_gameSessionActive) {
        qWarning() << "[SESSION] switchToGame ignored — no active game";
        return;
    }
    if (!isGameWindowAlive()) {
        qWarning() << "[SESSION] switchToGame — game gone, ending session";
        finishGameSession(QStringLiteral("game gone on switchToGame"));
        return;
    }
    qWarning() << "[SESSION] switchToGame — hide shell, focus game"
               << "| title:" << (m_gameTitle.isEmpty() ? QStringLiteral("(n/a)") : m_gameTitle);
    focusGameWindow();
    hideShellForGame();
    focusGameWindow();
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

    if (p == QLatin1String("ea")
        || p == QLatin1String("origin")
        || p == QLatin1String("eadesktop")
        || p == QLatin1String("ea app")
        || p == QLatin1String("eaapp")
        || p == QLatin1String("electronic arts"))
        return new EaAuth(this);

    if (p == QLatin1String("riot")
        || p == QLatin1String("riot games")
        || p == QLatin1String("riotgames")
        || p == QLatin1String("valorant")
        || p == QLatin1String("league of legends"))
        return new RiotAuth(this);

    // Battle.net / VK — пока direct exe
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

void ProcessManager::launchFirstExisting(const QStringList &candidatePaths, const QString &args)
{
    QStringList tried;
    for (const QString &raw : candidatePaths) {
        const QString path = raw.trimmed();
        if (path.isEmpty())
            continue;
        tried << path;
        if (QFileInfo::exists(path)) {
            qWarning().noquote() << "[LAUNCH] лаунчер найден:" << path;
            launch(path, args);
            return;
        }
        qWarning().noquote() << "[LAUNCH] нет файла:" << path;
    }
    qCritical().noquote() << "[LAUNCH] лаунчер не установлен / путь не найден. Проверено:"
                          << tried.join(QStringLiteral(" | "));
}

void ProcessManager::launchDetached(const QString &exePath, const QString &args)
{
    const QString path = QDir::fromNativeSeparators(exePath.trimmed());
    if (path.isEmpty()) {
        qWarning() << "[QUICK] пустой путь";
        return;
    }
    if (!QFileInfo::exists(path)) {
        qWarning() << "[QUICK] файл не найден:" << path;
        return;
    }

#ifdef Q_OS_WIN
    // ShellExecute умеет .lnk / .exe и поднимает уже запущенный Discord/Telegram.
    const QString native = QDir::toNativeSeparators(path);
    const QString argsTrim = args.trimmed();
    const HINSTANCE rc = ShellExecuteW(
                nullptr,
                L"open",
                reinterpret_cast<LPCWSTR>(native.utf16()),
                argsTrim.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(argsTrim.utf16()),
                nullptr,
                SW_SHOWNORMAL);
    if (reinterpret_cast<quintptr>(rc) <= 32) {
        qWarning() << "[QUICK] ShellExecute failed:" << path << "code" << reinterpret_cast<quintptr>(rc);
    } else {
        qWarning() << "[QUICK] launched:" << path;
    }
#else
    Q_UNUSED(args);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        qWarning() << "[QUICK] openUrl failed:" << path;
#endif
}

void ProcessManager::raiseTopmostToolWindow(QObject *windowObject)
{
    auto *window = qobject_cast<QWindow *>(windowObject);
    if (!window)
        return;

    // Не активируем: иначе перехватываем фокус у игры / автологина.
    if (!window->isVisible())
        window->setVisible(true);
    window->raise();

#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;
    // WS_EX_NOACTIVATE: клик по tool-окну не уводит фокус и не сворачивает fullscreen-игру.
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_NOACTIVATE) == 0)
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
#endif
}

void ProcessManager::launchPlatformSession(const QJsonObject &authData, const QString &appIdHint)
{
    requestClearGameSearch();

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

    const bool looksEa =
        exeFi.fileName().compare(QStringLiteral("EADesktop.exe"), Qt::CaseInsensitive) == 0
        || exePath.contains(QStringLiteral("EA Desktop"), Qt::CaseInsensitive)
        || argsStr.contains(QStringLiteral("origin2://"), Qt::CaseInsensitive)
        || argsStr.contains(QStringLiteral("origin://"), Qt::CaseInsensitive)
        || argsStr.contains(QStringLiteral("eadm://"), Qt::CaseInsensitive);

    const bool looksRiot =
        exeFi.fileName().compare(QStringLiteral("RiotClientServices.exe"), Qt::CaseInsensitive) == 0
        || exePath.contains(QStringLiteral("Riot Client"), Qt::CaseInsensitive)
        || exePath.contains(QStringLiteral("Riot Games"), Qt::CaseInsensitive)
        || argsStr.contains(QStringLiteral("valorant"), Qt::CaseInsensitive)
        || argsStr.contains(QStringLiteral("league_of_legends"), Qt::CaseInsensitive)
        || gameTitle.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive)
        || gameTitle.contains(QStringLiteral("League of Legends"), Qt::CaseInsensitive)
        || platform.contains(QStringLiteral("riot"));

    if (looksEpic && (platform.isEmpty()
                      || platform == QLatin1String("steam")
                      || platform == QLatin1String("pc")
                      || platform == QLatin1String("direct")
                      || platform == QLatin1String("lesta")
                      || platform == QLatin1String("ea")
                      || platform == QLatin1String("riot"))) {
        resolveNote = QStringLiteral("override_to_epic_from_exe_args (was '%1')").arg(platformRaw);
        platform = QStringLiteral("epic");
    } else if (looksEa && platform != QLatin1String("ea")
               && platform != QLatin1String("epic")
               && platform != QLatin1String("steam")
               && platform != QLatin1String("riot")) {
        resolveNote = QStringLiteral("override_to_ea_from_exe (was '%1')").arg(platformRaw);
        platform = QStringLiteral("ea");
    } else if (platform == QLatin1String("origin")
               || platform == QLatin1String("eadesktop")
               || platform == QLatin1String("eaapp")
               || platform == QLatin1String("ea app")
               || platform == QLatin1String("electronic arts")) {
        resolveNote = QStringLiteral("normalized_ea (was '%1')").arg(platformRaw);
        platform = QStringLiteral("ea");
    } else if (looksRiot && platform != QLatin1String("riot")
               && platform != QLatin1String("epic")
               && platform != QLatin1String("ea")
               && platform != QLatin1String("steam")) {
        resolveNote = QStringLiteral("override_to_riot_from_exe (was '%1')").arg(platformRaw);
        platform = QStringLiteral("riot");
    } else if (platform == QLatin1String("riot games")
               || platform == QLatin1String("riotgames")
               || platform == QLatin1String("valorant")
               || platform == QLatin1String("league of legends")) {
        resolveNote = QStringLiteral("normalized_riot (was '%1')").arg(platformRaw);
        platform = QStringLiteral("riot");
    } else if (looksSteam && platform != QLatin1String("steam")
               && platform != QLatin1String("epic")
               && platform != QLatin1String("ea")
               && platform != QLatin1String("riot")) {
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
    m_currentAccountId = authData.value(QStringLiteral("account_id")).toInt();
    m_currentLogin = authData.value(QStringLiteral("login")).toString();
    m_currentPlatform = platform;
    m_personalAccount = (authMode == QLatin1String("personal"))
        || m_currentLogin.trimmed().isEmpty()
        || platformSource == QLatin1String("personal_account");

    setGameTitle(gameTitle);

    qWarning().noquote() << "[SESSION] launch:"
                         << (gameTitle.isEmpty() ? QStringLiteral("(n/a)") : gameTitle)
                         << "| id:" << m_currentGameId
                         << "| platform:" << platform
                         << "| login:" << (m_currentLogin.isEmpty() ? QStringLiteral("(personal)") : m_currentLogin)
                         << "| resolve:" << resolveNote;
    {
        const QString resolvedApp = SteamAuth::resolveAppId(authData, appIdHint);
        const QString titleLower = gameTitle.toLower();
        if (!resolvedApp.isEmpty()
            && (titleLower.contains(QStringLiteral("world of tanks"))
                || titleLower.contains(QStringLiteral("wot")))
            && resolvedApp == QLatin1String("440")) {
            qWarning().noquote() << "[SESSION] WARN: title looks like World of Tanks but Steam app_id=440 (TF2)"
                                 << "— возможные битые данные игры в БД";
        }
    }

    if (m_platformAuth) {
        m_platformAuth->stopScout();
        m_platformAuth->deleteLater();
        m_platformAuth = nullptr;
    }
    m_platformAuth = createPlatformAuth(platform);
    qDebug() << "[SESSION] auth handler:" << m_platformAuth->platformId();
#ifdef Q_OS_WIN
    m_gameSessionActive = true;
    setHasActiveGame(false); // becomes true only after acceptGameWindow
    showShellToggle(false);
    setShellHiddenForGame(false);
    m_gameHwnd = 0;
    m_gameWindowClass.clear();
    m_gamePid = 0;
    m_gameProcessImage.clear();
    m_gameAcceptedAtMs = 0;
    m_gameGoneTicks = 0;
    m_pendingGameHwnd = 0;
    m_pendingGameClass.clear();
    m_personalLoginWait = false;
    m_gameExitTimer->stop();
    m_netWatchTimer->stop();

    m_platformAuth->killLauncher();

    const int killDelayMs = (platform == QLatin1String("steam")
                             || platform == QLatin1String("epic")) ? 700
                          : (platform == QLatin1String("ea") ? 1500
                          : (platform == QLatin1String("riot") ? 5000 : 100));
    if (platform == QLatin1String("riot"))
        qWarning() << "[SESSION] Riot: ждём" << killDelayMs
                   << "ms после kill + рестарт vgc перед стартом";

    QTimer::singleShot(killDelayMs, this, [this, authData, appIdHint, platform]() {
        if (!m_gameSessionActive || !m_platformAuth)
            return;

        // Sanitize personal payload BEFORE applyCache: never pass DB machine-cache.
        QJsonObject data = authData;
        const QString authMode = data.value(QStringLiteral("auth")).toObject()
                                     .value(QStringLiteral("mode")).toString();
        const QString platformSource = data.value(QStringLiteral("platform_source")).toString();
        const bool personal = m_personalAccount
            || (authMode.compare(QStringLiteral("personal"), Qt::CaseInsensitive) == 0)
            || (platformSource.compare(QStringLiteral("personal_account"),
                                       Qt::CaseInsensitive) == 0)
            || data.value(QStringLiteral("login")).toString().trimmed().isEmpty();
        if (personal) {
            const bool hadVdf = data.contains(QStringLiteral("vdf_files"));
            const bool hadAuthCache = data.value(QStringLiteral("auth")).toObject()
                                          .contains(QStringLiteral("cache"));
            data.remove(QStringLiteral("vdf_files"));
            data.remove(QStringLiteral("local_vdf"));
            data.remove(QStringLiteral("config_vdf"));
            QJsonObject authObj = data.value(QStringLiteral("auth")).toObject();
            authObj.remove(QStringLiteral("cache"));
            authObj.insert(QStringLiteral("mode"), QStringLiteral("personal"));
            data.insert(QStringLiteral("auth"), authObj);
            data.insert(QStringLiteral("login"), QString());
            data.insert(QStringLiteral("password"), QString());
            qWarning() << "[SESSION]" << platform.toUpper()
                       << "personal: stripped DB cache from payload"
                       << "| had vdf_files:" << hadVdf
                       << "| had auth.cache:" << hadAuthCache;
        }

        const bool cacheOk = m_platformAuth->applyCache(data);
        // Personal: never club backup. Club: preserve needBackup from applyCache (scout path).
        if (personal) {
            m_platformAuth->setNeedsCacheBackup(false);
            qWarning() << "[SESSION]" << platform.toUpper()
                       << "личный аккаунт — clear session, без scout / backup";
        } else {
            const bool wantBackupAfterApply = m_platformAuth->needsCacheBackup();
            m_platformAuth->setNeedsCacheBackup(!cacheOk || wantBackupAfterApply);
            qWarning() << "[SESSION]" << platform.toUpper()
                       << (cacheOk ? "тихий вход (cache)" : "без cache → scout + backup после входа");
        }

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

        const QString login = data.value(QStringLiteral("login")).toString();
        const QString password = data.value(QStringLiteral("password")).toString();
        connect(m_process, &QProcess::started, this, [this, login, password, platform, personal]() {
            qWarning() << "[SESSION] process started:" << (m_process ? m_process->program() : QString())
                       << (m_process ? m_process->arguments() : QStringList());
            emit gameStarted();
            if (m_platformAuth)
                m_platformAuth->startScout(login, password);
            startGameFindPoll();

            // Личный аккаунт: НЕ hideShell, НЕ m_gamePid=launcher (иначе flicker → kill).
            // Ждём реальный game window; exit-watch только следит за смертью лаунчера.
            if (personal || login.isEmpty()) {
                QTimer::singleShot(2500, this, [this, platform]() {
                    if (!m_gameSessionActive || m_gameHwnd != 0)
                        return;

                    QString image = m_platformAuth
                        ? m_platformAuth->launcherProcessName()
                        : QString();
                    bool alive = !image.isEmpty() && isProcessRunning(image);
                    if (platform == QLatin1String("riot")) {
                        alive = isProcessRunning(QStringLiteral("RiotClientServices.exe"))
                                || isProcessRunning(QStringLiteral("RiotClientUx.exe"))
                                || isProcessRunning(QStringLiteral("LeagueClientUx.exe"));
                        if (alive) {
                            image = isProcessRunning(QStringLiteral("LeagueClientUx.exe"))
                                ? QStringLiteral("LeagueClientUx.exe")
                                : QStringLiteral("RiotClientServices.exe");
                        }
                    } else if (platform == QLatin1String("epic")) {
                        alive = isProcessRunning(QStringLiteral("EpicGamesLauncher.exe"))
                                || isProcessRunning(QStringLiteral("EpicWebHelper.exe"));
                        if (alive)
                            image = QStringLiteral("EpicGamesLauncher.exe");
                    } else if (platform == QLatin1String("ea")) {
                        alive = isProcessRunning(QStringLiteral("EADesktop.exe"))
                                || isProcessRunning(QStringLiteral("Origin.exe"));
                        if (alive)
                            image = QStringLiteral("EADesktop.exe");
                    } else if (platform == QLatin1String("steam")) {
                        alive = isProcessRunning(QStringLiteral("steam.exe"));
                        if (alive)
                            image = QStringLiteral("steam.exe");
                    }
                    if (!alive)
                        return;

                    qWarning() << "[SESSION] личный" << platform.toUpper()
                               << "— login wait (shell visible, no hide, no launcher-as-game)";
                    emit gameStartedSuccessfully(); // снять loading overlay → видна форма входа
                    // НЕ ставим m_gamePid / m_gameAcceptedAtMs на лаунчер — только флаг ожидания
                    m_personalLoginWait = true;
                    m_gamePid = 0;
                    m_gameProcessImage.clear();
                    m_gameAcceptedAtMs = 0;
                    m_gameGoneTicks = 0;
                    if (!m_gameFindTimer->isActive())
                        m_gameFindTimer->start();
                    if (!m_gameExitTimer->isActive())
                        m_gameExitTimer->start(2000);
                    setShellTopmost(false);
                    // hideShellForGame — только из acceptGameWindow после окна игры
                });
            }
        });

        m_platformAuth->startLauncher(m_process, data, appIdHint);
        qWarning() << "[SESSION] startLauncher queued:"
                   << (m_process ? m_process->program() : QString())
                   << (m_process ? m_process->arguments() : QStringList())
                   << "| state:" << (m_process ? int(m_process->state()) : -1);

        // Epic/Steam часто сразу завершают parent QProcess, лаунчер живёт отдельно.
        // Нельзя по NotRunning рвать scout через 8с.
        QTimer::singleShot(20000, this, [this, platform]() {
            if (!m_gameSessionActive || m_gameHwnd != 0)
                return;
            // Личный login-wait / принятый лаунчер — не фейлим по parent QProcess
            if (m_personalLoginWait || m_gamePid != 0 || m_gameAcceptedAtMs > 0) {
                qWarning() << "[SESSION]" << platform.toUpper()
                           << (m_personalLoginWait ? "personal login-wait" : "лаунчер в session watch")
                           << "— skip fail-timer";
                return;
            }

            if (m_platformAuth && !m_platformAuth->allowsGameDetect()) {
                qWarning() << "[SESSION]" << platform.toUpper()
                           << "QProcess мог завершиться — scout/логин ещё идёт, не фейлим";
                return;
            }

            const bool riotFamilyAlive =
                platform == QLatin1String("riot")
                && (isProcessRunning(QStringLiteral("RiotClientServices.exe"))
                    || isProcessRunning(QStringLiteral("RiotClientUx.exe"))
                    || isProcessRunning(QStringLiteral("LeagueClientUx.exe")));
            if (riotFamilyAlive) {
                qWarning() << "[SESSION] RIOT процесс жив — OK (parent QProcess мог отвалиться)";
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

    // Riot: League Client (CEF, часто 1280×720) или Valorant fullscreen
    if (m_currentPlatform == QLatin1String("riot")) {
        HWND hwnd = nullptr;
        QString cls;
        if (!findRiotProductWindow(&hwnd, &cls))
            return;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        qWarning() << "[SESSION] riot product window:" << cls
                   << "| exe:" << processImageForPid(pid);
        m_pendingGameHwnd = 0;
        m_pendingGameClass.clear();
        acceptGameWindow(reinterpret_cast<quintptr>(hwnd), cls);
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
    m_personalLoginWait = false; // реальная игра — выходим из login-wait
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

    // Epic/EA/Riot: бэкап после детекта игры (yaml/ini часто пишутся после логина).
    // Личный аккаунт — не трогаем клубный machine-cache на сервере.
    const bool platformAlwaysBackup = !m_personalAccount
        && (m_currentPlatform == QLatin1String("epic")
            || m_currentPlatform == QLatin1String("ea")
            || m_currentPlatform == QLatin1String("riot"));
    if (m_platformAuth && !m_personalAccount
        && (m_platformAuth->needsCacheBackup() || platformAlwaysBackup)) {
        // Riot: Cookies/Sessions CEF дописываются после RSO — ждём дольше (persist + flush)
        const int delayMs = (m_currentPlatform == QLatin1String("riot")) ? 10000 : 3000;
        QTimer::singleShot(delayMs, this, [this, platformAlwaysBackup]() {
            if (!m_platformAuth)
                return;
            if (!m_gameSessionActive && !platformAlwaysBackup)
                return;
            qWarning() << "[" << m_currentPlatform.toUpper()
                       << "] cache backup → сервер";
            m_platformAuth->backupCache(m_netManager, m_currentTerminalId, m_currentLogin,
                                        m_currentAccountId, m_currentGameId);
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

    // League Client: только видимое окно (невидимый HWND потом «пропадает» → ложный game closed)
    if (m_currentPlatform == QLatin1String("riot") && isRiotLeagueProcess(pid)
        && isLeagueClientSized(h) && IsWindowVisible(h)) {
        qWarning() << "[SESSION] League Client → accept:" << className << "| exe:" << img;
        acceptGameWindow(hwnd, className);
        return;
    }
    if (m_currentPlatform == QLatin1String("riot") && isRiotLeagueProcess(pid)
        && isLeagueClientSized(h) && !IsWindowVisible(h)) {
        qWarning() << "[SESSION] League ещё не visible — ждём:" << className << "| exe:" << img;
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
    setHasActiveGame(true);
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
    if (m_currentPlatform == QLatin1String("riot")) {
        HWND hwnd = nullptr;
        QString cls;
        if (!findRiotProductWindow(&hwnd, &cls))
            return false;
        if (outHwnd)
            *outHwnd = reinterpret_cast<quintptr>(hwnd);
        return true;
    }

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
            qWarning() << "[SESSION] rebind game hwnd → pid" << m_gamePid << m_gameProcessImage;
        }
        m_gameGoneTicks = 0;
        return;
    }

    // Riot: LeagueClientUx часто меняет HWND при загрузке — пока процесс жив, сессию не рвём
    if (m_currentPlatform == QLatin1String("riot")) {
        const bool leagueAlive = isProcessRunning(QStringLiteral("LeagueClientUx.exe"))
            || isProcessRunning(QStringLiteral("LeagueClient.exe"))
            || isProcessRunning(QStringLiteral("VALORANT-Win64-Shipping.exe"));
        if (leagueAlive || isPidAlive(m_gamePid)) {
            m_gameGoneTicks = 0;
            return;
        }
    }

    // Личный login-wait: лаунчер ≠ игра. Сессия жива, пока лаунчер жив; не «game window closed».
    if ((m_personalLoginWait || m_personalAccount) && m_gameHwnd == 0) {
        bool launcherAlive = false;
        if (m_platformAuth) {
            const QString img = m_platformAuth->launcherProcessName();
            if (!img.isEmpty())
                launcherAlive = isProcessRunning(img);
        }
        if (!launcherAlive && m_currentPlatform == QLatin1String("riot")) {
            launcherAlive = isProcessRunning(QStringLiteral("RiotClientServices.exe"))
                            || isProcessRunning(QStringLiteral("RiotClientUx.exe"))
                            || isProcessRunning(QStringLiteral("LeagueClientUx.exe"));
        }
        if (!launcherAlive && m_currentPlatform == QLatin1String("steam"))
            launcherAlive = isProcessRunning(QStringLiteral("steam.exe"));
        if (!launcherAlive && m_currentPlatform == QLatin1String("epic")) {
            launcherAlive = isProcessRunning(QStringLiteral("EpicGamesLauncher.exe"))
                            || isProcessRunning(QStringLiteral("EpicWebHelper.exe"));
        }
        if (!launcherAlive && m_currentPlatform == QLatin1String("ea")) {
            launcherAlive = isProcessRunning(QStringLiteral("EADesktop.exe"))
                            || isProcessRunning(QStringLiteral("Origin.exe"));
        }
        if (launcherAlive) {
            m_gameGoneTicks = 0;
            return;
        }
        // Лаунчер реально умер во время ручного логина
        ++m_gameGoneTicks;
        qWarning() << "[SESSION] personal login-wait: launcher gone tick" << m_gameGoneTicks
                   << "| platform:" << m_currentPlatform;
        if (m_gameGoneTicks >= 3)
            finishGameSession(QStringLiteral("launcher closed during personal login"));
        return;
    }

    // Не завершать сессию, если watched pid — сам лаунчер (защита от старых путей)
    if (m_gameHwnd == 0 && !m_gameProcessImage.isEmpty()) {
        const QString img = m_gameProcessImage.toLower();
        const bool watchedIsLauncher =
            img.contains(QStringLiteral("eadesktop"))
            || img.contains(QStringLiteral("origin.exe"))
            || img.contains(QStringLiteral("epicgameslauncher"))
            || img.contains(QStringLiteral("epicwebhelper"))
            || img.contains(QStringLiteral("steam.exe"))
            || img.contains(QStringLiteral("riotclient"));
        if (watchedIsLauncher) {
            const bool alive = isPidAlive(m_gamePid) || isProcessRunning(m_gameProcessImage);
            if (alive) {
                m_gameGoneTicks = 0;
                return;
            }
        }
    }

    // Grace только для splash→main (окно пропало, процесс ещё грузит).
    // Не применять grace к лаунчеру — только к реальной игре (m_gameHwnd был принят).
    const qint64 aliveForMs = QDateTime::currentMSecsSinceEpoch() - m_gameAcceptedAtMs;
    if (m_gameAcceptedAtMs > 0 && aliveForMs < 45000 && isPidAlive(m_gamePid)
        && !m_personalLoginWait) {
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
    setHasActiveGame(false);
    showShellToggle(false);
    m_personalLoginWait = false;
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
    // (showShellAfterGame ends the *visual* session return — not the mid-session toggle path)
    showShellAfterGame();
    setGameTitle(QString());
    m_currentGameId = 0;
    emit gameFinished();

    // Riot: soft close → ждать flush CEF (persist yaml) → backup → force kill.
    // Жёсткий taskkill /F сразу после логина сбрасывает Persisting 0 cookies.
    if (m_currentPlatform == QLatin1String("riot") && m_platformAuth) {
        auto *riot = qobject_cast<RiotAuth *>(m_platformAuth);
        if (riot) {
            riot->prepareGracefulShutdown();
            const QString login = m_currentLogin;
            const int termId = m_currentTerminalId;
            const int accountId = m_currentAccountId;
            const int gameId = m_currentGameId;
            const bool needBackup = !m_personalAccount
                && (riot->needsCacheBackup() || riot->didInteractiveLogin());
            IPlatformAuth *auth = m_platformAuth;
            NetworkManager *net = m_netManager;
            qWarning() << "[SESSION] Riot graceful: ждём 10s flush yaml, потом"
                       << (needBackup ? "backup+forceKill" : "forceKill (personal — без backup)");
            QTimer::singleShot(10000, this, [riot, auth, net, termId, login, accountId, gameId, needBackup]() {
                if (needBackup && auth && net) {
                    qWarning() << "[SESSION] Riot backup после soft-close flush";
                    auth->backupCache(net, termId, login, accountId, gameId);
                    auth->setNeedsCacheBackup(false);
                }
                if (riot)
                    riot->forceKillRemaining();
            });
            m_netWatchTimer->start(5000);
            return;
        }
    }

    if (m_platformAuth)
        m_platformAuth->killLauncher();

    if (m_platformAuth && !m_personalAccount
        && (m_platformAuth->needsCacheBackup()
            || m_currentPlatform == QLatin1String("epic")
            || m_currentPlatform == QLatin1String("ea")
            || m_currentPlatform == QLatin1String("riot"))) {
        const QString login = m_currentLogin;
        const int termId = m_currentTerminalId;
        const int accountId = m_currentAccountId;
        const int gameId = m_currentGameId;
        IPlatformAuth *auth = m_platformAuth;
        NetworkManager *net = m_netManager;
        qWarning() << "[SESSION] cache backup после killLauncher (отложенный)";
        const int postKillMs = (m_currentPlatform == QLatin1String("riot")) ? 8000 : 2500;
        QTimer::singleShot(postKillMs, this, [auth, net, termId, login, accountId, gameId]() {
            if (!auth || !net)
                return;
            auth->backupCache(net, termId, login, accountId, gameId);
            auth->setNeedsCacheBackup(false);
        });
    }

    m_netWatchTimer->start(5000);
}

void ProcessManager::backupAndSendVdfPayload()
{
    if (m_platformAuth)
        m_platformAuth->backupCache(m_netManager, m_currentTerminalId, m_currentLogin,
                                    m_currentAccountId, m_currentGameId);
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    Q_UNUSED(exitCode);
    if (m_gameSessionActive) {
        if (m_gameHwnd != 0)
            return;

        // EA/Riot: parent QProcess часто отваливается, лаунчер/игра живут отдельно.
        if (m_currentPlatform == QLatin1String("ea")
            || m_currentPlatform == QLatin1String("riot")) {
            qWarning() << "[SESSION]" << m_currentPlatform.toUpper()
                       << "parent QProcess finished — сессию не трогаем"
                       << "(ждём окно игры / выход пользователя)";
            return;
        }

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
                qWarning() << "[SESSION] parent exited, but" << image << "still running — keep session";
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
    // Не блокируем UI (powershell New-NetQosPolicy раньше ждал до 3 с на логине).
    auto *qosProc = new QProcess(this);
    QObject::connect(qosProc, &QProcess::finished, qosProc, &QObject::deleteLater);
    QObject::connect(qosProc, &QProcess::errorOccurred, qosProc, &QObject::deleteLater);
    if (enable) {
        qosProc->start(QStringLiteral("powershell"), {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-Command"),
            QStringLiteral("New-NetQosPolicy -Name 'ReactorGameTraffic' -AppPathNameMatchCondition 'steam.exe' -DSCPAction 46 -Confirm:$false -ErrorAction SilentlyContinue")
        });
    } else {
        qosProc->start(QStringLiteral("powershell"), {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-Command"),
            QStringLiteral("Remove-NetQosPolicy -Name 'ReactorGameTraffic' -Confirm:$false -ErrorAction SilentlyContinue")
        });
    }
#else
    Q_UNUSED(enable);
#endif
}

void ProcessManager::setSystemVolume(int level)
{
#ifdef Q_OS_WIN
    // waveOutSetVolume does not control the modern Windows mixer / default
    // playback endpoint — use IAudioEndpointVolume via audiomanager_win.
    int clamped = level;
    if (clamped < 0) clamped = 0;
    if (clamped > 100) clamped = 100;
    qWarning() << "[AUDIO] setVolume" << clamped << "(from UI)";
    win32_set_master_volume(clamped);
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

void ProcessManager::rebootPC()
{
#ifdef Q_OS_WIN
    qWarning() << "[SYSTEM] rebootPC: shutdown /r /t 0";
    const bool ok = QProcess::startDetached(
        QStringLiteral("shutdown"),
        {QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0")});
    if (!ok)
        qWarning() << "[SYSTEM] rebootPC: не удалось запустить shutdown.exe";
#else
    qWarning() << "[SYSTEM] rebootPC: не поддерживается на этой платформе";
#endif
}

void ProcessManager::applyPowerAction(const QString &action)
{
    // Заглушка: реальное выключение/перезагрузка пока отключены (иначе долгий цикл отладки).
    if (action == QLatin1String("reboot")) {
        qWarning() << "[SYSTEM] applyPowerAction: REBOOT stub — реальный reboot отключён";
        // rebootPC();
        return;
    }
    if (action == QLatin1String("shutdown")) {
        qWarning() << "[SYSTEM] applyPowerAction: SHUTDOWN stub — реальный shutdown отключён";
        // QProcess::startDetached("shutdown", {"/s", "/t", "0"});
        return;
    }
    qWarning() << "[SYSTEM] applyPowerAction: неизвестное действие" << action;
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
