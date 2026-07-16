#include "steamauth.h"
#include "networkmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStringConverter>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

static bool writeTextFile(const QString &path, const QString &content)
{
    if (content.isEmpty())
        return false;
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[STEAM] VDF write fail:" << path << file.errorString();
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << content;
    file.close();
    return true;
}

static QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    return in.readAll();
}

#ifdef Q_OS_WIN
static void parkWindowOffscreen(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    SetWindowPos(hwnd, nullptr, -20000, -20000, w, h,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

struct SteamAuthEnumCtx {
    HWND loginHwnd = nullptr;
    QString loginTitle;
    int loginScore = -1;
    HWND pickerHwnd = nullptr;
    QString pickerTitle;
    int pickerScore = -1;
    HWND steamDlgHwnd = nullptr;
    QString steamDlgTitle;
    int steamDlgScore = -1;
};

static QString steamWindowLabel(const QString &t, const QString &cls, int w, int hgt)
{
    return t + QStringLiteral(" [") + cls + QLatin1Char(' ')
           + QString::number(w) + QLatin1Char('x') + QString::number(hgt) + QLatin1Char(']');
}

static bool isSteamAccountPickerTitle(const QString &t)
{
    return t.contains(QStringLiteral("играт"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("playing"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Who's"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Who will"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("выбер"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("пользовател"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("этом комп"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("this computer"), Qt::CaseInsensitive);
}

static bool isSteamLoginTitle(const QString &t)
{
    return t.contains(QStringLiteral("Войти"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Вход"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Sign"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Login"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Аккаунт"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Account"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Guard"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("парол"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("password"), Qt::CaseInsensitive);
}

static BOOL CALLBACK enumSteamAuthProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<SteamAuthEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();
    if (t.isEmpty())
        return TRUE;

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString clsStr = QString::fromWCharArray(wcls);

    if (clsStr == QStringLiteral("CabinetWClass")
        || clsStr.startsWith(QStringLiteral("Qt"), Qt::CaseInsensitive)
        || clsStr.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
        || clsStr.contains(QStringLiteral("Tray"), Qt::CaseInsensitive)
        || clsStr == QStringLiteral("Progman")
        || clsStr == QStringLiteral("WorkerW"))
        return TRUE;

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    const bool offscreen = (rc.left <= -5000 || rc.top <= -5000);

    if (!offscreen && (w < 400 || hgt < 280 || w >= 1200 || hgt >= 900))
        return TRUE;

    const bool isSdl = clsStr.contains(QStringLiteral("SDL"), Qt::CaseInsensitive);
    const bool isPicker = isSteamAccountPickerTitle(t);
    const bool isLogin = !isPicker && isSteamLoginTitle(t);
    const bool isSteamTitle =
        t.compare(QStringLiteral("Steam"), Qt::CaseInsensitive) == 0
        || t.contains(QStringLiteral("Steam"), Qt::CaseInsensitive);

    if (isPicker) {
        int score = 80;
        if (isSdl) score += 20;
        if (score > c->pickerScore) {
            c->pickerScore = score;
            c->pickerHwnd = h;
            c->pickerTitle = steamWindowLabel(t, clsStr, w, hgt);
        }
        return TRUE;
    }

    if (isLogin) {
        int score = 50;
        if (isSdl) score += 40;
        if (w > 600 && w < 900 && hgt > 380 && hgt < 550) score += 20;
        if (score > c->loginScore) {
            c->loginScore = score;
            c->loginHwnd = h;
            c->loginTitle = steamWindowLabel(t, clsStr, w, hgt);
        }
        return TRUE;
    }

    if (isSteamTitle && isSdl) {
        int score = 40;
        if (w > 500 && w < 1000 && hgt > 300 && hgt < 700) score += 20;
        if (score > c->steamDlgScore) {
            c->steamDlgScore = score;
            c->steamDlgHwnd = h;
            c->steamDlgTitle = steamWindowLabel(t, clsStr, w, hgt);
        }
    }
    return TRUE;
}

static void sendReturnKey()
{
    keybd_event(VK_RETURN, 0, 0, 0);
    keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
}
#endif

SteamAuth::SteamAuth(QObject *parent)
    : IPlatformAuth(parent)
{
}

SteamAuth::~SteamAuth()
{
    stopScout();
}

QString SteamAuth::steamInstallPath()
{
    QSettings settings(QStringLiteral("REACTOR"), QStringLiteral("REACTOR SHELL"));
    return settings.value(QStringLiteral("Paths/steam_path"),
                          QStringLiteral("C:/Program Files (x86)/Steam")).toString();
}

QString SteamAuth::resolveAppId(const QJsonObject &authData, const QString &appIdHint)
{
    QString appId = appIdHint.trimmed();
    if (appId.isEmpty())
        appId = authData.value(QStringLiteral("platform_app_id")).toString().trimmed();

    const QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();
    if (appId.isEmpty() && !argsStr.isEmpty()) {
        QRegularExpression reAppl(QStringLiteral("-applaunch\\s+(\\d+)"));
        QRegularExpressionMatch match = reAppl.match(argsStr);
        if (match.hasMatch()) {
            appId = match.captured(1);
        } else {
            QRegularExpression reNum(QStringLiteral("^(\\d{2,})\\b"));
            match = reNum.match(argsStr);
            if (match.hasMatch())
                appId = match.captured(1);
        }
    }
    return appId;
}

QString SteamAuth::localAppDataSteamVdfPath() const
{
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty())
        return QString();
    return localAppData + QStringLiteral("/Steam/local.vdf");
}

QString SteamAuth::buildLoginUsersVdf(const QString &steamId,
                                      const QString &login,
                                      const QString &persona,
                                      const QString &existing) const
{
    if (!existing.trimmed().isEmpty() && !steamId.isEmpty() && existing.contains(steamId))
        return existing;

    const QString id = steamId.isEmpty() ? QStringLiteral("0") : steamId;
    const QString name = persona.isEmpty() ? login : persona;
    const QString ts = QString::number(QDateTime::currentSecsSinceEpoch());

    return QString(
        "\"users\"\n"
        "{\n"
        "\t\"%1\"\n"
        "\t{\n"
        "\t\t\"AccountName\"\t\t\"%2\"\n"
        "\t\t\"PersonaName\"\t\t\"%3\"\n"
        "\t\t\"RememberPassword\"\t\t\"1\"\n"
        "\t\t\"WantsOfflineMode\"\t\t\"0\"\n"
        "\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\n"
        "\t\t\"AllowAutoLogin\"\t\t\"1\"\n"
        "\t\t\"MostRecent\"\t\t\"1\"\n"
        "\t\t\"Timestamp\"\t\t\"%4\"\n"
        "\t}\n"
        "}\n"
    ).arg(id, login, name, ts);
}

void SteamAuth::killLauncher()
{
#ifdef Q_OS_WIN
    auto silentKill = [](const QString &image) {
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
    };
    silentKill(QStringLiteral("steam.exe"));
    silentKill(QStringLiteral("steamwebhelper.exe"));
#endif
}

bool SteamAuth::applyCache(const QJsonObject &authData)
{
    const QString login = authData.value(QStringLiteral("login")).toString();
    const QString steamId = authData.value(QStringLiteral("steam_id")).toString();
    const QString persona = authData.value(QStringLiteral("persona_name")).toString(login);

    QJsonObject vdf = authData.value(QStringLiteral("vdf_files")).toObject();
    if (vdf.isEmpty()) {
        const QJsonObject auth = authData.value(QStringLiteral("auth")).toObject();
        const QJsonObject cache = auth.value(QStringLiteral("cache")).toObject();
        if (cache.contains(QStringLiteral("vdf_files")))
            vdf = cache.value(QStringLiteral("vdf_files")).toObject();
        else
            vdf = cache;
    }

    const QString steamPath = steamInstallPath();
    const QString configDir = steamPath + QStringLiteral("/config");
    QDir().mkpath(configDir);

    const QString configVdf = vdf.value(QStringLiteral("config_vdf")).toString();
    if (!configVdf.isEmpty())
        writeTextFile(configDir + QStringLiteral("/config.vdf"), configVdf);

    const QString loginUsers = buildLoginUsersVdf(
        steamId, login, persona, vdf.value(QStringLiteral("loginusers_vdf")).toString()
    );
    writeTextFile(configDir + QStringLiteral("/loginusers.vdf"), loginUsers);

#ifdef Q_OS_WIN
    if (!login.isEmpty()) {
        QSettings steamReg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                           QSettings::NativeFormat);
        steamReg.setValue(QStringLiteral("AutoLoginUser"), login);
        steamReg.setValue(QStringLiteral("RememberPassword"), 1);
    }
#endif

    const QString cachedLocal = vdf.value(QStringLiteral("local_vdf")).toString().trimmed();
    if (!cachedLocal.isEmpty()) {
        writeTextFile(configDir + QStringLiteral("/local.vdf"), cachedLocal);
        const QString appLocal = localAppDataSteamVdfPath();
        if (!appLocal.isEmpty())
            writeTextFile(appLocal, cachedLocal);
        return true;
    }

    qWarning() << "[STEAM] Нет machine-cache — нужен логин/пароль";
    return false;
}

void SteamAuth::startLauncher(QProcess *process,
                              const QJsonObject &authData,
                              const QString &appIdHint)
{
    if (!process)
        return;

    const QString steamPath = steamInstallPath();
    const QString appId = resolveAppId(authData, appIdHint);
    const QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();

    QStringList args;
    if (!appId.isEmpty() && appId != QStringLiteral("0"))
        args << QStringLiteral("-applaunch") << appId;

    const QStringList passthrough = {
        QStringLiteral("-novid"),
        QStringLiteral("-nojoy"),
        QStringLiteral("-silent"),
        QStringLiteral("-shutdown"),
    };
    for (const QString &flag : passthrough) {
        if (argsStr.contains(flag, Qt::CaseInsensitive) && !args.contains(flag))
            args << flag;
    }
    if (!args.contains(QStringLiteral("-shutdown")))
        args << QStringLiteral("-shutdown");

    process->setWorkingDirectory(steamPath);
    process->start(steamPath + QStringLiteral("/steam.exe"), args);
}

void SteamAuth::stopScout()
{
    if (!m_authScoutTimer)
        return;
    m_authScoutTimer->stop();
    m_authScoutTimer->deleteLater();
    m_authScoutTimer = nullptr;
}

void SteamAuth::startScout(const QString &login, const QString &password)
{
#ifdef Q_OS_WIN
    stopScout();
    m_scoutTicks = 0;
    m_scoutInjectTick = 0;
    m_scoutInjected = false;
    m_scoutAccountConfirmed = false;

    m_authScoutTimer = new QTimer(this);
    m_authScoutTimer->setInterval(350);

    connect(m_authScoutTimer, &QTimer::timeout, this, [this, login, password]() {
        if (!m_authScoutTimer)
            return;

        ++m_scoutTicks;

        if (m_scoutTicks > 300) {
            if (!m_scoutInjected)
                qWarning() << "[STEAM] Scout timeout — окно входа не найдено";
            else if (!m_scoutAccountConfirmed)
                qWarning() << "[STEAM] Scout timeout — окно выбора аккаунта не подтверждено";
            stopScout();
            return;
        }

        SteamAuthEnumCtx ctx;
        EnumWindows(enumSteamAuthProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.loginHwnd)
            parkWindowOffscreen(ctx.loginHwnd);
        if (ctx.pickerHwnd)
            parkWindowOffscreen(ctx.pickerHwnd);
        if (ctx.steamDlgHwnd)
            parkWindowOffscreen(ctx.steamDlgHwnd);

        if (!m_scoutInjected && ctx.loginHwnd && m_scoutTicks >= 12) {
            m_scoutInjected = true;
            m_scoutInjectTick = m_scoutTicks;
            HWND authHwnd = ctx.loginHwnd;
            qWarning() << "[STEAM] Интерактивный логин (off-screen):" << ctx.loginTitle;

            SetForegroundWindow(authHwnd);
            SetFocus(authHwnd);
            parkWindowOffscreen(authHwnd);

            auto pasteText = [](const QString &text) {
                if (!OpenClipboard(NULL))
                    return;
                EmptyClipboard();
                const std::wstring wstr = text.toStdWString();
                const size_t bytes = (wstr.size() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (!hMem) {
                    CloseClipboard();
                    return;
                }
                memcpy(GlobalLock(hMem), wstr.c_str(), bytes);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
                CloseClipboard();

                keybd_event(VK_CONTROL, 0, 0, 0);
                keybd_event('V', 0, 0, 0);
                keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
                keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
            };

            QTimer::singleShot(400, this, [this, authHwnd, login, password, pasteText]() {
                if (!m_authScoutTimer)
                    return;
                parkWindowOffscreen(authHwnd);
                pasteText(login);
                keybd_event(VK_TAB, 0, 0, 0);
                keybd_event(VK_TAB, 0, KEYEVENTF_KEYUP, 0);

                QTimer::singleShot(300, this, [this, authHwnd, password, pasteText]() {
                    if (!m_authScoutTimer)
                        return;
                    pasteText(password);

                    QTimer::singleShot(200, this, [this, authHwnd]() {
                        if (!m_authScoutTimer)
                            return;
                        sendReturnKey();
                        parkWindowOffscreen(authHwnd);
                        qDebug() << "[STEAM] Логин/пароль отправлены";
                        m_needBackup = true;
                    });
                });
            });
        }

        HWND picker = ctx.pickerHwnd ? ctx.pickerHwnd : ctx.steamDlgHwnd;
        const QString pickerLabel = ctx.pickerHwnd ? ctx.pickerTitle : ctx.steamDlgTitle;
        if (!m_scoutAccountConfirmed && picker) {
            const bool afterLogin = m_scoutInjected && (m_scoutTicks >= m_scoutInjectTick + 5);
            const bool pickerOnly = !m_scoutInjected && ctx.pickerHwnd && m_scoutTicks >= 12;
            const bool steamDlgOnly = !m_scoutInjected && !ctx.loginHwnd && ctx.steamDlgHwnd
                                      && m_scoutTicks >= 16;
            if (afterLogin || pickerOnly || steamDlgOnly) {
                m_scoutAccountConfirmed = true;
                qWarning() << "[STEAM] Выбор аккаунта (off-screen):" << pickerLabel;

                SetForegroundWindow(picker);
                SetFocus(picker);
                parkWindowOffscreen(picker);

                QTimer::singleShot(250, this, [this, picker]() {
                    if (!m_authScoutTimer)
                        return;
                    parkWindowOffscreen(picker);
                    sendReturnKey();
                    QTimer::singleShot(350, this, [this, picker]() {
                        if (!m_authScoutTimer)
                            return;
                        parkWindowOffscreen(picker);
                        sendReturnKey();
                        qDebug() << "[STEAM] Аккаунт подтверждён (Enter)";
                        m_needBackup = true;
                    });
                });
            }
        }
    });

    m_authScoutTimer->start();
#else
    Q_UNUSED(login);
    Q_UNUSED(password);
#endif
}

void SteamAuth::backupCache(NetworkManager *net, int terminalId, const QString &login)
{
#ifdef Q_OS_WIN
    if (login.isEmpty()) {
        qWarning() << "[STEAM] VDF backup: login пуст";
        return;
    }

    const QString steamPath = steamInstallPath();
    const QString configDir = steamPath + QStringLiteral("/config");

    QString configVdf = readTextFile(configDir + QStringLiteral("/config.vdf"));
    QString loginusersVdf = readTextFile(configDir + QStringLiteral("/loginusers.vdf"));

    QString localVdf = readTextFile(localAppDataSteamVdfPath());
    if (localVdf.isEmpty())
        localVdf = readTextFile(configDir + QStringLiteral("/local.vdf"));

    QJsonObject rootPayload;
    rootPayload.insert(QStringLiteral("login"), login);
    rootPayload.insert(QStringLiteral("terminal_id"), terminalId);
    rootPayload.insert(QStringLiteral("platform"), QStringLiteral("steam"));
    rootPayload.insert(QStringLiteral("config_vdf"), configVdf);
    rootPayload.insert(QStringLiteral("loginusers_vdf"), loginusersVdf);
    rootPayload.insert(QStringLiteral("local_vdf"), localVdf);

    if (!net || net->serverUrl().isEmpty()) {
        qWarning() << "[STEAM] VDF backup: serverUrl пуст";
        return;
    }

    QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qDebug() << "[STEAM] VDF backup → server, bytes:" << jsonData.size();

    QNetworkReply *reply = net->networkAccessManager()->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            qDebug() << "[STEAM] VDF backup OK";
        else
            qWarning() << "[STEAM] VDF backup fail:" << reply->errorString();
    });
#else
    Q_UNUSED(net);
    Q_UNUSED(terminalId);
    Q_UNUSED(login);
#endif
}
