// Путь: src/core/processmanager.cpp
#include "processmanager.h"
#include "audiomanager_win.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QProcess>
#include <QTimer>
#include <QRegularExpression>
#include <QTextStream>
#include <QMessageAuthenticationCode>
#include <QTimeZone>
#include <QDateTime>
#include <windows.h>

#ifdef Q_OS_WIN
HHOOK ProcessManager::keyboardHook = nullptr;

LRESULT CALLBACK ProcessManager::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;
        bool bCtrlDown = GetAsyncKeyState(VK_CONTROL) >> ((sizeof(SHORT) * 8) - 1);
        bool bAltDown = p->flags & LLKHF_ALTDOWN;

        if ((p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) ||
            (p->vkCode == VK_TAB && bAltDown) ||
            (p->vkCode == VK_ESCAPE && bAltDown) ||
            (p->vkCode == VK_ESCAPE && bCtrlDown))
        {
            return 1;
        }
    }
    return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
}

// Структура для передачи параметров внутрь Callback-функции Win32
struct TargetWindowData {
    DWORD processId;
    HWND foundHwnd;
    bool isBorderless;
};

// Callback-функция Win32 для поиска Borderless-окна игры
BOOL CALLBACK FindBorderlessWindowCallback(HWND hwnd, LPARAM lParam) {
    TargetWindowData *data = reinterpret_cast<TargetWindowData*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);

    // Если окно принадлежит процессу нашей игры и оно видимое
    if (windowPid == data->processId && IsWindowVisible(hwnd)) {
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;

            // Получаем разрешение текущего рабочего стола
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            // Читаем Win32 стили окна
            LONG style = GetWindowLong(hwnd, GWL_STYLE);

            // ДЕТЕКТ БOРДЕРЛЕССА: Размеры окна равны или больше экрана,
            // при этом у окна НЕТ классического заголовка (WS_CAPTION)
            if (width >= screenWidth && height >= screenHeight && !(style & WS_CAPTION)) {
                data->foundHwnd = hwnd;
                data->isBorderless = true;
                return FALSE; // Окно без рамки найдено, прекращаем перебор!
            }
        }
    }
    return TRUE; // Продолжаем перебор окон
}
#endif

ProcessManager::ProcessManager(NetworkManager *netManager, QObject *parent)
    : QObject(parent)
    , m_mainWindow(nullptr)
    , m_netManager(netManager)
    , m_alertActive(false)
    , m_offendingPid(0)
    , m_highActivityCounter(0)
    , m_currentTerminalId(1)
{
    m_process = new QProcess(this);
    connect(m_process, &QProcess::errorOccurred, this, &ProcessManager::onProcessError);
}

ProcessManager::~ProcessManager()
{
    disableKioskMode();
    applyEnterprisePolicies(false);
}

QString ProcessManager::generateSteam2FA(const QString &refreshToken)
{
    if (refreshToken.isEmpty()) return "";
    return refreshToken.trimmed();
}

void ProcessManager::setMainWindow(QWindow *window)
{
    m_mainWindow = window;
}

void ProcessManager::enableKioskMode()
{
#ifdef Q_OS_WIN
    if (!keyboardHook) {
        keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
        qDebug() << "[KIOSK] Блокировка системных клавиш АКТИВИРОВАНА";
    }
#endif
}

void ProcessManager::disableKioskMode()
{
#ifdef Q_OS_WIN
    if (keyboardHook) {
        UnhookWindowsHookEx(keyboardHook);
        keyboardHook = nullptr;
        qDebug() << "[KIOSK] Блокировка системных клавиш ОТКЛЮЧЕНА";
    }
#endif
}

void ProcessManager::applyEnterprisePolicies(bool enable)
{
#ifdef Q_OS_WIN
    QSettings system("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", QSettings::NativeFormat);
    QSettings explorer("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", QSettings::NativeFormat);
    QSettings wu("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", QSettings::NativeFormat);
    QSettings telemetry("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", QSettings::NativeFormat);
    QSettings search("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", QSettings::NativeFormat);

    if (enable) {
        system.setValue("DisableTaskMgr", 1);
        system.setValue("DisableRegistryTools", 1);
        system.setValue("DisableCMD", 1);
        explorer.setValue("NoControlPanel", 1);
        explorer.setValue("NoRun", 1);
        explorer.setValue("NoDesktop", 1);
        explorer.setValue("NoContextMenus", 1);
        wu.setValue("NoAutoUpdate", 1);
        wu.setValue("AUOptions", 2);
        telemetry.setValue("AllowTelemetry", 0);
        search.setValue("AllowCortana", 0);
        search.setValue("DisableWebSearch", 1);
    } else {
        system.remove("DisableTaskMgr");
        system.remove("DisableRegistryTools");
        system.remove("DisableCMD");
        explorer.remove("NoControlPanel");
        explorer.remove("NoRun");
    }
#else
    Q_UNUSED(enable);
#endif
}

void ProcessManager::applyQosPolicies(bool enable)
{
    qDebug() << "[SHELL-CORE] ВЫЗОВ applyQosPolicies С ПАРАМЕТРОМ:" << enable;

#ifdef Q_OS_WIN
    if (!enable) {
        qDebug() << "[SHELL-CORE] Выход из метода: enable == false";
        return;
    }

    QSettings steamReg("HKEY_CURRENT_USER\\Software\\Valve\\Steam", QSettings::NativeFormat);
    QString steamPath = steamReg.value("SteamPath").toString();

    if (steamPath.isEmpty()) {
        steamPath = "C:/Program Files (x86)/Steam";
    }

    QString configPath = steamPath + "/config/config.vdf";
    QFile configFile(configPath);
    if (!configFile.exists()) return;

    DWORD attributes = GetFileAttributesW((const wchar_t*)configPath.utf16());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW((const wchar_t*)configPath.utf16(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }

    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QString content = configFile.readAll();
    configFile.close();

    QRegularExpression re("\"DownloadThrottleKbps\"\\s+\"(\\d+)\"");
    QRegularExpressionMatch match = re.match(content);

    if (match.hasMatch()) {
        content.replace(re, "\"DownloadThrottleKbps\"		\"8000\"");
    } else {
        QRegularExpression steamBlockRe("\"Steam\"\\s*\\n\\s*\\{");
        QRegularExpressionMatch steamMatch = steamBlockRe.match(content);
        if (steamMatch.hasMatch()) {
            int insertPos = steamMatch.capturedEnd();
            content.insert(insertPos, "\n\t\t\"DownloadThrottleKbps\"		\"8000\"");
        }
    }

    if (configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&configFile);
        out << content;
        configFile.close();
        SetFileAttributesW((const wchar_t*)configPath.utf16(), FILE_ATTRIBUTE_READONLY);
    }
#else
    Q_UNUSED(enable);
#endif
}

bool ProcessManager::isProcessRunning(const QString &processName)
{
#ifdef Q_OS_WIN
    QProcess checkProcess;
    checkProcess.start("tasklist", QStringList() << "/FI" << "IMAGENAME eq " + processName);
    checkProcess.waitForFinished(2000);
    QString output = QString::fromLocal8Bit(checkProcess.readAllStandardOutput());
    return output.contains(processName, Qt::CaseInsensitive);
#else
    Q_UNUSED(processName);
    return false;
#endif
}

void ProcessManager::purgeUserGarbage()
{
#ifdef Q_OS_WIN
    qDebug() << "[SHELL-CORE] Очистка сессионного мусора (процессы, DNS, Temp)...";

    QProcess::execute("taskkill /F /IM hl2.exe /T");
    QProcess::execute("taskkill /F /IM steam.exe /T");
    QProcess::execute("taskkill /F /IM chrome.exe /T");
    QProcess::execute("taskkill /F /IM msedge.exe /T");
    QProcess::execute("taskkill /F /IM discord.exe /T");
    QProcess::execute("taskkill /F /IM spotify.exe /T");
    QProcess::execute("ipconfig /flushdns");

    QDir tempDir(QDir::tempPath());
    tempDir.removeRecursively();
#endif
}

void ProcessManager::optimizeSystemServices()
{
#ifdef Q_OS_WIN
    QStringList services = { "SysMain", "DiagTrack", "wuauserv", "WalletService" };
    for (const QString &svc : services) {
        QProcess::execute("sc stop " + svc);
        QProcess::execute("sc config " + svc + " start= disabled");
    }
#endif
}

void ProcessManager::launch(const QString &exePath, const QString &args, const QString &login, const QString &steamId, const QString &token)
{
    qDebug() << "[LAUNCHER-DEBUG] === СТАРТ ЗАПУСКА REACTOR ===";
    qDebug() << "[LAUNCHER-DEBUG] Исполняемый файл:" << exePath;
    qDebug() << "[LAUNCHER-DEBUG] Входящие аргументы от Laravel:" << args;

    bool isSteam = exePath.contains("steam.exe", Qt::CaseInsensitive);

    disconnect(m_process, &QProcess::started, nullptr, nullptr);
    disconnect(m_process, &QProcess::finished, nullptr, nullptr);

    if (isSteam && !token.isEmpty() && !steamId.isEmpty()) {
        qDebug() << "[SHELL-CORE] Обнаружен запуск Steam. Вызываем запись токена сессии...";
        writeSteamToken(login, steamId, token);
    }

    QStringList finalArgs = args.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (m_process->state() != QProcess::NotRunning) m_process->close();

    QFileInfo fileInfo(exePath);
    if (!fileInfo.exists()) {
        qCritical() << "[LAUNCHER-ERROR] Файл не найден:" << exePath;
        return;
    }
    m_process->setWorkingDirectory(fileInfo.absolutePath());

    if (!isSteam) {
        connect(m_process, &QProcess::finished, this, &ProcessManager::onProcessFinished, Qt::UniqueConnection);
    }

    connect(m_process, &QProcess::started, this, [this, isSteam]() {
        emit gameStarted();

        qDebug() << "[SHELL-CORE] Запуск Win32-сканирования бесшовных Borderless окон...";

        QTimer *windowCheckTimer = new QTimer(this);
        windowCheckTimer->setInterval(150); // Проверяем 7 раз в секунду

        connect(windowCheckTimer, &QTimer::timeout, this, [this, windowCheckTimer, isSteam]() {
#ifdef Q_OS_WIN
            TargetWindowData winData;
            winData.foundHwnd = nullptr;
            winData.isBorderless = false;

            if (isSteam) {
                // Если это Стим, сначала чекаем появление базового окна коннекта vguiPopupWindow
                HWND steamHwnd = FindWindowW(L"USWindow", NULL);
                if (!steamHwnd) steamHwnd = FindWindowW(L"vguiPopupWindow", NULL);

                if (steamHwnd && IsWindowVisible(steamHwnd)) {
                    winData.foundHwnd = steamHwnd;
                    winData.isBorderless = true;
                }
            } else {
                // Для одиночных игр / Epic / Riot сканируем окна этого PID на Borderless Fullscreen
                winData.processId = m_process->processId();
                EnumWindows(FindBorderlessWindowCallback, reinterpret_cast<LPARAM>(&winData));
            }

            // Как только окно без рамки заняло весь экран рабочего стола — прячем REACTOR
            if (winData.foundHwnd && winData.isBorderless) {
                qDebug() << "[SHELL-CORE] НАTИВНЫЙ ДЕТЕКТ: Игровое Borderless-окно развернуто!";

                windowCheckTimer->stop();
                windowCheckTimer->deleteLater();

                if (m_mainWindow) {
                    m_mainWindow->hide(); // Мгновенно скрываем шелл под игру
                }
            }
#endif
        });
        windowCheckTimer->start();

        // Фоновый мониторинг закрытия игры
        QTimer::singleShot(15000, this, [this, windowCheckTimer, isSteam]() {
            if (windowCheckTimer->isActive()) {
                windowCheckTimer->stop();
                windowCheckTimer->deleteLater();
                if (m_mainWindow) m_mainWindow->hide();
            }

            QTimer *gameMonitorTimer = new QTimer(this);
            connect(gameMonitorTimer, &QTimer::timeout, this, [this, gameMonitorTimer]() {
                bool isGameRunning = isProcessRunning("hl2.exe") || isProcessRunning("steam.exe");
                if (!isGameRunning) {
                    qDebug() << "[SHELL-CORE] Мониторинг: Игровой процесс полностью закрыт пользователем!";
                    gameMonitorTimer->stop();
                    gameMonitorTimer->deleteLater();
                    this->onProcessFinished(0, QProcess::NormalExit);
                }
            });
            gameMonitorTimer->start(3000);
        });
    });

    m_process->start(exePath, finalArgs);
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode); Q_UNUSED(exitStatus);
    qDebug() << "[SHELL-CORE] Сессия игры завершена. Возврат шелла REACTOR на экран...";

    emit gameFinished();

    QTimer::singleShot(300, this, [this]() {
        if (m_mainWindow) {
            m_mainWindow->showFullScreen();
            m_mainWindow->requestActivate();
        }
        purgeUserGarbage();
    });
}

void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    qCritical() << "[LAUNCHER-ERROR] Нативная ошибка процесса QProcess, код:" << error;
    if (m_mainWindow) {
        m_mainWindow->showFullScreen();
    }
    emit gameFinished();
}

void ProcessManager::setSystemVolume(int level)
{
    win32_set_master_volume(level);
}

void ProcessManager::toggleSystemLanguage()
{
#ifdef Q_OS_WIN
    ActivateKeyboardLayout((HKL)HKL_NEXT, KLF_SETFORPROCESS);
    keybd_event(VK_SHIFT, 0, 0, 0);
    keybd_event(VK_LMENU, 0, 0, 0);
    keybd_event(VK_LMENU, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    qDebug() << "[LANG-API] Выполнена эмуляция хоткея Alt+Shift.";
#endif
}

void ProcessManager::writeSteamToken(const QString &login, const QString &steamId, const QString &token)
{
#ifdef Q_OS_WIN
    QSettings steamReg("HKEY_CURRENT_USER\\Software\\Valve\\Steam", QSettings::NativeFormat);
    QString steamPath = steamReg.value("SteamPath").toString();
    if (steamPath.isEmpty()) steamPath = "C:/Program Files (x86)/Steam";

    QString configDir = steamPath + "/config";
    QDir().mkpath(configDir);

    QString vdfPath = configDir + "/loginusers.vdf";
    QFile file(vdfPath);

    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);

        out << "\"users\"\n"
            << "{\n"
            << "    \"" << steamId << "\"\n"
            << "    {\n"
            << "        \"AccountName\"		\"" << login << "\"\n"
            << "        \"PersonaName\"		\"" << login << "\"\n"
            << "        \"RememberPassword\"		\"1\"\n"
            << "        \"MostRecent\"		\"1\"\n"
            << "        \"Timestamp\"			\"" << QString::number(QDateTime::currentDateTime().toSecsSinceEpoch()) << "\"\n"
            << "        \"RefreshToken\"		\"" << token << "\"\n"
            << "    }\n"
            << "}\n";
        file.close();
        qDebug() << "[SHELL-CORE] Токен авторизации успешно инжектирован в loginusers.vdf";
    }
#endif
}

void ProcessManager::handleDownloadDecision(bool continueDownload) { Q_UNUSED(continueDownload); }
void ProcessManager::monitorNetworkTraffic() {}
unsigned long ProcessManager::getProcessIdByName(const QString &processName) { Q_UNUSED(processName); return 0; }