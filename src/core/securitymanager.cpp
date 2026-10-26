#include "securitymanager.h"
#include <QDebug>
#include <QCoreApplication>
#include <QProcess>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
HHOOK g_kioskHook = nullptr;

LRESULT CALLBACK kioskKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const auto *key = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        const bool alt = (key->flags & LLKHF_ALTDOWN) != 0;
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const DWORD vk = key->vkCode;
        // Ctrl+Alt+Del обрабатывает winlogon раньше хука — его глушит DisableTaskMgr.
        if (vk == VK_LWIN || vk == VK_RWIN
            || (vk == VK_TAB && alt)
            || (vk == VK_ESCAPE && (alt || ctrl))
            || (vk == VK_F4 && alt)) {
            return 1;
        }
    }
    return CallNextHookEx(g_kioskHook, nCode, wParam, lParam);
}

void setDword(HKEY root, const wchar_t *subKey, const wchar_t *name, DWORD value)
{
    HKEY key = nullptr;
    const LONG opened = RegCreateKeyExW(root, subKey, 0, nullptr, 0,
                                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
}

void setString(HKEY root, const wchar_t *subKey, const wchar_t *name, const wchar_t *value)
{
    HKEY key = nullptr;
    const LONG opened = RegCreateKeyExW(root, subKey, 0, nullptr, 0,
                                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS)
        return;
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value), bytes);
    RegCloseKey(key);
}

void deleteValue(HKEY root, const wchar_t *subKey, const wchar_t *name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
}
#endif

} // namespace

SecurityManager::SecurityManager(QObject *parent) : QObject(parent) {}

SecurityManager::~SecurityManager() {}

// Вспомогательный метод для записи DWORD (32-бит инт) в реестр Windows
void SecurityManager::setRegistryValue(const QString &keyPath, const QString &valueName, uint32_t value)
{
#ifdef Q_OS_WIN
    const bool hkcu = keyPath.startsWith(QStringLiteral("HKEY_CURRENT_USER\\"), Qt::CaseInsensitive);
    const QString sub = keyPath.mid(hkcu ? 18 : 19); // strip hive prefix
    const std::wstring subKey = sub.toStdWString();
    const std::wstring name = valueName.toStdWString();
    const HKEY root = hkcu ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    setDword(root, subKey.c_str(), name.c_str(), value);
    // Диспетчер задач с экрана Ctrl+Alt+Del читает HKLM, не только HKCU.
    if (hkcu && (valueName == QLatin1String("DisableTaskMgr")
                 || valueName == QLatin1String("DisableLockWorkstation")
                 || valueName == QLatin1String("DisableChangePassword")
                 || valueName == QLatin1String("HideFastUserSwitching"))) {
        setDword(HKEY_LOCAL_MACHINE, subKey.c_str(), name.c_str(), value);
    }
    qWarning() << "[SECURITY-REG]" << keyPath << valueName << "=" << value;
#else
    Q_UNUSED(keyPath);
    Q_UNUSED(valueName);
    Q_UNUSED(value);
#endif
}

// Вспомогательный метод для записи STRING в реестр Windows
void SecurityManager::setRegistryString(const QString &keyPath, const QString &valueName, const QString &value)
{
#ifdef Q_OS_WIN
    const bool hkcu = keyPath.startsWith(QStringLiteral("HKEY_CURRENT_USER\\"), Qt::CaseInsensitive);
    const QString sub = keyPath.mid(hkcu ? 18 : 19);
    setString(hkcu ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE,
              sub.toStdWString().c_str(),
              valueName.toStdWString().c_str(),
              value.toStdWString().c_str());
    qWarning() << "[SECURITY-REG]" << keyPath << valueName << "=" << value;
#else
    Q_UNUSED(keyPath);
    Q_UNUSED(valueName);
    Q_UNUSED(value);
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
    setRegistryValue(expKey, "NoLogoff", 1);                // Убирает «Выйти» с экрана Ctrl+Alt+Del
    setRegistryValue(expKey, "NoControlPanel", 1);

#ifdef Q_OS_WIN
    const wchar_t *disallow = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\DisallowRun";
    setDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"DisallowRun", 1);
    const wchar_t *blocked[] = {
        L"taskmgr.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"regedit.exe", L"mmc.exe"
    };
    for (int i = 0; i < 6; ++i)
        setString(HKEY_CURRENT_USER, disallow, std::to_wstring(i + 1).c_str(), blocked[i]);
#endif
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
#ifdef Q_OS_WIN
    if (!g_kioskHook)
        g_kioskHook = SetWindowsHookExW(WH_KEYBOARD_LL, kioskKeyboardProc, GetModuleHandleW(nullptr), 0);
#endif

    if (!m_locked) {
        m_locked = true;
        emit lockedChanged();
    }
    qDebug() << "[SECURITY] === СИСТЕМА НАДЕМНО ЗАБЛОКИРОВАНА В РЕЖИМЕ КЛУБА ===";
}

// МЕТОД ДЛЯ АДМИНИСТРАТИВНОГО РАЗБЛОКИРОВАНИЯ (НАПРИМЕР, ДЛЯ ОБНОВЛЕНИЯ ОБРАЗА)
void SecurityManager::unlockSystem()
{
    qDebug() << "[SECURITY] === СНЯТИЕ ОГРАНИЧЕНИЙ БЕЗОПАСНОСТИ ===";

    QString sKey = "HKEY_CURRENT_USER\\Software\\Policies\\Microsoft\\Windows\\System";
    setRegistryValue(sKey, "DisableCMD", 0);

    QString rKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    setRegistryValue(rKey, "DisableRegistryTools", 0);
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
    setRegistryValue(expKey, "NoLogoff", 0);
    setRegistryValue(expKey, "NoControlPanel", 0);
    setRegistryValue(expKey, "DisallowRun", 0);

#ifdef Q_OS_WIN
    if (g_kioskHook) {
        UnhookWindowsHookEx(g_kioskHook);
        g_kioskHook = nullptr;
    }
    const wchar_t *disallow = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\DisallowRun";
    for (int i = 1; i <= 6; ++i)
        deleteValue(HKEY_CURRENT_USER, disallow, std::to_wstring(i).c_str());
#endif

    setupCustomShell(false); // Возвращаем explorer.exe
    startExplorer();

    if (m_locked) {
        m_locked = false;
        emit lockedChanged();
    }
}

void SecurityManager::startExplorer()
{
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer.exe"), {});
#endif
}