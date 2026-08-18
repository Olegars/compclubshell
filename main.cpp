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
#include <QLibrary>
#include <QStringList>
#include <QTimer>
#include <QtWebView/QtWebView>
#include <cstdio>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// Инфраструктура ядра REACTOR
#include "src/core/hwidprovider.h"
#include "src/core/pathresolver.h"
#include "src/core/networkmanager.h"
#include "src/core/sessionalertmanager.h"
#include "src/core/securitymanager.h"
#include "src/core/processmanager.h"
#include "src/core/hidinputmonitor.h"
#include "src/core/voiceassistant.h"
#include "src/core/lobbyaudiomanager.h"
#include "src/core/ccbootsuperclient.h"

// C++ Модели данных для QML слоя
#include "src/models/gamemodel.h"
#include "src/models/storemodel.h"

namespace {

QString g_debugLogPath = QStringLiteral("C:/ShellVideo/logs/shell-debug.log");

bool isNoisyLogLine(QtMsgType type, const QString &msg)
{
    // FFmpeg / multimedia / NVIDIA ShadowPlay
    if (msg.contains(QLatin1String("Input #0"))
        || msg.contains(QLatin1String("Metadata:"))
        || msg.contains(QLatin1String("Duration:"))
        || msg.contains(QLatin1String("Stream #0"))
        || msg.contains(QLatin1String("compatible_brands"))
        || msg.contains(QLatin1String("handler_name"))
        || msg.contains(QLatin1String("LvMetaInfo"))
        || msg.contains(QLatin1String("Using Qt multimedia"))
        || msg.contains(QLatin1String("Qt Multimedia requires"))
        || msg.contains(QLatin1String("qt.multimedia"))
        || msg.contains(QLatin1String("QMediaPlayer"))
        || msg.contains(QLatin1String("QWaitCondition: Destroyed while threads"))
        || msg.contains(QLatin1String("QObject::connect(QObject, Unknown): invalid nullptr"))
        || msg.contains(QLatin1String("ShadowPlay"))
        || msg.contains(QLatin1String("CLogger::Create"))
        || msg.contains(QLatin1String("CaptureCore.log"))) {
        return true;
    }

    // Периодический / ожидаемый шум оболочки
    if (msg.contains(QLatin1String("[THERMAL]"))
        || msg.contains(QLatin1String("[CLIMATE]"))
        || msg.contains(QLatin1String("[AUDIO] devices:"))
        || msg.contains(QLatin1String("[AUDIO] WARN: only HDMI"))
        || msg.contains(QLatin1String("[AUDIO] speakers-scan"))
        || msg.contains(QLatin1String("[AUDIO] speakers device:"))
        || msg.contains(QLatin1String("[AUDIO] headphones guard"))
        || msg.contains(QLatin1String("[AUDIO] PolicyConfig"))
        || msg.contains(QLatin1String("[AUDIO] failed to force"))
        || msg.contains(QLatin1String("[AUDIO] preferred headphones"))
        || msg.contains(QLatin1String("[STORAGE] persistent SSD"))
        || msg.contains(QLatin1String("[LOBBY] music start"))
        || msg.contains(QLatin1String("[LOBBY] routed"))
        || msg.contains(QLatin1String("[LOBBY] music faded"))
        || msg.contains(QLatin1String("[LOBBY] enabled="))
        || msg.contains(QLatin1String("[LOBBY] greeting failed"))
        || msg.contains(QLatin1String("[VOICE] config"))
        || msg.contains(QLatin1String("[VOICE] hotkey"))
        || msg.contains(QLatin1String("[VOICE] sessionActive"))
        || msg.contains(QLatin1String("[VOICE-NET] voice-greeting HTTP 422"))
        || msg.contains(QLatin1String("[SESSION-ALERT]"))
        || msg.contains(QLatin1String("[POWER] idle + desired"))
        || msg.contains(QLatin1String("[POWER] heartbeat started"))
        || msg.contains(QLatin1String("[POWER] logout"))
        || msg.contains(QLatin1String("[POWER] notifyPowerOffline"))
        || msg.contains(QLatin1String("[POWER] offline ack:"))
        || msg.contains(QLatin1String("applyPowerAction:"))
        || msg.contains(QLatin1String("[OVERLAYS]"))
        || msg.contains(QLatin1String("[PLAYER-OPTIMIZED]"))
        || msg.contains(QLatin1String("[SHOP]"))
        || msg.contains(QLatin1String("[HID]"))
        || msg.contains(QLatin1String("[START-TRACE]"))
        || msg.contains(QLatin1String("[DEBUG-MAIN]"))
        || msg.contains(QLatin1String("[PAY] diag"))
        || msg.contains(QLatin1String("[PAY] loadingChanged"))
        || msg.contains(QLatin1String("[QUICK] launched:"))
        || msg.contains(QLatin1String("QProcess: Destroyed while process"))) {
        return true;
    }

    // Happy-path SESSION / STEAM: оставляем только WARN / ошибки / старт-стоп сессии.
    if (msg.contains(QLatin1String("[SESSION]"))
        || msg.contains(QLatin1String("[STEAM]"))) {
        const bool keep = msg.contains(QLatin1String("WARN:"))
            || msg.contains(QLatin1String("FAIL"))
            || msg.contains(QLatin1String("fail"))
            || msg.contains(QLatin1String("error"), Qt::CaseInsensitive)
            || msg.contains(QLatin1String("timeout"), Qt::CaseInsensitive)
            || msg.contains(QLatin1String("ignored"))
            || msg.contains(QLatin1String("ending session"))
            || msg.contains(QLatin1String("game gone on"))
            || msg.contains(QLatin1String("Сессия завершена"))
            || msg.contains(QLatin1String("Игра запущена"))
            || msg.contains(QLatin1String("launch:"))
            || msg.contains(QLatin1String("Нет machine-cache"))
            || msg.contains(QLatin1String("не найден"))
            || msg.contains(QLatin1String("cache miss"))
            || msg.contains(QLatin1String("Интерактивный логин"))
            || msg.contains(QLatin1String("без cache"));
        if (!keep)
            return true;
    }

    // DBG в консоли слишком шумный (оверлеи, NET, PAY…)
    if (type == QtDebugMsg)
        return true;

    return false;
}

void reactorMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
    if (isNoisyLogLine(type, msg))
        return;

    static const char *levels[] = { "DBG", "WRN", "CRT", "FTL", "INF" };
    const char *level = levels[type <= QtInfoMsg ? int(type) : 0];

    const QString line = QStringLiteral("%1 [%2] %3")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                 QString::fromLatin1(level), msg);

    fprintf(stderr, "%s\n", qPrintable(line));
    fflush(stderr);

    QDir().mkpath(QFileInfo(g_debugLogPath).absolutePath());
    QFile f(g_debugLogPath);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << line << '\n';
    }
}

/** FFmpeg пишет Input #0 / Stream #0 напрямую в stderr — глушим до ERROR. */
void quietFfmpegNativeLog()
{
    using AvLogSetLevelFn = void (*)(int);
    const QStringList names = {
        QStringLiteral("avutil-61"),
        QStringLiteral("avutil-60"),
        QStringLiteral("avutil-59"),
        QStringLiteral("avutil-58"),
        QStringLiteral("avutil"),
    };
    for (const QString &name : names) {
        QLibrary lib(name);
        if (!lib.load())
            continue;
        auto fn = reinterpret_cast<AvLogSetLevelFn>(lib.resolve("av_log_set_level"));
        if (!fn)
            continue;
        fn(16); // AV_LOG_ERROR
        return;
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qInstallMessageHandler(reactorMessageHandler);
    // ИСПРАВЛЕНО: Снимаем блокировку Qt 6 на чтение локальных ресурсов через XMLHttpRequest
    qputenv("QML_XHR_ALLOW_FILE_READ", "1");
#ifdef Q_OS_WIN
    // FFmpeg backend часто синхронно блокирует UI на MediaPlayer.setSource.
    // Windows Media Foundation обычно открывает локальные mp4 без такого хитча.
    if (qgetenv("QT_MEDIA_BACKEND").isEmpty())
        qputenv("QT_MEDIA_BACKEND", "windows");
#endif

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
        const QString origin = NetworkManager::buildServerUrl(apiIp, apiPort);

        QString extraArgs = qEnvironmentVariable("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS");
        const auto appendArg = [&extraArgs](const QString &flag, const QString &marker) {
            if (extraArgs.contains(marker))
                return;
            if (!extraArgs.isEmpty())
                extraArgs += QLatin1Char(' ');
            extraArgs += flag;
        };

        // 1) Секьюр-контекст для http-origin бэкенда (иначе форма ЮKassa не рисуется).
        //    Для https origin уже доверенный, и флаг только мешал бы.
        if (origin.startsWith(QLatin1String("http://"))) {
            appendArg("--unsafely-treat-insecure-origin-as-secure=" + origin,
                      QStringLiteral("unsafely-treat-insecure-origin-as-secure"));
        }
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

    PathResolver *pathResolver = new PathResolver(&app);
    g_debugLogPath = pathResolver->debugLogPath();
    QDir().mkpath(QFileInfo(g_debugLogPath).absolutePath());

    QQuickStyle::setStyle("Basic");
    // avutil подгружается вместе с Qt Multimedia — глушим после старта и ещё раз чуть позже.
    quietFfmpegNativeLog();
    QTimer::singleShot(1500, &app, []() { quietFfmpegNativeLog(); });

    SecurityManager *securityManager = new SecurityManager(&app);
    CcbootSuperClient *ccbootSuper = new CcbootSuperClient(securityManager, &app);

    QString secIni = PathResolver::findConfigIni();
    QSettings secSettings(secIni, QSettings::IniFormat);
    const QString prodRaw = secSettings.value(QStringLiteral("Security/production")).toString().trimmed().toLower();
    const bool isProduction = (prodRaw == QLatin1String("1")
                               || prodRaw == QLatin1String("true")
                               || prodRaw == QLatin1String("yes"));

    if (ccbootSuper->superClientActive()) {
        qDebug() << "[REACTOR-MAIN] Super Client активен — киоск не включаем, explorer для правки образа.";
        securityManager->unlockSystem();
    } else if (isProduction) {
        qDebug() << "[REACTOR-MAIN] Запуск в режиме PRODUCTION. Инициализация SecurityManager...";
        securityManager->lockDownSystem();
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
    VoiceAssistant *voiceAssistant = new VoiceAssistant(
        networkManager, processManager, sessionAlertManager, &app);
    LobbyAudioManager *lobbyAudio = new LobbyAudioManager(
        networkManager, sessionAlertManager, &app);

    networkManager->fetchTerminalConfig(HwidProvider::machineHwid());
    if (ccbootSuper->superClientActive())
        networkManager->setMaintenance(true);
    networkManager->checkTerminalStatus();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, networkManager, [networkManager]() {
        networkManager->notifyPowerOffline();
    });

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
    rootContext->setContextProperty("VoiceAssistant", voiceAssistant);
    rootContext->setContextProperty("LobbyAudio", lobbyAudio);
    rootContext->setContextProperty("SecurityManager", securityManager);
    rootContext->setContextProperty("Ccboot", ccbootSuper);
    rootContext->setContextProperty("PathResolver", pathResolver);

    qDebug() << "[REACTOR-MAIN] Рабочая директория приложения:" << QDir::currentPath();
    qDebug() << "[REACTOR-MAIN] Поиск корневого интерфейса в QRC ресурсах...";

    // Загрузка главного файла интерфейса REACTOR SHELL
    const QUrl url(QStringLiteral("qrc:/qt/qml/sector0451/Main.qml"));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url, processManager, networkManager, ccbootSuper](QObject *obj, const QUrl &objUrl) {
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
                    ccbootSuper->setMainWindow(mainWindow);
                    qDebug() << "[REACTOR-MAIN] Поток QWindow успешно передан в ProcessManager.";
                }
            }
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}