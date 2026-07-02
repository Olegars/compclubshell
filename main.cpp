#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QWindow>
#include <QVariant>
#include <QDebug>
#include <QProcess>
#include <QRegularExpression>
#include <QDir>

#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"
#include "src/core/processmanager.h"
#include "src/core/networkmanager.h"

QString getMotherboardSerialNumber() {
#ifdef Q_OS_WIN
    QProcess process;
    process.start("wmic", QStringList() << "baseboard" << "get" << "serialnumber");
    if (!process.waitForFinished(2000)) return "UNKNOWN-HWID";

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    QStringList lines = output.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);

    if (lines.size() > 1) {
        QString serial = lines.at(1).trimmed();
        if (!serial.isEmpty() && serial.toUpper() != "TO BE FILLED BY O.E.M.") {
            return serial;
        }
    }

    process.start("wmic", QStringList() << "path" << "win32_computersystemproduct" << "get" << "uuid");
    if (process.waitForFinished(2000)) {
        output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        lines = output.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
        if (lines.size() > 1) return lines.at(1).trimmed();
    }
#endif
    return "LINUX-DEV-NODE";
}

int main(int argc, char *argv[])
{
    qDebug() << "[START-TRACE] [STEP 1] Вход в функцию main.cpp";

    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    QString cachePath = "C:/ShellVideo/Cache/";
    if (QDir().mkpath(cachePath)) {
        qDebug() << "[START-TRACE] [STEP 2] Папка кэша проверена/создана:" << cachePath;
    } else {
        qDebug() << "[START-TRACE] [STEP 2] КРИТИЧЕСКАЯ ОШИБКА: Не удалось создать папку кэша!";
    }

    app.setProperty("engine", QVariant::fromValue<QObject*>(&engine));

    GameModel gamesModel;
    StoreModel storeModel;
    ProcessManager launcher;
    NetworkManager netManager(&gamesModel, &storeModel);

    qDebug() << "[START-TRACE] [STEP 3] Инициализируем свойства контекста QML";
    engine.rootContext()->setContextProperty("gamesModel", &gamesModel);
    engine.rootContext()->setContextProperty("storeModel", &storeModel);
    engine.rootContext()->setContextProperty("Launcher", &launcher);
    engine.rootContext()->setContextProperty("NetManager", &netManager);

    qDebug() << "[START-TRACE] [STEP 4] Загружаем модуль Main.qml через engine";
    engine.loadFromModule("sector0451", "Main");

    if (engine.rootObjects().isEmpty()) {
        qDebug() << "[START-TRACE] КРИТИЧЕСКАЯ ОШИБКА: rootObjects пуст, QML не загрузился!";
        return -1;
    }

    qDebug() << "[START-TRACE] [STEP 5] QML успешно распарсен, настраиваем Kiosk-режим";
    QWindow *mainWindow = qobject_cast<QWindow*>(engine.rootObjects().first());
    if (mainWindow) {
        launcher.setMainWindow(mainWindow);
        launcher.enableKioskMode();
    }

    QString hwid = getMotherboardSerialNumber();
    qDebug() << "[START-TRACE] [STEP 6] Аппаратный HWID определен:" << hwid;
    netManager.fetchTerminalConfig(hwid);

    qDebug() << "[START-TRACE] [STEP 7] Запуск фоновых сетевых запросов к играм/товарам";
    netManager.fetchGames();
    netManager.fetchProducts();

    qDebug() << "[START-TRACE] [STEP 8] Переход к выполнению цикла событий app.exec()";
    return app.exec();
}