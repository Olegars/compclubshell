#include "processmanager.h"
#include <QProcess>
#include <QDateTime>
#include <QDir>
#include <QSettings>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStringConverter>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>

HWINEVENTHOOK g_pGameHook = nullptr;
ProcessManager* g_pProcessManagerInstance = nullptr;

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    Q_UNUSED(idObject);
    Q_UNUSED(idChild);
    Q_UNUSED(dwEventThread);
    Q_UNUSED(dwmsEventTime);

    if (event == EVENT_SYSTEM_FOREGROUND && hwnd) {
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            if (width >= screenWidth && height >= screenHeight) {
                char className[256];
                GetClassNameA(hwnd, className, sizeof(className));
                QString clsStr(className);

                if (clsStr.startsWith("Qt", Qt::CaseInsensitive)
                    || clsStr == "Progman"
                    || clsStr == "WorkerW"
                    || clsStr == "SDL_app"
                    || clsStr == "vguiPopupWindow"
                    || clsStr.contains("Chrome_WidgetWin", Qt::CaseInsensitive)) {
                    return;
                }

                UnhookWinEvent(hWinEventHook);
                g_pGameHook = nullptr;

                if (g_pProcessManagerInstance) {
                    g_pProcessManagerInstance->onGameWindowFound(
                        reinterpret_cast<quintptr>(hwnd), clsStr);
                }
            }
        }
    }
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
#endif

static bool writeTextFile(const QString &path, const QString &content)
{
    if (content.isEmpty())
        return false;
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[STEAM] VDF write fail:" << path << file.errorString();
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

ProcessManager::ProcessManager(NetworkManager *netManager, QObject *parent)
    : QObject(parent)
    , m_process(nullptr)
    , m_mainWindow(nullptr)
    , m_netManager(netManager)
    , m_netWatchTimer(new QTimer(this))
    , m_gameExitTimer(new QTimer(this))
    , m_authScoutTimer(nullptr)
    , m_alertActive(false)
    , m_offendingPid(0)
    , m_highActivityCounter(0)
    , m_currentTerminalId(1)
    , m_currentGameId(0)
    , m_gameSessionActive(false)
    , m_gameHwnd(0)
    , m_gameGoneTicks(0)
    , m_needVdfBackup(false)
    , m_scoutTicks(0)
    , m_scoutInjected(false)
{
#ifdef Q_OS_WIN
    g_pProcessManagerInstance = this;
#endif
    connect(m_netWatchTimer, &QTimer::timeout, this, &ProcessManager::monitorNetworkTraffic);
    connect(m_gameExitTimer, &QTimer::timeout, this, &ProcessManager::checkGameExit);
    m_netWatchTimer->start(5000);
}

ProcessManager::~ProcessManager()
{
#ifdef Q_OS_WIN
    if (g_pGameHook) UnhookWinEvent(g_pGameHook);
#endif
}

void ProcessManager::setMainWindow(QWindow *window) {
    m_mainWindow = window;
}

QString ProcessManager::localAppDataSteamVdfPath() const
{
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty())
        return QString();
    return localAppData + "/Steam/local.vdf";
}

QString ProcessManager::buildLoginUsersVdf(const QString &steamId, const QString &login, const QString &persona, const QString &existing) const
{
    if (!existing.trimmed().isEmpty() && !steamId.isEmpty() && existing.contains(steamId))
        return existing;

    const QString id = steamId.isEmpty() ? QStringLiteral("0") : steamId;
    const QString name = persona.isEmpty() ? login : persona;
    const QString ts = QString::number(QDateTime::currentSecsSinceEpoch());

    return QString(
        "\"users\"\n"
        "{\n"
        "\t\"%1\"\n"
        "\t{\n"
        "\t\t\"AccountName\"\t\t\"%2\"\n"
        "\t\t\"PersonaName\"\t\t\"%3\"\n"
        "\t\t\"RememberPassword\"\t\t\"1\"\n"
        "\t\t\"WantsOfflineMode\"\t\t\"0\"\n"
        "\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\n"
        "\t\t\"AllowAutoLogin\"\t\t\"1\"\n"
        "\t\t\"MostRecent\"\t\t\"1\"\n"
        "\t\t\"Timestamp\"\t\t\"%4\"\n"
        "\t}\n"
        "}\n"
    ).arg(id, login, name, ts);
}

void ProcessManager::killSteamProcesses()
{
    const bool steamUp = isProcessRunning("steam.exe");
    const bool helperUp = isProcessRunning("steamwebhelper.exe");
    if (!steamUp && !helperUp)
        return;

    QProcess killProcess;
    killProcess.start("taskkill", QStringList() << "/F" << "/T" << "/IM" << "steam.exe");
    killProcess.waitForFinished(5000);
    if (isProcessRunning("steamwebhelper.exe")) {
        QProcess killHelper;
        killHelper.start("taskkill", QStringList() << "/F" << "/T" << "/IM" << "steamwebhelper.exe");
        killHelper.waitForFinished(3000);
    }
    QThread::msleep(500);
}

bool ProcessManager::applySteamAuthFiles(const QJsonObject &authData, const QString &steamPath)
{
    const QString login = authData.value("login").toString();
    const QString steamId = authData.value("steam_id").toString();
    const QString persona = authData.value("persona_name").toString(login);
    const QJsonObject vdf = authData.value("vdf_files").toObject();

    const QString configDir = steamPath + "/config";
    QDir().mkpath(configDir);

    const QString configVdf = vdf.value("config_vdf").toString();
    if (!configVdf.isEmpty())
        writeTextFile(configDir + "/config.vdf", configVdf);

    const QString loginUsers = buildLoginUsersVdf(
        steamId, login, persona, vdf.value("loginusers_vdf").toString()
    );
    writeTextFile(configDir + "/loginusers.vdf", loginUsers);

    const QString cachedLocal = vdf.value("local_vdf").toString().trimmed();
    if (!cachedLocal.isEmpty()) {
        writeTextFile(configDir + "/local.vdf", cachedLocal);
        const QString appLocal = localAppDataSteamVdfPath();
        if (!appLocal.isEmpty())
            writeTextFile(appLocal, cachedLocal);
        return true;
    }

    qWarning() << "[STEAM] Нет machine-cache — нужен логин/пароль";
    return false;
}

void ProcessManager::stopAuthScout()
{
    if (!m_authScoutTimer)
        return;
    m_authScoutTimer->stop();
    m_authScoutTimer->deleteLater();
    m_authScoutTimer = nullptr;
}

#ifdef Q_OS_WIN
static void parkWindowOffscreen(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    // Уводим за край, не трогая z-order лишний раз — заставка остаётся видимой
    SetWindowPos(hwnd, nullptr, -20000, -20000, w, h,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

struct SteamLoginEnumCtx {
    HWND best = nullptr;
    QString bestTitle;
    int bestScore = -1;
};

static BOOL CALLBACK enumSteamLoginProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<SteamLoginEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();
    if (t.isEmpty())
        return TRUE;

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString clsStr = QString::fromWCharArray(wcls);

    if (clsStr == QStringLiteral("CabinetWClass")
        || clsStr.startsWith(QStringLiteral("Qt"), Qt::CaseInsensitive)
        || clsStr.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
        || clsStr.contains(QStringLiteral("Tray"), Qt::CaseInsensitive)
        || clsStr == QStringLiteral("Progman")
        || clsStr == QStringLiteral("WorkerW"))
        return TRUE;

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;

    // Уже уведённые за экран тоже учитываем (для повторного park / inject)
    const bool offscreen = (rc.left <= -5000 || rc.top <= -5000);

    if (!offscreen && (w < 480 || hgt < 320 || w >= 1000 || hgt >= 700))
        return TRUE;

    const bool hasLoginWord =
        t.contains(QStringLiteral("Вход"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("Sign"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("Login"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("Аккаунт"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("Account"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("Guard"), Qt::CaseInsensitive);

    const bool isSteamTitle =
        t.compare(QStringLiteral("Steam"), Qt::CaseInsensitive) == 0
        || t.contains(QStringLiteral("Steam"), Qt::CaseInsensitive);

    if (!hasLoginWord && !isSteamTitle)
        return TRUE;

    const bool isSdl = clsStr.contains(QStringLiteral("SDL"), Qt::CaseInsensitive);
    if (!hasLoginWord && !isSdl)
        return TRUE;

    int score = 0;
    if (hasLoginWord) score += 50;
    if (isSdl) score += 40;
    if (w > 600 && w < 900 && hgt > 380 && hgt < 550) score += 20;

    if (score > c->bestScore) {
        c->bestScore = score;
        c->best = h;
        c->bestTitle = t + QStringLiteral(" [") + clsStr + QLatin1Char(' ')
                       + QString::number(w) + QLatin1Char('x') + QString::number(hgt) + QLatin1Char(']');
    }
    return TRUE;
}
#endif

void ProcessManager::startAuthScout(const QString &login, const QString &password)
{
#ifdef Q_OS_WIN
    stopAuthScout();
    m_scoutTicks = 0;
    m_scoutInjected = false;

    m_authScoutTimer = new QTimer(this);
    m_authScoutTimer->setInterval(200);

    connect(m_authScoutTimer, &QTimer::timeout, this, [this, login, password]() {
        if (!m_authScoutTimer)
            return;

        if (m_gameHwnd != 0) {
            stopAuthScout();
            return;
        }

        ++m_scoutTicks;

        if (m_scoutTicks > 250) {
            if (m_gameHwnd == 0 && !m_scoutInjected)
                qWarning() << "[STEAM] Scout timeout — окно входа не найдено";
            stopAuthScout();
            return;
        }

        SteamLoginEnumCtx ctx;
        EnumWindows(enumSteamLoginProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.best)
            return;

        // Сразу уводим диалог за экран — не светится поверх заставки
        parkWindowOffscreen(ctx.best);

        if (m_scoutInjected)
            return; // продолжаем только прятать, пока не стартует игра

        if (m_scoutTicks < 12)
            return;

        m_scoutInjected = true;
        HWND authHwnd = ctx.best;
        qWarning() << "[STEAM] Интерактивный логин (off-screen):" << ctx.bestTitle;

        // Фокус без вытаскивания на монитор
        SetForegroundWindow(authHwnd);
        SetFocus(authHwnd);
        QThread::msleep(400);
        parkWindowOffscreen(authHwnd);

        auto pasteText = [](const QString &text) {
            if (!OpenClipboard(NULL))
                return;
            EmptyClipboard();
            const std::wstring wstr = text.toStdWString();
            const size_t bytes = (wstr.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!hMem) {
                CloseClipboard();
                return;
            }
            memcpy(GlobalLock(hMem), wstr.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
            CloseClipboard();

            keybd_event(VK_CONTROL, 0, 0, 0);
            keybd_event('V', 0, 0, 0);
            keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        };

        pasteText(login);
        keybd_event(VK_TAB, 0, 0, 0);
        keybd_event(VK_TAB, 0, KEYEVENTF_KEYUP, 0);
        QThread::msleep(300);
        pasteText(password);
        QThread::msleep(200);
        keybd_event(VK_RETURN, 0, 0, 0);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);

        parkWindowOffscreen(authHwnd);
        qDebug() << "[STEAM] Логин/пароль отправлены";
        m_needVdfBackup = true;
        // Таймер не стопаем — пока игра не откроется, диалог Steam держим за экраном
    });

    m_authScoutTimer->start();
#else
    Q_UNUSED(login);
    Q_UNUSED(password);
#endif
}

void ProcessManager::launchGameWithSmartAuth(const QJsonObject &authData, const QString &steamAppId)
{
    QString appId = steamAppId.trimmed();
    const QString argsStr = authData.value("args").toString().trimmed();

    if (appId.isEmpty() && !argsStr.isEmpty()) {
        QRegularExpression reAppl("-applaunch\\s+(\\d+)");
        QRegularExpressionMatch match = reAppl.match(argsStr);
        if (match.hasMatch()) {
            appId = match.captured(1);
        } else {
            // Часто в БД лежит просто "440 -novid ..." без -applaunch
            QRegularExpression reNum("^(\\d{2,})\\b");
            match = reNum.match(argsStr);
            if (match.hasMatch())
                appId = match.captured(1);
        }
    }

    if (authData.contains("terminal_id"))
        m_currentTerminalId = authData["terminal_id"].toInt();
    if (authData.contains("game_id"))
        m_currentGameId = authData["game_id"].toInt();
    if (authData.contains("login"))
        m_currentSteamLogin = authData["login"].toString();

    qDebug() << "[STEAM] Launch appId:" << (appId.isEmpty() ? "none" : appId)
             << "| login:" << m_currentSteamLogin
             << "| pc:" << m_currentTerminalId;

#ifdef Q_OS_WIN
    QSettings settings("REACTOR", "REACTOR SHELL");
    QString steamPath = settings.value("Paths/steam_path", "C:/Program Files (x86)/Steam").toString();

    killSteamProcesses();

    m_gameSessionActive = true;
    m_gameHwnd = 0;
    m_gameWindowClass.clear();
    m_gameGoneTicks = 0;
    m_needVdfBackup = false;
    m_gameExitTimer->stop();

    const bool cacheApplied = applySteamAuthFiles(authData, steamPath);
    // Нет кэша с сервера → после успешного входа обязательно сохраним VDF в БД
    // (даже если Steam взял сессию с локального диска без окна логина / scout).
    m_needVdfBackup = !cacheApplied;
    qDebug() << "[STEAM]" << (cacheApplied ? "тихий вход (VDF cache)" : "без cache → scout + backup после входа");

    if (g_pGameHook) UnhookWinEvent(g_pGameHook);
    g_pGameHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
    );

    if (m_process) {
        m_process->disconnect();
        m_process->deleteLater();
    }
    m_process = new QProcess(this);
    connect(m_process, &QProcess::errorOccurred, this, &ProcessManager::onProcessError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ProcessManager::onProcessFinished);

    QString steamExe = steamPath + "/steam.exe";
    m_process->setWorkingDirectory(steamPath);

    QStringList args;
    if (!appId.isEmpty() && appId != "0")
        args << "-applaunch" << appId;

    const QStringList passthrough = {
        QStringLiteral("-novid"),
        QStringLiteral("-nojoy"),
        QStringLiteral("-silent"),
        QStringLiteral("-shutdown"),
    };
    for (const QString &flag : passthrough) {
        if (argsStr.contains(flag, Qt::CaseInsensitive) && !args.contains(flag))
            args << flag;
    }
    if (!args.contains(QStringLiteral("-shutdown")))
        args << QStringLiteral("-shutdown");

    m_process->start(steamExe, args);
    if (!m_process->waitForStarted(5000)) {
        qCritical() << "[STEAM] Не удалось запустить steam.exe:" << m_process->errorString();
        m_gameSessionActive = false;
        emit gameFinished();
        return;
    }
    emit gameStarted();

    startAuthScout(authData.value("login").toString(), authData.value("password").toString());
#else
    Q_UNUSED(authData);
#endif
}

void ProcessManager::onGameWindowFound(quintptr hwnd, const QString &className)
{
    qDebug() << "[STEAM] Игра запущена:" << className;
    stopAuthScout();
    emit gameStartedSuccessfully();
    startGameExitWatch(hwnd, className);

    if (m_needVdfBackup) {
        // После scout ConnectCache пишется не сразу; при локальном входе без scout
        // файлы уже на диске — чуть ждём и в обоих случаях шлём в БД.
        const int delayMs = m_scoutInjected ? 5000 : 2500;
        QTimer::singleShot(delayMs, this, [this]() {
            if (!m_gameSessionActive || !m_needVdfBackup)
                return;
            qDebug() << "[STEAM] VDF backup после входа (кэша на сервере не было)";
            backupAndSendVdfPayload();
            m_needVdfBackup = false;
        });
    }
}

void ProcessManager::startGameExitWatch(quintptr hwnd, const QString &className)
{
    m_gameHwnd = hwnd;
    m_gameWindowClass = className;
    m_gameGoneTicks = 0;
    m_gameSessionActive = true;
    if (!m_gameExitTimer->isActive())
        m_gameExitTimer->start(2000);
}

bool ProcessManager::isGameWindowAlive() const
{
#ifdef Q_OS_WIN
    if (m_gameHwnd && IsWindow(reinterpret_cast<HWND>(m_gameHwnd)))
        return true;
    return enumHasGameClass(m_gameWindowClass);
#else
    return false;
#endif
}

void ProcessManager::checkGameExit()
{
    if (!m_gameSessionActive)
        return;

    if (isGameWindowAlive()) {
        m_gameGoneTicks = 0;
        return;
    }

    ++m_gameGoneTicks;
    if (m_gameGoneTicks >= 2)
        finishGameSession(QStringLiteral("game window closed"));
}

void ProcessManager::finishGameSession(const QString &reason)
{
    if (!m_gameSessionActive)
        return;

    m_gameSessionActive = false;
    m_gameExitTimer->stop();
    m_gameHwnd = 0;
    m_gameGoneTicks = 0;
    stopAuthScout();

    qDebug() << "[STEAM] Сессия завершена:" << reason;

    if (m_needVdfBackup) {
        backupAndSendVdfPayload();
        m_needVdfBackup = false;
    }

    killSteamProcesses();

    if (m_mainWindow) {
        m_mainWindow->show();
        m_mainWindow->raise();
        m_mainWindow->requestActivate();
    }
    emit gameFinished();
}

void ProcessManager::backupAndSendVdfPayload() {
#ifdef Q_OS_WIN
    if (m_currentSteamLogin.isEmpty()) {
        qWarning() << "[STEAM] VDF backup: login пуст";
        return;
    }

    QSettings settings("REACTOR", "REACTOR SHELL");
    QString steamPath = settings.value("Paths/steam_path", "C:/Program Files (x86)/Steam").toString();
    QString configDir = steamPath + "/config";

    QString configVdf = readTextFile(configDir + "/config.vdf");
    QString loginusersVdf = readTextFile(configDir + "/loginusers.vdf");

    QString localVdf = readTextFile(localAppDataSteamVdfPath());
    if (localVdf.isEmpty())
        localVdf = readTextFile(configDir + "/local.vdf");

    QJsonObject rootPayload;
    rootPayload["login"] = m_currentSteamLogin;
    rootPayload["terminal_id"] = m_currentTerminalId;
    rootPayload["config_vdf"] = configVdf;
    rootPayload["loginusers_vdf"] = loginusersVdf;
    rootPayload["local_vdf"] = localVdf;

    if (!m_netManager || m_netManager->serverUrl().isEmpty()) {
        qWarning() << "[STEAM] VDF backup: serverUrl пуст";
        return;
    }

    QUrl url(m_netManager->serverUrl() + "/api/shell/games/update-vdf");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qDebug() << "[STEAM] VDF backup → server, bytes:" << jsonData.size();

    QNetworkReply *reply = m_netManager->networkAccessManager()->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            qDebug() << "[STEAM] VDF backup OK";
        else
            qWarning() << "[STEAM] VDF backup fail:" << reply->errorString();
    });
#endif
}


void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitStatus);
    if (m_gameSessionActive) {
        if (m_gameHwnd == 0) {
            QTimer::singleShot(4000, this, [this]() {
                if (!m_gameSessionActive || m_gameHwnd != 0)
                    return;
                if (!isProcessRunning("steam.exe"))
                    finishGameSession(QStringLiteral("steam exited before game window"));
            });
        }
        return;
    }
    Q_UNUSED(exitCode);
    if (m_mainWindow) {
        m_mainWindow->show();
        m_mainWindow->raise();
        m_mainWindow->requestActivate();
    }
    emit gameFinished();
}

void ProcessManager::onProcessError(QProcess::ProcessError error) {
    qCritical() << "[STEAM] Process error:" << error;
    if (m_gameSessionActive)
        return;
    emit gameFinished();
}

bool ProcessManager::isProcessRunning(const QString &processName) {
#ifdef Q_OS_WIN
    QProcess proc;
    proc.start("tasklist", QStringList() << "/FI" << QString("IMAGENAME eq %1").arg(processName));
    proc.waitForFinished(1000);
    return QString::fromUtf8(proc.readAllStandardOutput()).contains(processName, Qt::CaseInsensitive);
#else
    return false;
#endif
}

unsigned long ProcessManager::getProcessIdByName(const QString &processName) {
#ifdef Q_OS_WIN
    QProcess proc;
    proc.start("powershell", QStringList() << "-Command" << "Get-Process -Name " + processName.split('.').first() + " -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id");
    proc.waitForFinished(1000);
    bool ok;
    unsigned long pid = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toULong(&ok);
    return ok ? pid : 0;
#else
    return 0;
#endif
}

void ProcessManager::monitorNetworkTraffic() {
    if (m_alertActive) return;
#ifdef Q_OS_WIN
    if (isProcessRunning("steam.exe")) {
        QProcess netCheck;
        netCheck.start("powershell", QStringList() << "-Command" << "Get-Counter '\\Network Interface(*)\\Bytes Received/sec' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty CounterSamples | Select-Object -ExpandProperty CookedValue");
        netCheck.waitForFinished(1500);

        bool ok;
        double bytesPerSec = QString::fromUtf8(netCheck.readAllStandardOutput()).trimmed().toDouble(&ok);
        if (ok && bytesPerSec > 40000000) {
            m_highActivityCounter++;
            if (m_highActivityCounter >= 3) {
                m_alertActive = true;
                m_offendingPid = getProcessIdByName("steam.exe");
                emit heavyDownloadDetected("Steam Update");
            }
        } else {
            m_highActivityCounter = 0;
        }
    }
#endif
}

void ProcessManager::applyQosPolicies(bool enable) {
#ifdef Q_OS_WIN
    QProcess qosProc;
    if (enable) {
        qosProc.start("powershell", QStringList() << "-Command" << "New-NetQosPolicy -Name 'ReactorGameTraffic' -AppPathNameMatchCondition 'steam.exe' -DSCPAction 46 -Confirm:$false");
    } else {
        qosProc.start("powershell", QStringList() << "-Command" << "Remove-NetQosPolicy -Name 'ReactorGameTraffic' -Confirm:$false");
    }
    qosProc.waitForFinished(3000);
#endif
}

void ProcessManager::setSystemVolume(int level) {
#ifdef Q_OS_WIN
    HMODULE hWinmm = GetModuleHandleA("winmm.dll");
    if (!hWinmm) hWinmm = LoadLibraryA("winmm.dll");
    if (hWinmm) {
        typedef MMRESULT (WINAPI *WaveOutSetVolumeProto)(HWAVEOUT, DWORD);
        WaveOutSetVolumeProto pWaveOutSetVolume = reinterpret_cast<WaveOutSetVolumeProto>(GetProcAddress(hWinmm, "waveOutSetVolume"));
        if (pWaveOutSetVolume) {
            DWORD winVol = (0xFFFF * level) / 100;
            pWaveOutSetVolume(nullptr, DWORD(winVol | (winVol << 16)));
        }
    }
#endif
}

void ProcessManager::toggleSystemLanguage() {
#ifdef Q_OS_WIN
    HWND activeWnd = GetForegroundWindow();
    if (activeWnd) {
        PostMessageA(activeWnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)HKL_NEXT);
    }
#endif
}

void ProcessManager::handleDownloadDecision(bool continueDownload) {
    if (!continueDownload && m_offendingPid != 0) {
#ifdef Q_OS_WIN
        QProcess killProc;
        killProc.start("taskkill", QStringList() << "/F" << "/PID" << QString::number(m_offendingPid));
        killProc.waitForFinished(2000);
#endif
    }
    m_alertActive = false;
    m_offendingPid = 0;
    m_highActivityCounter = 0;
}

void ProcessManager::applyEnterprisePolicies(bool enable) {
#ifdef Q_OS_WIN
    QSettings systemReg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", QSettings::NativeFormat);
    if (enable) {
        systemReg.setValue("DisableTaskMgr", 1);
        systemReg.setValue("DisableRegistryTools", 1);
    } else {
        systemReg.remove("DisableTaskMgr");
        systemReg.remove("DisableRegistryTools");
    }
#endif
}
void ProcessManager::launchGameWithSmartAuthString(const QString &jsonString, const QString &steamAppId)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qCritical() << "[STEAM] JSON parse error:" << parseError.errorString();
        return;
    }

    this->launchGameWithSmartAuth(doc.object(), steamAppId);
}