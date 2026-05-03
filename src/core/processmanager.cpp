#include "processmanager.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
HHOOK ProcessManager::keyboardHook = nullptr;

// Коллбэк для перехвата клавиш (Win, Alt+Tab, Ctrl+Esc и т.д.)
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
            return 1; // Поглощаем нажатие
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
    // При выходе из программы возвращаем права юзеру (на всякий случай)
    applyEnterprisePolicies(false);
}

void ProcessManager::setMainWindow(QWindow *window)
{
    m_mainWindow = window;
}

// ==========================================
// КИОСК-РЕЖИМ (КЛАВИАТУРА)
// ==========================================
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

// ==========================================
// ENTERPRISE POLICIES (РЕЕСТР LTSC)
// ==========================================
void ProcessManager::applyEnterprisePolicies(bool enable)
{
#ifdef Q_OS_WIN
    // 1. Политики системы (Диспетчер задач, CMD, Реестр)
    QSettings system("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", QSettings::NativeFormat);
    // 2. Ограничения Проводника (Панель управления, рабочий стол)
    QSettings explorer("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", QSettings::NativeFormat);
    // 3. Отключение обновлений (AU - Automatic Updates)
    QSettings wu("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", QSettings::NativeFormat);
    // 4. Телеметрия и Кортана
    QSettings telemetry("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", QSettings::NativeFormat);
    QSettings search("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", QSettings::NativeFormat);

    if (enable) {
        // Закручиваем гайки
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

        qDebug() << "[ENTERPRISE] Режим LTSC применен: Система максимально урезана.";
    } else {
        // Отпускаем для игры
        system.remove("DisableTaskMgr");
        system.remove("DisableRegistryTools");
        system.remove("DisableCMD");

        explorer.remove("NoControlPanel");
        explorer.remove("NoRun");

        qDebug() << "[ENTERPRISE] Политики временно ослаблены для запуска приложения.";
    }
#else
    Q_UNUSED(enable);
#endif
}

// ==========================================
// ОЧИСТКА И ОПТИМИЗАЦИЯ
// ==========================================
void ProcessManager::purgeUserGarbage()
{
#ifdef Q_OS_WIN
    qDebug() << "[PURGE] Начало глубокой очистки...";

    // Убиваем типичные хвосты
    QProcess::execute("taskkill /F /IM chrome.exe /T");
    QProcess::execute("taskkill /F /IM msedge.exe /T");
    QProcess::execute("taskkill /F /IM discord.exe /T");
    QProcess::execute("taskkill /F /IM spotify.exe /T");

    // Чистим DNS кеш
    QProcess::execute("ipconfig /flushdns");

    // Чистим Temp (опционально, может подтормаживать)
    QDir tempDir(QDir::tempPath());
    tempDir.removeRecursively();

    qDebug() << "[PURGE] Система очищена для следующего сеанса.";
#endif
}

// Остановка тяжелых служб (вызывать один раз при старте шелла)
void ProcessManager::optimizeSystemServices()
{
#ifdef Q_OS_WIN
    QStringList services = { "SysMain", "DiagTrack", "wuauserv", "WalletService" };
    for (const QString &svc : services) {
        QProcess::execute("sc stop " + svc);
        QProcess::execute("sc config " + svc + " start= disabled");
    }
    qDebug() << "[OPTIMIZE] Лишние службы Windows остановлены и отключены.";
#endif
}

// ==========================================
// УПРАВЛЕНИЕ ПРОЦЕССОМ ИГРЫ
// ==========================================
void ProcessManager::launch(const QString &exePath, const QString &args)
{
    if (m_process->state() == QProcess::Running) return;

    QFileInfo fileInfo(exePath);
    if (!fileInfo.exists()) {
        qDebug() << "[LAUNCHER] Ошибка: Файл не найден!" << exePath;
        return;
    }

    // 1. Ослабляем политики (чтобы античиты и лаунчеры игр не ругались)
    applyEnterprisePolicies(false);
    disableKioskMode();

    m_process->setWorkingDirectory(fileInfo.absolutePath());

    if (m_mainWindow) m_mainWindow->hide();

    qDebug() << "[LAUNCHER] Запуск игры:" << exePath;
    m_process->start(exePath, args.split(" ", Qt::SkipEmptyParts));
    emit gameStarted();
}

void ProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    qDebug() << "[LAUNCHER] Игра закрыта юзером.";

    // 2. После игры - зачистка и возврат в режим Enterprise
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
    qDebug() << "[LAUNCHER] Критическая ошибка процесса:" << error;
    applyEnterprisePolicies(true);
    enableKioskMode();
    if (m_mainWindow) m_mainWindow->showFullScreen();
}