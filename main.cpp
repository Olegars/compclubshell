#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>
#include <QDateTime>
#include <QTextStream>
#include <QCoreApplication>
#include <QWindow>
#include <QQuickStyle>
#include <QtWebView/QtWebView>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// Инфраструктура ядра REACTOR
#include "src/core/hwidprovider.h"
#include "src/core/networkmanager.h"
#include "src/core/sessionalertmanager.h"
#include "src/core/securitymanager.h"
#include "src/core/processmanager.h"
#include "src/core/hidinputmonitor.h"

// C++ Модели данных для QML слоя
#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"

namespace {

const char *kDebugLogPath = "C:/ShellVideo/shell-debug.log";

void reactorMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)

    static const char *levels[] = { "DBG", "WRN", "CRT", "FTL", "INF" };
    const char *level = levels[type <= QtInfoMsg ? int(type) : 0];

    const QString line = QStringLiteral("%1 [%2] %3")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                 QString::fromLatin1(level), msg);

    // Консоль / "Вывод приложения" в Qt Creator.
    fprintf(stderr, "%s\n", qPrintable(line));
    fflush(stderr);

    QDir().mkpath(QStringLiteral("C:/ShellVideo"));
    QFile f(QString::fromLatin1(kDebugLogPath));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << line << '\n';
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qInstallMessageHandler(reactorMessageHandler);
    // ИСПРАВЛЕНО: Снимаем блокировку Qt 6 на чтение локальных ресурсов через XMLHttpRequest
    qputenv("QML_XHR_ALLOW_FILE_READ", "1");

    // Настройка базовых атрибутов приложения перед инициализацией QGuiApplication
    QCoreApplication::setOrganizationName("REACTOR");
    QCoreApplication::setOrganizationDomain("reactor.club");
    QCoreApplication::setApplicationName("REACTOR SHELL");

    // Платёжная форма ЮKassa (iframe yoomoney.ru) рисуется только в "secure context".
    // Бэкенд отдаётся по http:// на LAN-IP, поэтому весь ancestor-chain iframe'а
    // считается небезопасным и форма остаётся пустой. Помечаем origin бэкенда как
    // доверенный для WebView2 ещё до создания его окружения (переменная читается
    // движком Chromium при инициализации).
    {
        const QString exeDir = QString::fromLocal8Bit(argv[0]);
        const QString baseDir = QFileInfo(exeDir).absolutePath();
        const QStringList candidates = {
            baseDir + "/config.ini",
            baseDir + "/../config.ini",
            baseDir + "/../../config.ini",
        };
        QString configPath = candidates.first();
        for (const QString &c : candidates) {
            if (QFile::exists(c)) { configPath = c; break; }
        }

        QSettings settings(configPath, QSettings::IniFormat);
        const QString apiIp = settings.value("Network/api_ip", "192.168.222.2").toString().trimmed();
        const QString apiPort = settings.value("Network/api_port", "22222").toString().trimmed();
        const QString origin = "http://" + apiIp + ":" + apiPort;

        QString extraArgs = qEnvironmentVariable("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS");
        const auto appendArg = [&extraArgs](const QString &flag, const QString &marker) {
            if (extraArgs.contains(marker))
                return;
            if (!extraArgs.isEmpty())
                extraArgs += QLatin1Char(' ');
            extraArgs += flag;
        };

        // 1) Секьюр-контекст для http-origin бэкенда (иначе форма ЮKassa не рисуется).
        appendArg("--unsafely-treat-insecure-origin-as-secure=" + origin,
                  QStringLiteral("unsafely-treat-insecure-origin-as-secure"));
        // 2) Виджет ЮKassa схлопывает форму, если Chromium считает окно скрытым
        //    (document.visibilityState=hidden). Для встроенного WebView2 это
        //    ложное срабатывание расчёта перекрытия окон — отключаем его.
        appendArg("--disable-features=CalculateNativeWinOcclusion",
                  QStringLiteral("CalculateNativeWinOcclusion"));

        qputenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", extraArgs.toUtf8());
#ifdef Q_OS_WIN
        // qputenv правит только копию окружения в CRT, а WebView2 читает переменную
        // через GetEnvironmentVariable — поэтому дублируем в блок окружения Win32.
        const BOOL setOk = ::SetEnvironmentVariableW(
                    L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                    reinterpret_cast<const wchar_t *>(extraArgs.utf16()));

        wchar_t readBack[1024] = {};
        const DWORD n = ::GetEnvironmentVariableW(
                    L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", readBack, 1024);

        qDebug() << "[PAY-INIT] config:" << configPath;
        qDebug() << "[PAY-INIT] origin:" << origin;
        qDebug() << "[PAY-INIT] SetEnvironmentVariableW ok:" << bool(setOk);
        qDebug() << "[PAY-INIT] env readback (" << n << "):"
                 << QString::fromWCharArray(readBack, int(n));
#else
        qDebug() << "[PAY-INIT] WebView2 args:" << extraArgs;
#endif
    }

    // Qt WebView (Edge WebView2 on Windows) — для виджета ЮKassa
    QtWebView::initialize();

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
    GameModel *featuredGamesModel = new GameModel(&app);
    StoreModel *storeModel = new StoreModel(&app);

    // Инициализация менеджеров ядра REACTOR
    NetworkManager *networkManager = new NetworkManager(gamesModel, storeModel, &app);
    networkManager->setFeaturedGamesModel(featuredGamesModel);
    ProcessManager *processManager = new ProcessManager(networkManager, &app);
    SessionAlertManager *sessionAlertManager = new SessionAlertManager(&app);
    HidInputMonitor *hidMonitor = new HidInputMonitor(networkManager, &app);

    networkManager->fetchTerminalConfig(HwidProvider::machineHwid());
    networkManager->checkTerminalStatus();

    // Регистрация C++ контекстных свойств напрямую в QML движок
    QQmlContext *rootContext = engine.rootContext();
    rootContext->setContextProperty("NetworkManager", networkManager);
    rootContext->setContextProperty("gamesModel", gamesModel);
    rootContext->setContextProperty("featuredGamesModel", featuredGamesModel);
    rootContext->setContextProperty("storeModel", storeModel);
    rootContext->setContextProperty("Launcher", processManager);
    rootContext->setContextProperty("launcher", processManager);
    rootContext->setContextProperty("SessionAlert", sessionAlertManager);
    rootContext->setContextProperty("HidMonitor", hidMonitor);

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