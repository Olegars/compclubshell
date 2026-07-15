#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QWindow>
#include <QQuickStyle>

// Инфраструктура ядра REACTOR
#include "src/core/hwidprovider.h"
#include "src/core/networkmanager.h"
#include "src/core/securitymanager.h"
#include "src/core/processmanager.h"

// C++ Модели данных для QML слоя
#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"

int main(int argc, char *argv[])
{
    // ИСПРАВЛЕНО: Снимаем блокировку Qt 6 на чтение локальных ресурсов через XMLHttpRequest
    qputenv("QML_XHR_ALLOW_FILE_READ", "1");

    // Настройка базовых атрибутов приложения перед инициализацией QGuiApplication
    QCoreApplication::setOrganizationName("REACTOR");
    QCoreApplication::setOrganizationDomain("reactor.club");
    QCoreApplication::setApplicationName("REACTOR SHELL");

    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");

    // ГЛОБАЛЬНЫЙ ФЛАГ РЕЖИМА ПРОДАКШЕНА REACTOR (Для отладки окон поставьте false)
    bool isProduction = false;

    if (isProduction) {
        qDebug() << "[REACTOR-MAIN] Запуск в режиме PRODUCTION. Инициализация SecurityManager...";
        SecurityManager security;
        security.lockDownSystem();
    } else {
        qDebug() << "[REACTOR-MAIN] Запуск в режиме DEVELOPMENT. Процедуры безопасности Windows пропущены.";
    }

    QQmlApplicationEngine engine;

    // Инициализация C++ моделей данных
    GameModel *gamesModel = new GameModel(&app);
    StoreModel *storeModel = new StoreModel(&app);

    // Инициализация менеджеров ядра REACTOR
    NetworkManager *networkManager = new NetworkManager(gamesModel, storeModel, &app);
    ProcessManager *processManager = new ProcessManager(networkManager, &app);

    networkManager->fetchTerminalConfig(HwidProvider::machineHwid());
    networkManager->checkTerminalStatus();

    // Регистрация C++ контекстных свойств напрямую в QML движок
    QQmlContext *rootContext = engine.rootContext();
    rootContext->setContextProperty("NetworkManager", networkManager);
    rootContext->setContextProperty("gamesModel", gamesModel);
    rootContext->setContextProperty("storeModel", storeModel);
    rootContext->setContextProperty("Launcher", processManager);
    rootContext->setContextProperty("launcher", processManager);

    qDebug() << "[REACTOR-MAIN] Рабочая директория приложения:" << QDir::currentPath();
    qDebug() << "[REACTOR-MAIN] Поиск корневого интерфейса в QRC ресурсах...";

    // Загрузка главного файла интерфейса REACTOR SHELL
    const QUrl url(QStringLiteral("qrc:/qt/qml/sector0451/Main.qml"));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url, processManager, networkManager](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl) {
            qCritical() << "[REACTOR-MAIN] КРИТИЧЕСКАЯ ОШИБКА: Не удалось загрузить Main.qml!";
            QCoreApplication::exit(-1);
        } else {
            qDebug() << "[REACTOR-MAIN] Движок QML успешно развернул Main.qml в памяти.";
            if (obj) {
                networkManager->setRootQmlObject(obj);

                QWindow *mainWindow = qobject_cast<QWindow*>(obj);
                if (mainWindow) {
                    processManager->setMainWindow(mainWindow);
                    qDebug() << "[REACTOR-MAIN] Поток QWindow успешно передан в ProcessManager.";
                }
            }
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}