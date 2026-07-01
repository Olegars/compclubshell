// Путь: src/core/processmanager.cpp
#include "processmanager.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
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

ProcessManager::ProcessManager(QObject *parent) : QObject(parent), m_mainWindow(nullptr)
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

void ProcessManager::purgeUserGarbage()
{
#ifdef Q_OS_WIN
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

    applyEnterprisePolicies(false);
    disableKioskMode();
    m_process->setWorkingDirectory(fileInfo.absolutePath());
    if (m_mainWindow) m_mainWindow->hide();
    m_process->start(exePath, args.split(" ", Qt::SkipEmptyParts));
    emit gameStarted();
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode); Q_UNUSED(exitStatus);
    applyEnterprisePolicies(true);
    purgeUserGarbage();
    if (m_mainWindow) {
        m_mainWindow->showFullScreen();
        m_mainWindow->requestActivate();
    }
    enableKioskMode();
    emit gameFinished();
}

void ProcessManager::onProcessError(QProcess::ProcessError error)
{
    applyEnterprisePolicies(true);
    enableKioskMode();
    if (m_mainWindow) m_mainWindow->showFullScreen();
}
// Добавить в самый конец файла src/core/processmanager.cpp

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
    qDebug() << "[WINAPI] Громкость изменена на:" << level << "%";
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
        qDebug() << "[WINAPI] Переключена раскладка клавиатуры";
    }
#endif
}