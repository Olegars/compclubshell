#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QWindow>

#include "src/models/gamemodel.h"
#include "src/models/storemodel.h" // Добавили инклуд магазина
#include "src/core/processmanager.h"
#include "src/core/networkmanager.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    // 1. Создаем ядро
    GameModel gamesModel;
    StoreModel storeModel; // Создаем объект магазина
    ProcessManager launcher;

    // Передаем ОБЕ модели в NetworkManager
    NetworkManager netManager(&gamesModel, &storeModel);

    // 2. Регистрируем в QML
    engine.rootContext()->setContextProperty("gamesModel", &gamesModel);
    engine.rootContext()->setContextProperty("storeModel", &storeModel); // Регистрируем магазин
    engine.rootContext()->setContextProperty("Launcher", &launcher);
    engine.rootContext()->setContextProperty("NetManager", &netManager);

    // 3. Загружаем интерфейс
    engine.loadFromModule("sector0451", "Main");

    // Если интерфейс не загрузился — выходим
    if (engine.rootObjects().isEmpty())
        return -1;

    // 4. Настраиваем ProcessManager и режим Киоска
    QWindow *mainWindow = qobject_cast<QWindow*>(engine.rootObjects().first());
    if (mainWindow) {
        launcher.setMainWindow(mainWindow);

        // Включаем жесткую блокировку системных клавиш Windows
        launcher.enableKioskMode();
    }

    // 5. Загружаем списки с бэкенда в фоне
    netManager.fetchGames();
    netManager.fetchProducts(); // Запускаем скачивание товаров!

    return app.exec();
}