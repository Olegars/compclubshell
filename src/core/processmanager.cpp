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

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>

// Глобальные указатели для работы системного хука WinAPI
HWINEVENTHOOK g_pGameHook = nullptr;
ProcessManager* g_pProcessManagerInstance = nullptr;

// Системный колбэк: вызывается ядром Windows при смене фокуса окон
void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd) {
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;

            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            // Проверяем, что окно развернулось на весь экран
            if (width >= screenWidth && height >= screenHeight) {
                char className[256];
                GetClassNameA(hwnd, className, sizeof(className));
                QString clsStr(className);

                // Отсекаем интерфейс шелла, системные слои и само окно логина Steam
                if (clsStr != "Qt662Window" && clsStr != "Progman" && clsStr != "WorkerW" && clsStr != "SDL_app") {
                    qDebug() << "[REACTOR-WINAPI] Обнаружено полноэкранное окно игры (" << clsStr << "). Снимаем хук.";

                    UnhookWinEvent(hWinEventHook);
                    g_pGameHook = nullptr;

                    if (g_pProcessManagerInstance) {
                        emit g_pProcessManagerInstance->gameStartedSuccessfully();
                    }
                }
            }
        }
    }
}
#endif

// Вспомогательная функция для сохранения актуального кэша VDF на диск перед стартом
void saveVdfFiles(const QString &steamPath, const QJsonObject &vdfData) {
    QString configDir = steamPath + "/config";
    QDir().mkpath(configDir);

    auto saveFile = [](const QString &path, const QString &content) {
        if (content.isEmpty()) return;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << content;
            file.close();
        }
    };

    if (vdfData.contains("config_vdf")) saveFile(configDir + "/config.vdf", vdfData["config_vdf"].toString());
    if (vdfData.contains("loginusers_vdf")) saveFile(configDir + "/loginusers.vdf", vdfData["loginusers_vdf"].toString());
    if (vdfData.contains("local_vdf")) saveFile(configDir + "/local.vdf", vdfData["local_vdf"].toString());
}

ProcessManager::ProcessManager(NetworkManager *netManager, QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_mainWindow(nullptr)
    , m_netManager(netManager)
    , m_netWatchTimer(new QTimer(this))
    , m_alertActive(false)
    , m_offendingPid(0)
    , m_highActivityCounter(0)
    , m_currentTerminalId(1)
{
    connect(m_process, &QProcess::errorOccurred, this, &ProcessManager::onProcessError);
    connect(m_netWatchTimer, &QTimer::timeout, this, &ProcessManager::monitorNetworkTraffic);
    m_netWatchTimer->start(5000);
}

ProcessManager::~ProcessManager()
{
#ifdef Q_OS_WIN
    if (g_pGameHook) UnhookWinEvent(g_pGameHook);
#endif
}

void ProcessManager::setMainWindow(QWindow *window)
{
    m_mainWindow = window;
}

// УМНЫЙ АВТОНОМНЫЙ ЗАПУСК С УЧЕТОМ КЭША И WinAPI ИНЖЕКЦИИ
void ProcessManager::launchGameWithSmartAuth(const QJsonObject &authData, const QString &steamAppId)
{
    qDebug() << "[SMART-LAUNCH] Запуск игрового конвейера. AppID:" << steamAppId;
#ifdef Q_OS_WIN
    g_pProcessManagerInstance = this;

    QSettings settings("REACTOR", "REACTOR SHELL");
    QString steamPath = settings.value("Paths/steam_path", "C:/Program Files (x86)/Steam").toString();

    // 1. Загружаем VDF файлы авторизации, если бэкенд их передал
    if (authData.contains("vdf_files")) {
        saveVdfFiles(steamPath, authData["vdf_files"].toObject());
        qDebug() << "[SMART-LAUNCH] Свежие VDF-конфиги развернуты на диск.";
    }

    // 2. Жестко убиваем старый экземпляр, чтобы применить конфигурацию
    if (isProcessRunning("steam.exe")) {
        QProcess killProcess;
        killProcess.start("taskkill", QStringList() << "/F" << "/IM" << "steam.exe");
        killProcess.waitForFinished(3000);
        QThread::msleep(400);
    }

    // 3. Взводим WinAPI хук на отслеживание фокуса полноэкранной игры
    if (g_pGameHook) UnhookWinEvent(g_pGameHook);
    g_pGameHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
    );

    // 4. Запуск Steam с прямой инструкцией на открытие игры
    QProcess *steamProc = new QProcess(this);
    QString steamExe = steamPath + "/steam.exe";
    steamProc->setWorkingDirectory(steamPath);

    QStringList args;
    if (!steamAppId.isEmpty() && steamAppId != "0") {
        args << "-applaunch" << steamAppId;
    }
    steamProc->start(steamExe, args);
    emit gameStarted();

    // 5. Изолированный быстрый скаут на случай, если кэш протух и выскочит окно ввода
    QTimer *authScoutTimer = new QTimer(this);
    authScoutTimer->setInterval(150);

    QString login = authData["login"].toString();
    QString password = authData["password"].toString();

    connect(authScoutTimer, &QTimer::timeout, this, [authScoutTimer, login, password]() {
        // Если игра перехватила экран быстрее (кэш валиден), уничтожаем скаут авторизации
        if (!g_pGameHook) {
            authScoutTimer->stop();
            authScoutTimer->deleteLater();
            return;
        }

        HWND authHwnd = FindWindowA("SDL_app", NULL);
        if (authHwnd && IsWindowVisible(authHwnd)) {
            char windowTitle[256];
            GetWindowTextA(authHwnd, windowTitle, sizeof(windowTitle));
            QString titleStr = QString::fromUtf8(windowTitle);

            if (titleStr.contains("Steam") && (titleStr.contains("Вход") || titleStr.contains("Sign In") || titleStr.contains("Login"))) {
                qDebug() << "[SMART-LAUNCH] Кэш не подошел, обнаружено окно авторизации. Инжектируем...";

                authScoutTimer->stop();
                authScoutTimer->deleteLater();

                // Моментально депортируем окно за границы видимости монитора
                SetWindowPos(authHwnd, NULL, -2000, -2000, 705, 440, SWP_NOZORDER | SWP_NOACTIVATE);
                SetForegroundWindow(authHwnd);
                SetFocus(authHwnd);
                QThread::msleep(400);

                // Буферный симулятор ввода клавиш
                auto pasteText = [](const QString &text) {
                    if (OpenClipboard(NULL)) {
                        EmptyClipboard();
                        QByteArray bytes = text.toUtf8();
                        HGLOBAL hGlob = GlobalAlloc(GMEM_FIXED, bytes.size() + 1);
                        strcpy_s(reinterpret_cast<char*>(hGlob), bytes.size() + 1, bytes.constData());
                        SetClipboardData(CF_TEXT, hGlob);
                        CloseClipboard();
                        keybd_event(VK_CONTROL, 0, 0, 0); keybd_event('V', 0, 0, 0);
                        keybd_event('V', 0, KEYEVENTF_KEYUP, 0); keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
                        QThread::msleep(100);
                    }
                };

                pasteText(login);
                keybd_event(VK_TAB, 0, 0, 0); keybd_event(VK_TAB, 0, KEYEVENTF_KEYUP, 0); QThread::msleep(200);
                pasteText(password);
                keybd_event(VK_RETURN, 0, 0, 0); keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);

                // TODO: Добавить сюда асинхронный сбор свежих файлов VDF для отправки на Laravel
                ShowWindow(authHwnd, SW_HIDE);
            }
        }
    });

    authScoutTimer->start();
#endif
}

void ProcessManager::applyQosPolicies(bool enable)
{
    qDebug() << "[SHELL-CORE] Настройка сетевых политик приоритезации трафика (QoS):" << enable;
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

void ProcessManager::setSystemVolume(int level)
{
    qDebug() << "[SHELL-CORE] Изменение мастер-громкости системы на:" << level << "%";
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

void ProcessManager::toggleSystemLanguage()
{
#ifdef Q_OS_WIN
    HWND activeWnd = GetForegroundWindow();
    if (activeWnd) {
        PostMessageA(activeWnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)HKL_NEXT);
    }
#endif
}

void ProcessManager::handleDownloadDecision(bool continueDownload)
{
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

void ProcessManager::applyEnterprisePolicies(bool enable)
{
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

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    qDebug() << "[SHELL-CORE] Игровой процесс завершен. Код:" << exitCode;
    if (m_mainWindow) {
        m_mainWindow->show();
        m_mainWindow->raise();
        m_mainWindow->requestActivate();
    }
    emit gameFinished();
}

void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    qCritical() << "[SHELL-CORE] Ошибка QProcess:" << error;
    emit gameFinished();
}

bool ProcessManager::isProcessRunning(const QString &processName)
{
#ifdef Q_OS_WIN
    QProcess proc;
    proc.start("tasklist", QStringList() << "/FI" << QString("IMAGENAME eq %1").arg(processName));
    proc.waitForFinished(1000);
    return QString::fromUtf8(proc.readAllStandardOutput()).contains(processName, Qt::CaseInsensitive);
#else
    return false;
#endif
}

unsigned long ProcessManager::getProcessIdByName(const QString &processName)
{
#ifdef Q_OS_WIN
    QProcess proc;
    proc.start("powershell", QStringList() << "-Command" << QString("Get-Process -Name %1 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id").arg(processName.split('.').first()));
    proc.waitForFinished(1000);
    bool ok;
    unsigned long pid = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toULong(&ok);
    return ok ? pid : 0;
#else
    return 0;
#endif
}

void ProcessManager::monitorNetworkTraffic()
{
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