#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "src/core/networkmanager.h"
#include "src/core/processmanager.h"
#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Безопасное чтение реестра Windows на низком уровне (только для чтения)
QString readRegistryString(HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName)
{
#ifdef Q_OS_WIN
    HKEY hKey;
    // Открываем ключ строго с правами KEY_READ (не требует прав администратора)
    if (RegOpenKeyExW(rootKey, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buffer[256];
        DWORD bufferSize = sizeof(buffer);
        DWORD type = REG_SZ;

        if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bufferSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return QString::fromWCharArray(buffer).trimmed();
        }
        RegCloseKey(hKey);
    }
#endif
    return "";
}

// Корректная функция получения HWID без wmic и QSettings
QString getMotherboardSerialNumber()
{
#ifdef Q_OS_WIN
    // 1. Пробуем забрать серийник материнки из BIOS
    QString serial = readRegistryString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardSerialNumber");

    if (!serial.isEmpty() && serial.toUpper() != "TO BE FILLED BY O.E.M." && serial.toUpper() != "DEFAULT STRING") {
        return serial;
    }

    // 2. Фоллбэк: если BIOS пустой (актуально для некоторых китайских плат), берем MachineGuid системы
    QString machineId = readRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
    if (!machineId.isEmpty()) {
        return machineId;
    }
#endif
    return "UNKNOWN_HWID_FALLBACK";
}

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    GameModel gamesModel;
    StoreModel storeModel;

    NetworkManager netManager(&gamesModel, &storeModel);

    engine.rootContext()->setContextProperty("NetworkManager", &netManager);
    engine.rootContext()->setContextProperty("GamesModel", &gamesModel);
    engine.rootContext()->setContextProperty("StoreModel", &storeModel);

    // Вытаскиваем железный HWID
    QString hwid = getMotherboardSerialNumber();

    netManager.fetchTerminalConfig(hwid);
    netManager.fetchGames();
    netManager.fetchProducts();

    const QUrl url(QStringLiteral("qrc:/qt/qml/sector0451/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}