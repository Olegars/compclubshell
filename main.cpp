#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "src/core/networkmanager.h"
#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"

int main(int argc, char *argv[])
{
    // 1. РАЗБЛОКИРУЕМ XMLHttpRequest ДЛЯ ЛОКАЛЬНЫХ СЛОЕВ QML (qrc)
    // Устраняет ошибку: "Using GET on a local file is disabled by default"
    qputenv("QML_XHR_ALLOW_FILE_READ", "1");

    // 2. Переключаем тему на Basic для снятия ограничений кастомизации Windows
    // Устраняет варнинги "The current style does not support customization of this control"
    QQuickStyle::setStyle("Basic");

    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    // Инициализируем C++ модели данных лаунчера REACTOR
    GameModel *gamesModel = new GameModel(&app);
    StoreModel *storeModel = new StoreModel(&app);

    // Создаем единственный экземпляр менеджера сети
    NetworkManager *netManager = new NetworkManager(gamesModel, storeModel, &app);

    // Регистрируем объект под двумя именами для полной совместимости со всеми экранами лаунчера
    engine.rootContext()->setContextProperty("NetworkManager", netManager);
    engine.rootContext()->setContextProperty("NetManager", netManager);

    engine.rootContext()->setContextProperty("gamesModel", gamesModel);
    engine.rootContext()->setContextProperty("storeModel", storeModel);

    // Строгий путь автоматического генератора QML-модулей Qt6
    const QUrl url(QStringLiteral("qrc:/qt/qml/sector0451/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
        &app, [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        }, Qt::QueuedConnection);

    engine.load(url);

    // Находим созданный QML-объект окна и прокидываем ссылку на него
    if (!engine.rootObjects().isEmpty()) {
        QObject* rootWindow = engine.rootObjects().first();
        netManager->setRootQmlObject(rootWindow);

        qDebug() << "[REACTOR-SHELL] Модули ядра засинхронены. Контроль первичного опроса передан в QML.";

        // УБРАНО ДЛЯ ПРЕДОТВРАЩЕНИЯ ДУБЛИРОВАНИЯ: netManager->checkTerminalStatus();
        // Теперь опрос запускается строго из Component.onCompleted в Main.qml после фиксации HWID.
    }

    return app.exec();
}