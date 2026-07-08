#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>
#include <QDir>
#include <QCoreApplication>

// Инфраструктура ядра REACTOR
#include "src/core/networkmanager.h"
#include "src/core/securitymanager.h"

// C++ Модели данных для QML слоя
#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"

int main(int argc, char *argv[])
{
    // Настройка стилей, как было в твоем оригинале
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QCoreApplication::setOrganizationName("REACTOR");
    QCoreApplication::setOrganizationDomain("reactor.club");
    QCoreApplication::setApplicationName("REACTOR SHELL");

    QGuiApplication app(argc, argv);

    // ==========================================================================
    // ГЛОБАЛЬНЫЙ ФЛАГ РЕЖИМА ПРОДАКШЕНА REACTOR
    // ==========================================================================
    bool isProduction = true;

    if (isProduction) {
        qDebug() << "[REACTOR-MAIN] Запуск в режиме PRODUCTION. Инициализация SecurityManager...";
        SecurityManager security;
        security.lockDownSystem();
    } else {
        qDebug() << "[REACTOR-MAIN] Запуск в режиме DEVELOPMENT. Процедуры безопасности Windows пропущены.";
    }
    // ==========================================================================

    QQmlApplicationEngine engine;

    // Инициализация C++ моделей данных
    GameModel gamesModel;
    StoreModel storeModel;

    // Инициализация менеджера сетевой инфраструктуры
    NetworkManager netManager(&gamesModel, &storeModel);

    // Регистрация C++ контекстных свойств напрямую в QML движок
    engine.rootContext()->setContextProperty("NetworkManager", &netManager);
    engine.rootContext()->setContextProperty("networkManager", &netManager);

    engine.rootContext()->setContextProperty("GamesModel", &gamesModel);
    engine.rootContext()->setContextProperty("gamesModel", &gamesModel);

    engine.rootContext()->setContextProperty("StoreModel", &storeModel);
    engine.rootContext()->setContextProperty("storeModel", &storeModel);

    // Дополнительная отладочная информация
    qDebug() << "[REACTOR-MAIN] Рабочая директория приложения:" << QDir::currentPath();
    qDebug() << "[REACTOR-MAIN] Поиск корневого интерфейса в QRC ресурсах...";

    // ЖЕЛЕЗОБЕТОННЫЙ СТАРЫЙ ПУТЬ QT 6 К РЕСУРСАМ МОДУЛЯ
    const QUrl url(QStringLiteral("qrc:/qt/qml/sector0451/Main.qml"));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl) {
            qCritical() << "[REACTOR-MAIN] КРИТИЧЕСКАЯ ОШИБКА: Не удалось загрузить Main.qml!";
            QCoreApplication::exit(-1);
        } else {
            qDebug() << "[REACTOR-MAIN] Движок QML успешно развернул Main.qml в памяти.";
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}