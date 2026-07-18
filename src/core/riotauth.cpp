#include "riotauth.h"
#include "processmanager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSslError>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <UIAutomation.h>
#endif

#ifdef Q_OS_WIN
static QString processImageForPid(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return QString();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    QString name;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                name = QString::fromWCharArray(pe.szExeFile);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return name;
}

static QSet<DWORD> collectRiotPids()
{
    QSet<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile);
            if (name.contains(QStringLiteral("RiotClient"), Qt::CaseInsensitive)
                || name.compare(QStringLiteral("Riot Client.exe"), Qt::CaseInsensitive) == 0
                || name.contains(QStringLiteral("LeagueClient"), Qt::CaseInsensitive)) {
                pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static void sendVk(WORD vk)
{
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void sendReturnKey() { sendVk(VK_RETURN); }

static void sendTabs(int count, const char *why)
{
    qWarning() << "[RIOT] Tab x" << count << why;
    for (int i = 0; i < count; ++i) {
        sendVk(VK_TAB);
        Sleep(140);
    }
}

static void typeUnicode(const QString &text)
{
    for (const QChar ch : text) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = 0;
        in[0].ki.wScan = ch.unicode();
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(35);
    }
}

static void setShellTopmostFrom(QObject *auth, bool enabled)
{
    if (auto *pm = qobject_cast<ProcessManager *>(auth->parent()))
        pm->setShellTopmost(enabled);
}

struct RiotLoginEnumCtx {
    HWND best = nullptr;
    QString title;
    int score = -1;
    QSet<DWORD> riotPids;
    QStringList dump;
    bool doDump = false;
};

static bool isRiotLoginTitle(const QString &t)
{
    if (t.isEmpty())
        return false;
    return t.contains(QStringLiteral("Riot Client"), Qt::CaseInsensitive)
           || t.compare(QStringLiteral("Riot"), Qt::CaseInsensitive) == 0
           || t.contains(QStringLiteral("Sign in"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Войти"), Qt::CaseInsensitive);
}

static void considerRiotCandidate(RiotLoginEnumCtx *c, HWND h, const QString &t,
                                  const QString &cls, int w, int hgt, DWORD pid, bool fromRiot)
{
    if (!fromRiot && !isRiotLoginTitle(t))
        return;

    const bool chrome = cls.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("Chrome_RenderWidget"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("Intermediate D3D"), Qt::CaseInsensitive);

    // Любое крупное окно Riot-процесса — кандидат (раньше порог 70 отсекал всё)
    int score = w * hgt / 1000;
    if (fromRiot) score += 100;
    if (isRiotLoginTitle(t)) score += 80;
    if (chrome) score += 50;
    if (w >= 500 && hgt >= 400)
        score += 40;

    if (score < 50)
        return;

    if (score > c->score) {
        c->score = score;
        c->best = h;
        c->title = (t.isEmpty() ? QStringLiteral("(no title)") : t)
                   + QStringLiteral(" [") + cls + QLatin1Char(' ')
                   + QString::number(w) + QLatin1Char('x') + QString::number(hgt)
                   + QStringLiteral(" pid=") + QString::number(pid)
                   + QStringLiteral(" score=") + QString::number(score)
                   + QLatin1Char(']');
    }
}

static BOOL CALLBACK enumRiotLoginProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<RiotLoginEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    const bool fromRiot = c->riotPids.contains(pid);

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString cls = QString::fromWCharArray(wcls);

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    if (w < 400 || hgt < 300)
        return TRUE;

    if (c->doDump && fromRiot) {
        c->dump << QStringLiteral("TOP pid=%1 %2 | \"%3\" | %4 %5x%6")
                       .arg(pid)
                       .arg(processImageForPid(pid))
                       .arg(t.isEmpty() ? QStringLiteral("(no title)") : t)
                       .arg(cls)
                       .arg(w)
                       .arg(hgt);
    }

    considerRiotCandidate(c, h, t, cls, w, hgt, pid, fromRiot);
    return TRUE;
}
#endif

RiotAuth::RiotAuth(QObject *parent)
    : IPlatformAuth(parent)
{
}

RiotAuth::~RiotAuth()
{
    stopScout();
}

void RiotAuth::silentKill(const QString &image)
{
#ifdef Q_OS_WIN
    auto *p = new QProcess;
    p->setProgram(QStringLiteral("taskkill"));
    p->setArguments({QStringLiteral("/F"), QStringLiteral("/T"),
                     QStringLiteral("/IM"), image});
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     p, &QObject::deleteLater);
    p->start();
#else
    Q_UNUSED(image);
#endif
}

void RiotAuth::killLauncher()
{
    qWarning() << "[RIOT] killLauncher";
    silentKill(QStringLiteral("RiotClientServices.exe"));
    silentKill(QStringLiteral("RiotClient.exe"));
    silentKill(QStringLiteral("RiotClientUx.exe"));
    silentKill(QStringLiteral("Riot Client.exe"));
    silentKill(QStringLiteral("LeagueClient.exe"));
    silentKill(QStringLiteral("LeagueClientUx.exe"));
    silentKill(QStringLiteral("LeagueClientUxRender.exe"));
    silentKill(QStringLiteral("LeagueCrashHandler64.exe"));
}

void RiotAuth::clearLocalSession()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                         + QStringLiteral("/Riot Games/Riot Client/Data");
    const QStringList files = {
        base + QStringLiteral("/RiotGamesPrivateSettings.yaml"),
        base + QStringLiteral("/RiotClientPrivateSettings.yaml"),
    };
    for (const QString &path : files) {
        if (QFile::exists(path) && QFile::remove(path))
            qWarning() << "[RIOT] cleared session file:" << path;
    }
}

bool RiotAuth::applyCache(const QJsonObject &authData)
{
    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }
    if (!exe.isEmpty())
        m_launcherExe = exe;
    m_gameTitle = authData.value(QStringLiteral("game_title")).toString();

    const QString mode = authData.value(QStringLiteral("auth")).toObject()
                             .value(QStringLiteral("mode")).toString();
    const QString login = authData.value(QStringLiteral("login")).toString().trimmed();
    const QString password = authData.value(QStringLiteral("password")).toString();

    // Личный вход — не трогаем сессию и не ждём scout.
    if (mode == QLatin1String("personal") || login.isEmpty() || password.isEmpty()) {
        qWarning() << "[RIOT] applyCache: personal / нет credentials — без scout";
        m_expectInteractive = false;
        m_allowsGameDetect = true;
        return true;
    }

    // Клубный: сбрасываем remember-me, чтобы не зайти под чужим аккаунтом.
    clearLocalSession();
    m_expectInteractive = true;
    m_allowsGameDetect = false;
    qWarning() << "[RIOT] applyCache: клубный → interactive scout для" << login;
    return false;
}

void RiotAuth::startLauncher(QProcess *process,
                             const QJsonObject &authData,
                             const QString &appIdHint)
{
    Q_UNUSED(appIdHint);
    if (!process)
        return;

    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }

    QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();
    if (argsStr.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        argsStr = launcher.value(QStringLiteral("args")).toString().trimmed();
    }

    const QString defaultRiot =
        QStringLiteral("C:\\Riot Games\\Riot Client\\RiotClientServices.exe");
    const QString title = authData.value(QStringLiteral("game_title")).toString();
    if (!title.isEmpty())
        m_gameTitle = title;

    auto looksLikeExePath = [](const QString &s) {
        const QString t = s.trimmed();
        if (t.isEmpty())
            return false;
        if (t.contains(QStringLiteral("--launch-product"), Qt::CaseInsensitive))
            return false;
        return t.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
            || t.contains(QStringLiteral("Riot Client"), Qt::CaseInsensitive)
            || t.startsWith(QStringLiteral(":\\"))
            || (t.size() >= 3 && t.at(1) == QLatin1Char(':'));
    };

    if (looksLikeExePath(argsStr)) {
        QString recovered = argsStr;
        if (recovered.startsWith(QStringLiteral(":\\")))
            recovered.prepend(QLatin1Char('C'));
        if (recovered.contains(QStringLiteral("RiotClientServices"), Qt::CaseInsensitive))
            exe = recovered;
        argsStr.clear();
    }

    if (!exe.contains(QStringLiteral("RiotClientServices"), Qt::CaseInsensitive)) {
        if (QFileInfo::exists(defaultRiot))
            exe = defaultRiot;
    }

    if (argsStr.isEmpty()) {
        if (m_gameTitle.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive))
            argsStr = QStringLiteral("--launch-product=valorant --launch-patchline=live");
        else if (m_gameTitle.contains(QStringLiteral("League"), Qt::CaseInsensitive))
            argsStr = QStringLiteral("--launch-product=league_of_legends --launch-patchline=live");
    }

    if (exe.isEmpty()) {
        qCritical() << "[RIOT] exe_path пуст";
        return;
    }

    const QFileInfo fi(exe);
    m_launcherExe = exe;
    m_productArgs = argsStr;
    m_productLaunchScheduled = false;
    m_productLaunchOk = false;
    m_playClickDone = false;
    m_playClickStop = false;
    m_overlayDismissScheduled = false;
    m_overlayDismissed = false;
    process->setWorkingDirectory(fi.absolutePath());

    // С product-args с первого старта — после логина Riot часто сам продолжает launch.
    // Доп. ShellExecute только ПОСЛЕ записи session yaml (раньше — сброс логина).
    QStringList args;
    if (!argsStr.isEmpty())
        args = QProcess::splitCommand(argsStr);
    if (m_expectInteractive)
        qWarning() << "[RIOT] старт с product (после логина ждём session yaml):" << m_productArgs;

    // TEMP DEBUG: шелл не перекрывает Riot Client
    setShellTopmostFrom(this, false);
    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->hideShellForGame();

    qWarning().noquote() << "[RIOT] Launch exe:" << exe;
    qWarning().noquote() << "[RIOT] Launch args:" << args;
    process->start(exe, args);
}

bool RiotAuth::isGameProcessRunning() const
{
#ifdef Q_OS_WIN
    const QStringList images = {
        QStringLiteral("LeagueClient.exe"),
        QStringLiteral("LeagueClientUx.exe"),
        QStringLiteral("VALORANT-Win64-Shipping.exe"),
        QStringLiteral("Valorant.exe"),
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile);
            for (const QString &img : images) {
                if (name.compare(img, Qt::CaseInsensitive) == 0) {
                    found = true;
                    break;
                }
            }
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    return false;
#endif
}

bool RiotAuth::parseProductArgs(QString *productId, QString *patchline) const
{
    if (!productId || !patchline)
        return false;
    *productId = QStringLiteral("league_of_legends");
    *patchline = QStringLiteral("live");
    if (m_gameTitle.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive))
        *productId = QStringLiteral("valorant");

    const QStringList parts = QProcess::splitCommand(m_productArgs);
    for (const QString &p : parts) {
        if (p.startsWith(QStringLiteral("--launch-product="), Qt::CaseInsensitive))
            *productId = p.mid(QStringLiteral("--launch-product=").size());
        else if (p.startsWith(QStringLiteral("--launch-patchline="), Qt::CaseInsensitive))
            *patchline = p.mid(QStringLiteral("--launch-patchline=").size());
    }
    return !productId->isEmpty();
}

QString RiotAuth::sessionSettingsPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/Riot Games/Riot Client/Data/RiotGamesPrivateSettings.yaml");
}

void RiotAuth::scheduleProductLaunch()
{
    if (m_productLaunchScheduled)
        return;
    m_productLaunchScheduled = true;
    m_productLaunchOk = false;
    m_playClickDone = false;
    m_sessionPollTicks = 0;
    m_rsoRetryCount = 0;
    qWarning() << "[RIOT] после логина: ждём RSO/session, затем product-launcher/.../launch";
    pollSessionThenLaunchProduct();
}

QNetworkAccessManager *RiotAuth::ensureNam()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::sslErrors,
                this, [](QNetworkReply *reply, const QList<QSslError> &) {
                    if (reply)
                        reply->ignoreSslErrors();
                });
    }
    return m_nam;
}

bool RiotAuth::readRiotLockfile(int *port, QString *password, QString *protocol) const
{
    const QString lockPath =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/Riot Games/Riot Client/Config/lockfile");
    QFile lf(lockPath);
    if (!lf.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QStringList parts = QString::fromUtf8(lf.readAll()).trimmed().split(QLatin1Char(':'));
    lf.close();
    if (parts.size() < 5)
        return false;
    if (port)
        *port = parts.at(2).toInt();
    if (password)
        *password = parts.at(3);
    if (protocol)
        *protocol = parts.at(4).isEmpty() ? QStringLiteral("https") : parts.at(4);
    return port && *port > 0 && password && !password->isEmpty();
}

void RiotAuth::postRiotApi(const QString &path, const QByteArray &body, const char *why)
{
    int port = 0;
    QString password;
    QString protocol;
    if (!readRiotLockfile(&port, &password, &protocol)) {
        qWarning() << "[RIOT] lockfile нет для" << path << "|" << why;
        return;
    }

    QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization",
                     QByteArrayLiteral("Basic ")
                         + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64());
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    qWarning().noquote() << "[RIOT] API POST" << url.toString()
                         << (body.isEmpty() ? QStringLiteral("{}") : QString::fromUtf8(body))
                         << "|" << why;
    QNetworkReply *reply = ensureNam()->post(req, body.isEmpty() ? QByteArrayLiteral("{}") : body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, why, path]() {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray resp = reply->readAll();
        qWarning().noquote() << "[RIOT] API" << path << "HTTP" << code
                             << "| body:" << QString::fromUtf8(resp.left(250))
                             << "|" << why;
        // 204 на старых endpoints = «принято, но ничего не сделано».
        // На product-launcher/.../launch 2xx считаем успехом запуска.
        if (code >= 200 && code < 300 && path.contains(QStringLiteral("/launch"))) {
            m_productLaunchOk = true;
            qWarning() << "[RIOT] product-launcher OK — ждём процесс игры";
        }
        reply->deleteLater();
    });
}

void RiotAuth::pollSessionThenLaunchProduct()
{
    if (m_sessionPollTimer) {
        m_sessionPollTimer->stop();
        m_sessionPollTimer->deleteLater();
        m_sessionPollTimer = nullptr;
    }

    m_sessionPollTimer = new QTimer(this);
    m_sessionPollTimer->setInterval(500);
    connect(m_sessionPollTimer, &QTimer::timeout, this, [this]() {
        ++m_sessionPollTicks;
        if (isGameProcessRunning()) {
            qWarning() << "[RIOT] игра уже есть — launch не нужен";
            m_productLaunchOk = true;
            scheduleDismissClientOverlay();
            m_sessionPollTimer->stop();
            m_sessionPollTimer->deleteLater();
            m_sessionPollTimer = nullptr;
            return;
        }

        const QFileInfo fi(sessionSettingsPath());
        const bool yamlReady = fi.exists() && fi.size() > 80;
        // Также пробуем по таймеру: RSO иногда готов раньше yaml
        const bool timeReady = m_sessionPollTicks >= 10; // ~5s после Enter
        if (!yamlReady && !timeReady) {
            if (m_sessionPollTicks > 60) {
                qWarning() << "[RIOT] timeout ожидания сессии — launch всё равно";
                m_sessionPollTimer->stop();
                m_sessionPollTimer->deleteLater();
                m_sessionPollTimer = nullptr;
                launchProductViaApi("timeout");
            }
            return;
        }

        if (yamlReady)
            qWarning() << "[RIOT] session yaml готов, size" << fi.size();
        else
            qWarning() << "[RIOT] yaml ещё нет, но прошло ~5s — проверяем RSO и launch";

        m_sessionPollTimer->stop();
        m_sessionPollTimer->deleteLater();
        m_sessionPollTimer = nullptr;

        QTimer::singleShot(2000, this, [this]() {
            if (isGameProcessRunning()) {
                m_productLaunchOk = true;
                qWarning() << "[RIOT] игра стартовала сама — OK";
                scheduleDismissClientOverlay();
                return;
            }
            // RSO check → UIA «Играть» (прямой LeagueClient.exe Windows блокирует)
            launchProductViaApi("after RSO ready");
        });
    });
    m_sessionPollTimer->start();
}

void RiotAuth::finishLaunchNudge(const char *why)
{
    if (isGameProcessRunning()) {
        qWarning() << "[RIOT] процесс игры уже есть |" << why;
        m_productLaunchOk = true;
        m_playClickStop = true;
        scheduleDismissClientOverlay();
        return;
    }
    if (m_playClickDone) {
        qWarning() << "[RIOT] UIA Play уже делали |" << why;
        return;
    }
    m_playClickDone = true;
    qWarning() << "[RIOT] страница «Играть» — один клик Play (без спама) |" << why;
    // Один уверенный клик. Повтор только если кнопка ещё не была видна.
    if (clickPlayButton(why))
        m_playClickStop = true;

    // Редкие retry: только если ещё НЕ кликали уверенно и процесса нет
    for (int ms : {4000, 9000}) {
        QTimer::singleShot(ms, this, [this]() {
            if (m_playClickStop || isGameProcessRunning()) {
                m_playClickStop = true;
                return;
            }
            if (clickPlayButton("retry Play once"))
                m_playClickStop = true;
        });
    }

    // Пока ждём процесс — гасим диалог «уже запущена» и не жмём Play снова
    for (int ms : {2500, 5000, 8000, 12000}) {
        QTimer::singleShot(ms, this, [this]() {
            if (isGameProcessRunning()) {
                m_playClickStop = true;
                m_productLaunchOk = true;
                scheduleDismissClientOverlay();
                return;
            }
            dismissAlreadyRunningDialog("poll");
        });
    }

    QTimer::singleShot(16000, this, [this]() {
        if (isGameProcessRunning()) {
            qWarning() << "[RIOT] LeagueClient поднялся после UIA Play";
            m_playClickStop = true;
            scheduleDismissClientOverlay();
        } else {
            qWarning() << "[RIOT] UIA Play не поднял игру — нужен ручной клик «Играть» "
                          "(прямой LeagueClient.exe Windows блокирует)";
        }
    });
}

void RiotAuth::discoverRiotLaunchPaths(const char *why)
{
    int port = 0;
    QString password;
    QString protocol;
    if (!readRiotLockfile(&port, &password, &protocol))
        return;

    const QByteArray auth = QByteArrayLiteral("Basic ")
        + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64();

    auto getPath = [this, port, protocol, auth, why](const QString &path) {
        QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", auth);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply *reply = ensureNam()->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, why, path]() {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            reply->deleteLater();
            if (code < 200 || code >= 300) {
                qWarning() << "[RIOT] discover" << path << "HTTP" << code;
                return;
            }
            // Вытащим пути с launch / product-launcher
            QStringList found;
            const QString text = QString::fromUtf8(body);
            QRegularExpression re(QStringLiteral("\"(/[^\"]*(?:launch|product-launcher|patchline)[^\"]*)\""));
            auto it = re.globalMatch(text);
            while (it.hasNext() && found.size() < 40) {
                const QString p = it.next().captured(1);
                if (!found.contains(p))
                    found << p;
            }
            qWarning() << "[RIOT] discover" << path << "→" << found.size() << "paths |" << why;
            for (const QString &p : found)
                qWarning().noquote() << "[RIOT]   path:" << p;

            Q_UNUSED(why);
            // Только лог — POST в launch-restriction даёт 405 и ничего не запускает
        });
    };

    // Один раз swagger для диагностики; авто-POST отключён
    getPath(QStringLiteral("/swagger/v3/openapi.json"));

    // Проверим, не блокирует ли Riot запуск
    QString productId, patchline;
    parseProductArgs(&productId, &patchline);
    auto getOnly = [this, port, protocol, auth, why](const QString &path) {
        QUrl url(QStringLiteral("%1://127.0.0.1:%2%3").arg(protocol).arg(port).arg(path));
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", auth);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        QNetworkReply *reply = ensureNam()->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, why, path]() {
            qWarning().noquote() << "[RIOT] GET" << path
                                 << "HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                                 << "| body:" << QString::fromUtf8(reply->readAll().left(250))
                                 << "|" << why;
            reply->deleteLater();
        });
    };
    getOnly(QStringLiteral("/launch-restriction/v1/ready"));
    getOnly(QStringLiteral("/launch-restriction/v1/restrictions/%1").arg(productId));
}

QString RiotAuth::resolveGameExePath() const
{
    QString productId;
    QString patchline;
    parseProductArgs(&productId, &patchline);
    if (productId.contains(QStringLiteral("valorant"), Qt::CaseInsensitive)) {
        const QStringList candidates = {
            QStringLiteral("C:/Riot Games/VALORANT/live/VALORANT.exe"),
            QStringLiteral("D:/Riot Games/VALORANT/live/VALORANT.exe"),
        };
        for (const QString &c : candidates) {
            if (QFileInfo::exists(c))
                return c;
        }
        return {};
    }
    const QStringList candidates = {
        QStringLiteral("C:/Riot Games/League of Legends/LeagueClient.exe"),
        QStringLiteral("D:/Riot Games/League of Legends/LeagueClient.exe"),
        QStringLiteral("E:/Riot Games/League of Legends/LeagueClient.exe"),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return {};
}

void RiotAuth::launchGameExeDirect(const char *why)
{
    // Windows/Vanguard: «нет доступа» к LeagueClient.exe из шелла/explorer.
    // Не вызываем — только спамим диалог ошибки.
    qWarning() << "[RIOT] skip direct LeagueClient — Access Denied на этой машине |" << why;
}

#ifdef Q_OS_WIN
static void riotClickScreen(int x, int y, const char *why)
{
    SetCursorPos(x, y);
    Sleep(40);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
    qWarning() << "[RIOT] mouse click" << x << y << "|" << why;
}

static bool riotNameLooksPlay(const QString &raw)
{
    const QString n = raw.trimmed().toLower();
    if (n.isEmpty())
        return false;
    return n == QStringLiteral("играть")
        || n == QStringLiteral("play")
        || n.contains(QStringLiteral("играть"))
        || (n.contains(QStringLiteral("play")) && !n.contains(QStringLiteral("display")));
}

static int riotRectArea(const RECT &r)
{
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return 0;
    return w * h;
}
#endif

bool RiotAuth::clickPlayButton(const char *why)
{
#ifdef Q_OS_WIN
    if (m_playClickStop || isGameProcessRunning()) {
        m_playClickStop = true;
        qWarning() << "[RIOT] Play click skip — уже стоп/игра жива |" << why;
        return false;
    }

    HWND hwnd = reinterpret_cast<HWND>(m_loginHwnd);
    if (!hwnd || !IsWindow(hwnd))
        hwnd = FindWindowW(nullptr, L"Riot Client");
    if (!hwnd || !IsWindow(hwnd)) {
        qWarning() << "[RIOT] UIA Play — нет HWND |" << why;
        return false;
    }

    AllowSetForegroundWindow(ASFW_ANY);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    Sleep(120);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        qWarning() << "[RIOT] UIA CoCreateInstance fail" << Qt::hex << quint32(hr);
        return false;
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        qWarning() << "[RIOT] UIA ElementFromHandle fail";
        automation->Release();
        return false;
    }

    IUIAutomationElement *best = nullptr;
    int bestArea = 0;
    POINT bestClick{};
    bool haveClick = false;

    auto consider = [&](IUIAutomationElement *el) {
        if (!el)
            return;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name) : QString();
        if (name)
            SysFreeString(name);
        if (!riotNameLooksPlay(n)) {
            el->Release();
            return;
        }

        BOOL off = FALSE;
        el->get_CurrentIsOffscreen(&off);
        RECT r{};
        el->get_CurrentBoundingRectangle(&r);
        const int area = riotRectArea(r);

        POINT cp{};
        BOOL gotCp = FALSE;
        el->GetClickablePoint(&cp, &gotCp);

        qWarning().noquote() << "[RIOT] UIA play-cand:\"" << n << "\" area" << area
                             << "off" << bool(off)
                             << "rect" << r.left << r.top
                             << (r.right - r.left) << "x" << (r.bottom - r.top)
                             << "clickable" << bool(gotCp) << cp.x << cp.y;

        // Ghost 0×0 — не кликаем
        if (off || (area < 80 && !gotCp)) {
            el->Release();
            return;
        }

        const int score = area > 0 ? area : 100;
        if (score >= bestArea) {
            if (best)
                best->Release();
            best = el;
            bestArea = score;
            if (area >= 80) {
                bestClick.x = (r.left + r.right) / 2;
                bestClick.y = (r.top + r.bottom) / 2;
                haveClick = true;
            } else if (gotCp) {
                bestClick = cp;
                haveClick = true;
            } else {
                haveClick = false;
            }
            return;
        }
        el->Release();
    };

    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (trueCond)
        root->FindAll(TreeScope_Descendants, trueCond, &all);
    int len = 0;
    if (all)
        all->get_Length(&len);
    qWarning() << "[RIOT] UIA descendants:" << len << "|" << why;
    const int limit = qMin(len, 400);
    for (int i = 0; i < limit; ++i) {
        IUIAutomationElement *el = nullptr;
        all->GetElement(i, &el);
        if (!el)
            continue;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name) : QString();
        if (name)
            SysFreeString(name);
        if (!riotNameLooksPlay(n)) {
            el->Release();
            continue;
        }
        consider(el);
    }
    if (all)
        all->Release();
    if (trueCond)
        trueCond->Release();

    bool clicked = false;
    if (best && haveClick) {
        BSTR pname = nullptr;
        best->get_CurrentName(&pname);
        qWarning().noquote() << "[RIOT] UIA Play ONE click:\""
                             << (pname ? QString::fromWCharArray(pname) : QString())
                             << "\" @" << bestClick.x << bestClick.y
                             << "area" << bestArea << "|" << why;
        if (pname)
            SysFreeString(pname);
        // Только один физический клик — без Invoke и без heuristic-сетки
        riotClickScreen(bestClick.x, bestClick.y, why);
        clicked = true;
        m_playClickStop = true;
        best->Release();
        best = nullptr;
    } else {
        if (best) {
            best->Release();
            best = nullptr;
        }
        // Кнопка ещё не в дереве — ждём следующий retry, не кликаем вслепую
        qWarning() << "[RIOT] UIA «Играть» ещё не видна — ждём retry |" << why;
    }

    root->Release();
    automation->Release();
    return clicked;
#else
    Q_UNUSED(why);
    return false;
#endif
}

#ifdef Q_OS_WIN
static HWND findLeagueClientHwnd()
{
    struct Cand {
        HWND hwnd = nullptr;
        int area = 0;
        QString title;
        QString img;
    };
    struct Ctx {
        Cand best;
        int logged = 0;
    } ctx;

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid)
            return TRUE;
        const QString img = processImageForPid(pid);
        if (!img.contains(QStringLiteral("LeagueClient"), Qt::CaseInsensitive))
            return TRUE;

        RECT r{};
        GetWindowRect(hwnd, &r);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        if (w < 200 || h < 200)
            return TRUE;

        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 255);
        const QString t = QString::fromWCharArray(title);
        const bool visible = IsWindowVisible(hwnd);
        int score = w * h;
        if (t.contains(QStringLiteral("League"), Qt::CaseInsensitive))
            score += 5000000;
        if (visible)
            score += 1000000;

        if (c->logged < 8) {
            qWarning().noquote() << "[RIOT] League HWND cand:" << t
                                 << img << w << "x" << h
                                 << "vis" << visible << "score" << score;
            ++c->logged;
        }

        if (score > c->best.area) {
            c->best.hwnd = hwnd;
            c->best.area = score;
            c->best.title = t;
            c->best.img = img;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.best.hwnd) {
        qWarning().noquote() << "[RIOT] League HWND pick:\"" << ctx.best.title << "\""
                             << ctx.best.img << "score" << ctx.best.area;
        return ctx.best.hwnd;
    }

    HWND byTitle = FindWindowW(nullptr, L"League of Legends");
    if (byTitle && IsWindow(byTitle))
        return byTitle;
    return nullptr;
}

static bool riotNameLooksStart(const QString &raw)
{
    const QString n = raw.trimmed().toLower();
    if (n.isEmpty())
        return false;
    return n == QStringLiteral("начать")
        || n == QStringLiteral("start")
        || n.contains(QStringLiteral("начать"))
        || n.contains(QStringLiteral("продолжить"))
        || (n.contains(QStringLiteral("continue")) && !n.contains(QStringLiteral("account")));
}
#endif

void RiotAuth::scheduleDismissClientOverlay()
{
    if (m_overlayDismissScheduled)
        return;
    m_overlayDismissScheduled = true;
    qWarning() << "[RIOT] планируем закрытие оверлея LoL (ОБУЧЕНИЕ → НАЧАТЬ)";
    // Окно клиента появляется позже процесса; туториал — ещё позже
    const int delaysMs[] = {2000, 4500, 7000, 10000, 14000, 20000};
    for (int ms : delaysMs) {
        QTimer::singleShot(ms, this, [this]() {
            if (!m_overlayDismissed)
                dismissClientOverlay("League tutorial");
        });
    }
}

void RiotAuth::dismissAlreadyRunningDialog(const char *why)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_loginHwnd);
    if (!hwnd || !IsWindow(hwnd))
        hwnd = FindWindowW(nullptr, L"Riot Client");
    if (!hwnd || !IsWindow(hwnd))
        return;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void **>(&automation)))
        || !automation)
        return;

    IUIAutomationElement *root = nullptr;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
        automation->Release();
        return;
    }

    const wchar_t *names[] = {L"Понятно", L"OK", L"Ok", L"Got it"};
    IUIAutomationElement *btn = nullptr;
    for (const wchar_t *nm : names) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(nm);
        IUIAutomationCondition *cond = nullptr;
        automation->CreatePropertyCondition(UIA_NamePropertyId, v, &cond);
        if (cond) {
            root->FindFirst(TreeScope_Descendants, cond, &btn);
            cond->Release();
        }
        VariantClear(&v);
        if (btn)
            break;
    }

    if (btn) {
        RECT r{};
        btn->get_CurrentBoundingRectangle(&r);
        if (r.right > r.left && (r.right - r.left) * (r.bottom - r.top) >= 80) {
            const int cx = (r.left + r.right) / 2;
            const int cy = (r.top + r.bottom) / 2;
            AllowSetForegroundWindow(ASFW_ANY);
            SetForegroundWindow(hwnd);
            riotClickScreen(cx, cy, "already-running Понятно");
            qWarning() << "[RIOT] закрыли диалог «уже запущена» |" << why;
            m_playClickStop = true; // больше не жмём Play
        }
        btn->Release();
    }

    root->Release();
    automation->Release();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::dismissClientOverlay(const char *why)
{
#ifdef Q_OS_WIN
    if (m_overlayDismissed)
        return;

    HWND hwnd = findLeagueClientHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        qWarning() << "[RIOT] overlay — нет HWND League Client |" << why;
        return;
    }

    AllowSetForegroundWindow(ASFW_ANY);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    Sleep(150);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        qWarning() << "[RIOT] overlay UIA CoCreate fail" << Qt::hex << quint32(hr);
        return;
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        qWarning() << "[RIOT] overlay ElementFromHandle fail |" << why;
        automation->Release();
        return;
    }

    // Как Play: обход всех потомков (Button type у CEF часто пуст)
    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    IUIAutomationElementArray *all = nullptr;
    if (trueCond)
        root->FindAll(TreeScope_Descendants, trueCond, &all);
    int len = 0;
    if (all)
        all->get_Length(&len);
    qWarning() << "[RIOT] overlay UIA descendants:" << len << "|" << why;

    IUIAutomationElement *best = nullptr;
    int bestArea = 0;
    POINT bestClick{};
    bool haveClick = false;
    int logged = 0;

    const int limit = qMin(len, 500);
    for (int i = 0; i < limit; ++i) {
        IUIAutomationElement *el = nullptr;
        all->GetElement(i, &el);
        if (!el)
            continue;
        BSTR name = nullptr;
        el->get_CurrentName(&name);
        const QString n = name ? QString::fromWCharArray(name) : QString();
        if (name)
            SysFreeString(name);

        BOOL off = FALSE;
        el->get_CurrentIsOffscreen(&off);
        RECT r{};
        el->get_CurrentBoundingRectangle(&r);
        const int area = riotRectArea(r);

        if (logged < 15 && area >= 400 && !n.trimmed().isEmpty()) {
            qWarning().noquote() << "[RIOT] overlay el#" << i << "\"" << n.trimmed() << "\""
                                 << (r.right - r.left) << "x" << (r.bottom - r.top)
                                 << "off" << bool(off);
            ++logged;
        }

        if (!riotNameLooksStart(n) || off || area < 80) {
            el->Release();
            continue;
        }

        POINT cp{};
        BOOL gotCp = FALSE;
        el->GetClickablePoint(&cp, &gotCp);
        qWarning().noquote() << "[RIOT] overlay cand:\"" << n.trimmed() << "\" area" << area
                             << "clickable" << bool(gotCp) << cp.x << cp.y;

        if (area >= bestArea) {
            if (best)
                best->Release();
            best = el;
            bestArea = area;
            bestClick.x = (r.left + r.right) / 2;
            bestClick.y = (r.top + r.bottom) / 2;
            haveClick = true;
            if (gotCp && area < 200) {
                bestClick = cp;
            }
            continue;
        }
        el->Release();
    }
    if (all)
        all->Release();
    if (trueCond)
        trueCond->Release();

    if (best && haveClick) {
        BSTR pname = nullptr;
        best->get_CurrentName(&pname);
        qWarning().noquote() << "[RIOT] overlay ONE click:\""
                             << (pname ? QString::fromWCharArray(pname) : QString())
                             << "\" @" << bestClick.x << bestClick.y << "|" << why;
        if (pname)
            SysFreeString(pname);
        riotClickScreen(bestClick.x, bestClick.y, why);
        m_overlayDismissed = true;
        best->Release();
    } else {
        if (best)
            best->Release();
        // CEF часто без имён — клик в зону синей «НАЧАТЬ» (низ центральной карточки).
        // Не ставим m_overlayDismissed: если промах — следующие retry ещё раз.
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const double spots[][2] = {{0.50, 0.70}, {0.50, 0.74}, {0.50, 0.66}};
        qWarning() << "[RIOT] overlay UIA пуст — heuristic НАЧАТЬ |" << why
                   << "client" << cr.right << "x" << cr.bottom;
        for (const auto &s : spots) {
            POINT p;
            p.x = int(cr.right * s[0]);
            p.y = int(cr.bottom * s[1]);
            ClientToScreen(hwnd, &p);
            riotClickScreen(p.x, p.y, "heuristic НАЧАТЬ");
            Sleep(180);
        }
        sendReturnKey();
    }

    root->Release();
    automation->Release();
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::shellExecuteProductOnce(const char *why)
{
    if (isGameProcessRunning()) {
        qWarning() << "[RIOT] ShellExecute skip — игра есть |" << why;
        return;
    }
    if (m_launcherExe.isEmpty() || m_productArgs.trimmed().isEmpty()) {
        qWarning() << "[RIOT] ShellExecute skip — нет exe/args |" << why;
        return;
    }
#ifdef Q_OS_WIN
    const QString work = QFileInfo(m_launcherExe).absolutePath();
    const UINT_PTR r = reinterpret_cast<UINT_PTR>(
        ShellExecuteW(nullptr, L"open",
                      reinterpret_cast<LPCWSTR>(m_launcherExe.utf16()),
                      reinterpret_cast<LPCWSTR>(m_productArgs.utf16()),
                      reinterpret_cast<LPCWSTR>(work.utf16()),
                      SW_SHOWNORMAL));
    qWarning().noquote() << "[RIOT] ShellExecute product (" << why << "):"
                         << m_launcherExe << m_productArgs
                         << "| result:" << r << (r > 32 ? "OK" : "FAIL");
#else
    Q_UNUSED(why);
#endif
}

void RiotAuth::launchProductViaApi(const char *why)
{
    if (isGameProcessRunning()) {
        qWarning() << "[RIOT] API skip — игра уже есть |" << why;
        return;
    }

    QString productId;
    QString patchline;
    parseProductArgs(&productId, &patchline);

    int port = 0;
    QString password;
    QString protocol;
    if (!readRiotLockfile(&port, &password, &protocol)) {
        qWarning() << "[RIOT] lockfile нет |" << why;
        return;
    }

    // 1) Проверяем RSO — без активной сессии launch даёт пустой 204
    QUrl sessionUrl(QStringLiteral("%1://127.0.0.1:%2/rso-auth/v1/session")
                        .arg(protocol).arg(port));
    QNetworkRequest sessionReq(sessionUrl);
    sessionReq.setRawHeader("Authorization",
                            QByteArrayLiteral("Basic ")
                                + QStringLiteral("riot:%1").arg(password).toUtf8().toBase64());
    sessionReq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    qWarning() << "[RIOT] GET /rso-auth/v1/session |" << why;
    QNetworkReply *sessionReply = ensureNam()->get(sessionReq);
    connect(sessionReply, &QNetworkReply::finished, this,
            [this, sessionReply, why, productId, patchline]() {
        const int code = sessionReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = sessionReply->readAll();
        qWarning().noquote() << "[RIOT] RSO session HTTP" << code
                             << "| body:" << QString::fromUtf8(body.left(300))
                             << "|" << why;
        sessionReply->deleteLater();

        // После Tab-логина страница «Играть» уже бывает открыта — не блокируемся на парсинге RSO
        const bool looksLoggedIn = m_credentialsSent
            || (code >= 200 && code < 300 && body.size() > 2);

        if (!looksLoggedIn) {
            if (m_rsoRetryCount < 4) {
                ++m_rsoRetryCount;
                qWarning() << "[RIOT] RSO ещё не залогинен — retry" << m_rsoRetryCount << "/4 через 3s";
                QTimer::singleShot(3000, this, [this]() {
                    if (!isGameProcessRunning())
                        launchProductViaApi("retry after RSO wait");
                });
            } else {
                qWarning() << "[RIOT] RSO так и не готов — finishLaunchNudge";
                finishLaunchNudge("RSO never ready");
            }
            return;
        }

        Q_UNUSED(productId);
        Q_UNUSED(patchline);
        // HTTP launch endpoint на этой сборке нет — только UIA «Играть»
        discoverRiotLaunchPaths(why); // GET restrictions + swagger log
        QTimer::singleShot(800, this, [this, why]() {
            finishLaunchNudge(why);
        });
    });
}

void RiotAuth::stopScout()
{
    if (!m_scoutTimer)
        return;
    m_scoutTimer->stop();
    m_scoutTimer->deleteLater();
    m_scoutTimer = nullptr;
}

void RiotAuth::startScout(const QString &login, const QString &password)
{
#ifdef Q_OS_WIN
    stopScout();
    m_ticks = 0;
    m_phaseTick = 0;
    m_stableCount = 0;
    m_lastW = 0;
    m_lastH = 0;
    m_loginHwnd = 0;
    m_phase = Phase::WaitLoginWindow;
    m_credentialsSent = false;
    m_login = login;
    m_password = password;

    if (!m_expectInteractive || login.isEmpty() || password.isEmpty()) {
        qWarning() << "[RIOT] Scout пропущен (personal / silent)";
        m_allowsGameDetect = true;
        return;
    }

    m_allowsGameDetect = false;
    setShellTopmostFrom(this, false);
    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->hideShellForGame();
    qWarning() << "[RIOT] Scout START (phase machine), login:" << login
               << "| pass len" << password.size();

    m_scoutTimer = new QTimer(this);
    m_scoutTimer->setInterval(300);

    connect(m_scoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_scoutTimer)
            return;

        ++m_ticks;
        if (m_ticks > 300) {
            qWarning() << "[RIOT] Scout TIMEOUT phase" << int(m_phase);
            m_allowsGameDetect = true;
            setShellTopmostFrom(this, true);
            stopScout();
            return;
        }

        if (m_phase == Phase::Done) {
            stopScout();
            return;
        }

        RiotLoginEnumCtx ctx;
        ctx.riotPids = collectRiotPids();
        ctx.doDump = (m_ticks % 8 == 0
                      && (m_phase == Phase::WaitLoginWindow
                          || m_phase == Phase::WaitStableSize));
        EnumWindows(enumRiotLoginProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.best) {
            HWND byTitle = FindWindowW(nullptr, L"Riot Client");
            if (byTitle && IsWindowVisible(byTitle)) {
                ctx.best = byTitle;
                ctx.title = QStringLiteral("FindWindow(Riot Client)");
            }
        }

        HWND hwnd = ctx.best ? ctx.best : reinterpret_cast<HWND>(m_loginHwnd);
        int w = 0, hgt = 0;
        if (hwnd && IsWindow(hwnd)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            w = rc.right - rc.left;
            hgt = rc.bottom - rc.top;
        }

        if (ctx.doDump) {
            qWarning() << "[RIOT] tick" << m_ticks << "| phase" << int(m_phase)
                       << "| best:" << (ctx.best ? ctx.title : QStringLiteral("(none)"))
                       << "| size" << w << "x" << hgt;
        }

        switch (m_phase) {
        case Phase::WaitLoginWindow:
            if (ctx.best && m_ticks >= 10) {
                m_loginHwnd = reinterpret_cast<quintptr>(ctx.best);
                m_lastW = w;
                m_lastH = hgt;
                m_stableCount = 0;
                m_phase = Phase::WaitStableSize;
                m_phaseTick = m_ticks;
                qWarning() << "[RIOT] окно найдено, ждём стабильный размер:" << ctx.title;
            }
            break;

        case Phase::WaitStableSize: {
            // Промежуточный 1300×600 — ещё не форма; клики по нему открывают браузер (Cloudflare).
            // Ждём финальный размер (~1536×864) и стабильность, без мыши.
            const bool largeEnough = (w >= 1400 && hgt >= 700);
            if (largeEnough && w == m_lastW && hgt == m_lastH)
                ++m_stableCount;
            else {
                m_stableCount = 0;
                m_lastW = w;
                m_lastH = hgt;
                if (ctx.best)
                    m_loginHwnd = reinterpret_cast<quintptr>(ctx.best);
                if (!largeEnough && (m_ticks % 8 == 0))
                    qWarning() << "[RIOT] ждём большой UI, сейчас" << w << "x" << hgt;
            }
            if (largeEnough && m_stableCount >= 6 && m_ticks >= m_phaseTick + 10) {
                m_phase = Phase::TypeUsername;
                m_phaseTick = m_ticks;
                qWarning() << "[RIOT] UI готов" << m_lastW << "x" << m_lastH
                           << "→ username (без клика, фокус уже в поле)";
            }
            break;
        }

        case Phase::TypeUsername: {
            // Пауза после стабилизации. Не кликать и не SetFocus — фокус уже в username.
            if (m_ticks < m_phaseTick + 5)
                break;
            HWND h = reinterpret_cast<HWND>(m_loginHwnd);
            if (!h || !IsWindow(h)) {
                qWarning() << "[RIOT] HWND потерян на username";
                m_phase = Phase::WaitLoginWindow;
                break;
            }
            setShellTopmostFrom(this, false);
            // Только foreground, если шелл перехватил; без BringWindowToTop/SetFocus
            if (GetForegroundWindow() != h) {
                AllowSetForegroundWindow(ASFW_ANY);
                SetForegroundWindow(h);
                Sleep(150);
            }
            for (int i = 0; i < 48; ++i)
                sendVk(VK_BACK);
            Sleep(100);
            qWarning() << "[RIOT] type username len" << m_login.size() << m_login;
            typeUnicode(m_login);
            m_credentialsSent = true;
            m_phase = Phase::TabToPassword;
            m_phaseTick = m_ticks;
            break;
        }

        case Phase::TabToPassword:
            if (m_ticks < m_phaseTick + 2)
                break;
            sendTabs(1, "→ password");
            m_phase = Phase::TypePassword;
            m_phaseTick = m_ticks;
            break;

        case Phase::TypePassword:
            if (m_ticks < m_phaseTick + 2)
                break;
            Sleep(80);
            for (int i = 0; i < 40; ++i)
                sendVk(VK_BACK);
            Sleep(60);
            qWarning() << "[RIOT] type password len" << m_password.size();
            typeUnicode(m_password);
            m_phase = Phase::TabToSubmit;
            m_phaseTick = m_ticks;
            break;

        case Phase::TabToSubmit:
            if (m_ticks < m_phaseTick + 2)
                break;
            sendTabs(5, "→ Войти");
            m_phase = Phase::PressEnter;
            m_phaseTick = m_ticks;
            break;

        case Phase::PressEnter:
            if (m_ticks < m_phaseTick + 1)
                break;
            Sleep(80);
            sendReturnKey();
            m_allowsGameDetect = true;
            m_phase = Phase::Done;
            qWarning() << "[RIOT] Enter на Войти — ждём session yaml → ShellExecute product";
            stopScout();
            scheduleProductLaunch();
            break;

        case Phase::Done:
            break;
        }
    });

    m_scoutTimer->start();
#else
    Q_UNUSED(login);
    Q_UNUSED(password);
    m_allowsGameDetect = true;
#endif
}
