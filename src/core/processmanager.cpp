// Путь: src/core/processmanager.cpp

#ifdef Q_OS_WIN
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00
    #endif
    #include <winsock2.h>
    #include <windows.h>
#endif

#include "processmanager.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QProcess>
#include <QTimer>
#include <QRegularExpression>

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
#endif

ProcessManager::ProcessManager(QObject *parent)
    : QObject(parent), m_mainWindow(nullptr), m_alertActive(false), m_offendingPid(0), m_highActivityCounter(0)
{
    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, &ProcessManager::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &ProcessManager::onProcessError);
}

ProcessManager::~ProcessManager()
{
    disableKioskMode();
    applyEnterprisePolicies(false);
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
    // Этот лог СТРОГО должен напечататься в консоли при любом раскладе!
    qDebug() << "[SHELL-CORE] ВЫЗОВ applyQosPolicies С ПАРАМЕТРОМ:" << enable;

#ifdef Q_OS_WIN
    if (!enable) {
        qDebug() << "[SHELL-CORE] Выход из метода: enable == false";
        return;
    }

    // 1. Проверяем, что реестр вообще отдаёт нам путь
    QSettings steamReg("HKEY_CURRENT_USER\\Software\\Valve\\Steam", QSettings::NativeFormat);
    QString steamPath = steamReg.value("SteamPath").toString();
    qDebug() << "[STEAM-DEBUG] Путь из реестра HKCU:" << steamPath;

    if (steamPath.isEmpty()) {
        steamPath = "C:/Program Files (x86)/Steam";
        qDebug() << "[STEAM-DEBUG] Путь в реестре пуст. Используем дефолт:" << steamPath;
    }

    QString configPath = steamPath + "/config/config.vdf";
    qDebug() << "[STEAM-DEBUG] Итоговый целевой путь к файлу:" << configPath;

    QFile configFile(configPath);
    if (!configFile.exists()) {
        qDebug() << "[STEAM-DEBUG] КРИТИЧЕСКАЯ ОШИБКА: Файл физически НЕ СУЩЕСТВУЕТ по этому пути!";
        return;
    }

    // Снимаем Read-Only для редактирования
    DWORD attributes = GetFileAttributesW((const wchar_t*)configPath.utf16());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW((const wchar_t*)configPath.utf16(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }

    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "[STEAM-DEBUG] Не удалось открыть файл на чтение!";
        return;
    }
    QString content = configFile.readAll();
    configFile.close();

    QRegularExpression re("\"DownloadThrottleKbps\"\\s+\"(\\d+)\"");
    QRegularExpressionMatch match = re.match(content);

    if (match.hasMatch()) {
        qDebug() << "[STEAM-DEBUG] НАЙДЕНА СТРОКА ЛИМИТА:" << match.captured(0);
        content.replace(re, "\"DownloadThrottleKbps\"		\"8000\"");
    } else {
        qDebug() << "[STEAM-DEBUG] Параметр DownloadThrottleKbps отсутствует. Встраиваем в секцию Steam...";
        QRegularExpression steamBlockRe("\"Steam\"\\s*\\n\\s*\\{");
        QRegularExpressionMatch steamMatch = steamBlockRe.match(content);
        if (steamMatch.hasMatch()) {
            int insertPos = steamMatch.capturedEnd();
            content.insert(insertPos, "\n\t\t\"DownloadThrottleKbps\"		\"8000\"");
            qDebug() << "[STEAM-DEBUG] Успешно встроили строку в начало блока.";
        } else {
            qDebug() << "[STEAM-DEBUG] ОШИБКА: Не нашли даже секцию \"Steam\" внутри файла!";
        }
    }

    if (configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&configFile);
        out << content;
        configFile.close();

        SetFileAttributesW((const wchar_t*)configPath.utf16(), FILE_ATTRIBUTE_READONLY);
        qDebug() << "[STEAM-DEBUG] Настройки сохранены, файл залочен в Read-Only.";
    } else {
        qDebug() << "[STEAM-DEBUG] Не удалось открыть файл на запись!";
    }
#else
    qDebug() << "[STEAM-DEBUG] Код пропущен, так как сборка идет НЕ под Windows (Q_OS_WIN не определен)!";
    Q_UNUSED(enable);
#endif
}

void ProcessManager::purgeUserGarbage()
{
#ifdef Q_OS_WIN
    qDebug() << "[SHELL-CORE] Очистка сессионного мусора (процессы, DNS, Temp)...";
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

void ProcessManager::launch(const QString &exePath, const QString &args)
{
    if (m_process->state() == QProcess::Running) return;

    QFileInfo fileInfo(exePath);
    if (!fileInfo.exists()) return;

    m_process->setWorkingDirectory(fileInfo.absolutePath());
    if (m_mainWindow) m_mainWindow->hide();

    m_process->start(exePath, args.split(" ", Qt::SkipEmptyParts));
    emit gameStarted();
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode); Q_UNUSED(exitStatus);
    qDebug() << "[SHELL-CORE] Сессия лаунчера / игры завершена.";

    QTimer::singleShot(300, this, [this]() {
        if (m_mainWindow) {
            m_mainWindow->showFullScreen();
            m_mainWindow->requestActivate();
        }

        QTimer::singleShot(300, this, [this]() {
            purgeUserGarbage();
            emit gameFinished();
        });
    });
}

void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    if (m_mainWindow) m_mainWindow->showFullScreen();
}

void ProcessManager::setSystemVolume(int level)
{
#ifdef Q_OS_WIN
    for (int i = 0; i < 50; ++i) {
        keybd_event(VK_VOLUME_DOWN, 0, 0, 0);
        keybd_event(VK_VOLUME_DOWN, 0, KEYEVENTF_KEYUP, 0);
    }
    int steps = level / 2;
    for (int i = 0; i < steps; ++i) {
        keybd_event(VK_VOLUME_UP, 0, 0, 0);
        keybd_event(VK_VOLUME_UP, 0, KEYEVENTF_KEYUP, 0);
    }
#else
    Q_UNUSED(level);
#endif
}

void ProcessManager::toggleSystemLanguage()
{
#ifdef Q_OS_WIN
    HWND foregroundWnd = GetForegroundWindow();
    if (foregroundWnd) {
        PostMessage(foregroundWnd, WM_INPUTLANGCHANGEREQUEST, INPUTLANGCHANGE_FORWARD, 0);
    }
#endif
}

void ProcessManager::handleDownloadDecision(bool continueDownload) { Q_UNUSED(continueDownload); }
void ProcessManager::monitorNetworkTraffic() {}