#include "securitymanager.h"
#include <QSettings>
#include <QDebug>
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

SecurityManager::SecurityManager(QObject *parent) : QObject(parent) {}

SecurityManager::~SecurityManager() {}

// Вспомогательный метод для записи DWORD (32-бит инт) в реестр Windows
void SecurityManager::setRegistryValue(const QString &keyPath, const QString &valueName, uint32_t value)
{
#ifdef Q_OS_WIN
    QSettings settings(keyPath, QSettings::Registry64Format);
    settings.setValue(valueName, value);
    qDebug() << "[SECURITY-REG] Записано:" << keyPath << "\\" << valueName << "=" << value;
#endif
}

// Вспомогательный метод для записи STRING в реестр Windows
void SecurityManager::setRegistryString(const QString &keyPath, const QString &valueName, const QString &value)
{
#ifdef Q_OS_WIN
    QSettings settings(keyPath, QSettings::Registry64Format);
    settings.setValue(valueName, value);
    qDebug() << "[SECURITY-REG] Записана строка:" << keyPath << "\\" << valueName << "=" << value;
#endif
}

// --- ТЕЛО ПРОЦЕДУР БЕЗОПАСНОСТИ REACTOR ---

void SecurityManager::disableCmdAndRegistry()
{
    // 1. Блокируем запуск командной строки cmd.exe и PowerShell scripts
    // Даже если юзер откроет диалог "Выбрать файл" и впишет cmd, Windows выкинет ошибку политики.
    QString sKey = "HKEY_CURRENT_USER\\Software\\Policies\\Microsoft\\Windows\\System";
    setRegistryValue(sKey, "DisableCMD", 1);

    // 2. Блокируем запуск редактора реестра regedit.exe, чтобы юзер не мог вернуть настройки назад.
    QString rKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    setRegistryValue(rKey, "DisableRegistryTools", 1);
}

void SecurityManager::disableTaskMgrAndCtrlAltDel()
{
    // Блокируем Диспетчер задач (Task Manager). Юзер не сможет убить процесс шелла REACTOR.
    QString sysKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    setRegistryValue(sysKey, "DisableTaskMgr", 1);

    // Дополнительно блокируем смену пароля и логаут из меню Ctrl+Alt+Del
    setRegistryValue(sysKey, "DisableChangePassword", 1);
    setRegistryValue(sysKey, "DisableLockWorkstation", 1);

    // ==========================================================================
    // ЖЕЛЕЗОБЕТОННАЯ БРОНЯ: Блокируем любые системные диалоги Explorer
    // ==========================================================================
    // Даже если ушлый юзер через Steam-браузер нажмет "Сохранить как...",
    // сама Windows запретит ему доступ к дискам и контекстным меню.
    QString expKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    setRegistryValue(expKey, "NoViewContextMenu", 1);       // Отключает правый клик мыши на рабочем столе и в проводнике
    setRegistryValue(expKey, "NoWindowsKey", 1);            // Намертво блокирует клавишу Win (Win+R, Win+E, Win+D больше не работают)
    setRegistryValue(expKey, "NoRun", 1);                   // Полностью выпиливает команду "Выполнить" (Win+R) из системы
    setRegistryValue(expKey, "NoDrives", 0x03FFFFFF);       // Прячет ВСЕ диски (от A до Z) в любых диалоговых окнах "Открыть/Сохранить"
    setRegistryValue(expKey, "NoFind", 1);                  // Отключает системный поиск Windows
}

void SecurityManager::disableStickyKeys()
{
    // Пятикратное нажатие SHIFT вызывает залипание клавиш, откуда можно выйти в справку Windows и проводник.
    // Записываем структуру настроек залипания клавиш в реестр, отключая хоткей.
    QString stickyKey = "HKEY_CURRENT_USER\\Control Panel\\Accessibility\\StickyKeys";
    setRegistryString(stickyKey, "Flags", "506"); // Флаг 506 полностью отключает триггер залипания
}

void SecurityManager::setupCustomShell(bool enable)
{
    // Заменяем стандартную оболочку Windows (explorer.exe с рабочим столом и пуском) на REACTOR SHELL.
    // Теперь при загрузке ПК вместо рабочего стола Windows будет инициализироваться строго твой шелл.
    QString shellKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";

    if (enable) {
        QString appPath = QCoreApplication::applicationFilePath();
        // Меняем слеши под стандарт Windows
        appPath = appPath.replace("/", "\\");
        setRegistryString(shellKey, "Shell", appPath);
    } else {
        // Возвращаем дефолтный проводник Windows
        setRegistryString(shellKey, "Shell", "explorer.exe");
    }
}

// ГЛАВНЫЙ ИСПОЛНИТЕЛЬНЫЙ МЕТОД ЛОКДАУНА СИСТЕМЫ
void SecurityManager::lockDownSystem()
{
    qDebug() << "[SECURITY] === ЗАПУСК ПРОЦЕДУР БЕЗОПАСНОСТИ REACTOR ===";

    disableCmdAndRegistry();
    disableTaskMgrAndCtrlAltDel();
    disableStickyKeys();
    setupCustomShell(true); // REACTOR становится главной оболочкой ОС Windows

    qDebug() << "[SECURITY] === СИСТЕМА НАДЕМНО ЗАБЛОКИРОВАНА В РЕЖИМЕ КЛУБА ===";
}

// МЕТОД ДЛЯ АДМИНИСТРАТИВНОГО РАЗБЛОКИРОВАНИЯ (НАПРИМЕР, ДЛЯ ОБНОВЛЕНИЯ ОБРАЗА)
void SecurityManager::unlockSystem()
{
    qDebug() << "[SECURITY] === СНЯТИЕ ОГРАНИЧЕНИЙ БЕЗОПАСНОСТИ ===";

    QString sKey = "HKEY_CURRENT_USER\\Software\\Policies\\Microsoft\\Windows\\System";
    setRegistryValue(sKey, "DisableCMD", 0);

    QString rKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    setRegistryValue(rKey, "DisableTaskMgr", 0);
    setRegistryValue(rKey, "DisableChangePassword", 0);
    setRegistryValue(rKey, "DisableLockWorkstation", 0);

    // Снимаем броню Explorer при разблокировке
    QString expKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    setRegistryValue(expKey, "NoViewContextMenu", 0);
    setRegistryValue(expKey, "NoWindowsKey", 0);
    setRegistryValue(expKey, "NoRun", 0);
    setRegistryValue(expKey, "NoDrives", 0);
    setRegistryValue(expKey, "NoFind", 0);

    setupCustomShell(false); // Возвращаем explorer.exe
}