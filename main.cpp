#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QWindow>
#include <QVariant>
#include <QDebug>
#include <QProcess>
#include <QRegularExpression>

#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"
#include "src/core/processmanager.h"
#include "src/core/networkmanager.h"

// Функция парсинга и получения уникального серийного номера материнской платы через WMIC
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

    // Запасной вариант: аппаратный UUID системы
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
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    // Регистрация инстанса движка для доступа из NetworkManager
    app.setProperty("engine", QVariant::fromValue<QObject*>(&engine));

    GameModel gamesModel;
    StoreModel storeModel;
    ProcessManager launcher;
    NetworkManager netManager(&gamesModel, &storeModel);

    engine.rootContext()->setContextProperty("gamesModel", &gamesModel);
    engine.rootContext()->setContextProperty("storeModel", &storeModel);
    engine.rootContext()->setContextProperty("Launcher", &launcher);
    engine.rootContext()->setContextProperty("NetManager", &netManager);

    engine.loadFromModule("sector0451", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    QWindow *mainWindow = qobject_cast<QWindow*>(engine.rootObjects().first());
    if (mainWindow) {
        launcher.setMainWindow(mainWindow);
        launcher.enableKioskMode();
    }

    // Считывание аппаратного ID серийного номера и отправка на Laravel
    QString hwid = getMotherboardSerialNumber();
    qDebug() << "[MAIN] Аппаратный HWID системы определен:" << hwid;
    netManager.fetchTerminalConfig(hwid);

    // Фоновая загрузка игровых и товарных сущностей
    netManager.fetchGames();
    netManager.fetchProducts();

    return app.exec();
}