#include "steamauth.h"
#include "networkmanager.h"
#include "pathresolver.h"
#include "processmanager.h"

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
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
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
    if (PathResolver *paths = PathResolver::instance()) {
        const QString configured = paths->steamPath();
        if (!configured.isEmpty() && QFileInfo::exists(configured + QStringLiteral("/steam.exe")))
            return configured;
        if (!configured.isEmpty())
            return configured;
    }
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

QString SteamAuth::forceSilentLoginUsersFlags(const QString &vdf)
{
    if (vdf.trimmed().isEmpty())
        return vdf;
    QString out = vdf;
    auto setKey = [&out](const QString &key, const QString &val) {
        const QRegularExpression re(
            QStringLiteral("(\"%1\"\\s+)\"[^\"]*\"").arg(QRegularExpression::escape(key)),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(out).hasMatch())
            out.replace(re, QStringLiteral("\\1\"%1\"").arg(val));
    };
    // Club silent path requires these even when server sent a stale/wiped loginusers.vdf
    setKey(QStringLiteral("RememberPassword"), QStringLiteral("1"));
    setKey(QStringLiteral("AllowAutoLogin"), QStringLiteral("1"));
    setKey(QStringLiteral("MostRecent"), QStringLiteral("1"));
    return out;
}

QString SteamAuth::ensureConfigAutoLoginUser(const QString &vdf, const QString &login)
{
    if (vdf.trimmed().isEmpty() || login.isEmpty())
        return vdf;
    QString out = vdf;
    const QRegularExpression re(
        QStringLiteral("(\"AutoLoginUser\"\\s+)\"[^\"]*\""),
        QRegularExpression::CaseInsensitiveOption);
    if (re.match(out).hasMatch())
        out.replace(re, QStringLiteral("\\1\"%1\"").arg(login));
    const QRegularExpression reRemember(
        QStringLiteral("(\"RememberPassword\"\\s+)\"[^\"]*\""),
        QRegularExpression::CaseInsensitiveOption);
    if (reRemember.match(out).hasMatch())
        out.replace(reRemember, QStringLiteral("\\1\"1\""));
    return out;
}

QString SteamAuth::buildLoginUsersVdf(const QString &steamId,
                                      const QString &login,
                                      const QString &persona,
                                      const QString &existing) const
{
    QString base;
    if (!existing.trimmed().isEmpty() && !steamId.isEmpty() && existing.contains(steamId)) {
        base = existing;
    } else {
        const QString id = steamId.isEmpty() ? QStringLiteral("0") : steamId;
        const QString name = persona.isEmpty() ? login : persona;
        const QString ts = QString::number(QDateTime::currentSecsSinceEpoch());

        base = QString(
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
    return forceSilentLoginUsersFlags(base);
}

static bool steamImageRunning(const QString &image)
{
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (QString::fromWCharArray(pe.szExeFile).compare(image, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    Q_UNUSED(image);
    return false;
#endif
}

static void steamSilentKill(const QString &image)
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

void SteamAuth::killLauncher()
{
    steamSilentKill(QStringLiteral("steam.exe"));
    steamSilentKill(QStringLiteral("steamwebhelper.exe"));
    steamSilentKill(QStringLiteral("steamservice.exe"));
    steamSilentKill(QStringLiteral("steamerrorreporter.exe"));
}

void SteamAuth::killSteamAndWait(int timeoutMs)
{
    killLauncher();
#ifdef Q_OS_WIN
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const bool alive = steamImageRunning(QStringLiteral("steam.exe"))
                           || steamImageRunning(QStringLiteral("steamwebhelper.exe"));
        if (!alive) {
            qWarning() << "[STEAM] killSteamAndWait: процессы Steam завершены";
            // Короткая пауза — отпустить файловые хендлы
            QThread::msleep(300);
            return;
        }
        killLauncher();
        QThread::msleep(250);
    }
    qWarning() << "[STEAM] WARN: killSteamAndWait timeout — Steam ещё жив, wipe всё равно";
#else
    Q_UNUSED(timeoutMs);
#endif
}

QString SteamAuth::sanitizeLoginUsersForLogout(const QString &vdf)
{
    if (vdf.trimmed().isEmpty())
        return vdf;
    QString out = vdf;
    auto zeroKey = [&out](const QString &key) {
        // "RememberPassword"\t\t"1" / "true" → "0"
        const QRegularExpression re(
            QStringLiteral("(\"%1\"\\s+)\"[^\"]*\"").arg(QRegularExpression::escape(key)),
            QRegularExpression::CaseInsensitiveOption);
        out.replace(re, QStringLiteral("\\1\"0\""));
    };
    zeroKey(QStringLiteral("RememberPassword"));
    zeroKey(QStringLiteral("AllowAutoLogin"));
    zeroKey(QStringLiteral("MostRecent"));
    return out;
}

QString SteamAuth::sanitizeConfigVdfForLogout(const QString &vdf)
{
    if (vdf.trimmed().isEmpty())
        return vdf;
    QString out = vdf;
    // Ключи автологина в InstallConfigStore / Software/Valve/Steam
    auto clearKey = [&out](const QString &key) {
        const QRegularExpression re(
            QStringLiteral("\"%1\"\\s+\"[^\"]*\"\\s*")
                .arg(QRegularExpression::escape(key)),
            QRegularExpression::CaseInsensitiveOption);
        out.remove(re);
    };
    clearKey(QStringLiteral("AutoLoginUser"));
    clearKey(QStringLiteral("AlreadyDoneUser"));
    clearKey(QStringLiteral("RememberPassword"));

    // ConnectCache блоки (JWT / auth tickets для silent login)
    const QRegularExpression connectBlock(
        QStringLiteral("\"ConnectCache\"\\s*\\{[^}]*\\}"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    out.remove(connectBlock);
    // Одиночные ConnectCache-значения на случай плоского формата
    const QRegularExpression connectLine(
        QStringLiteral("\"ConnectCache\"\\s+\"[^\"]*\"\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    out.remove(connectLine);
    return out;
}

void SteamAuth::wipePersonalSession()
{
    QStringList cleared;
    const QString steamPath = steamInstallPath();
    const QString configDir = steamPath + QStringLiteral("/config");

#ifdef Q_OS_WIN
    {
        QSettings steamReg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                           QSettings::NativeFormat);
        if (steamReg.contains(QStringLiteral("AutoLoginUser"))) {
            steamReg.remove(QStringLiteral("AutoLoginUser"));
            cleared << QStringLiteral("reg:AutoLoginUser");
        }
        if (steamReg.contains(QStringLiteral("AlreadyDoneUser"))) {
            steamReg.remove(QStringLiteral("AlreadyDoneUser"));
            cleared << QStringLiteral("reg:AlreadyDoneUser");
        }
        steamReg.setValue(QStringLiteral("RememberPassword"), 0);
        cleared << QStringLiteral("reg:RememberPassword=0");
    }
#endif

    const QString loginUsersPath = configDir + QStringLiteral("/loginusers.vdf");
    {
        const QString raw = readTextFile(loginUsersPath);
        if (!raw.isEmpty()) {
            const QString sanitized = sanitizeLoginUsersForLogout(raw);
            if (sanitized != raw && writeTextFile(loginUsersPath, sanitized))
                cleared << QStringLiteral("loginusers.vdf:MostRecent/RememberPassword/AllowAutoLogin→0");
            else if (sanitized == raw)
                cleared << QStringLiteral("loginusers.vdf:already-clean-or-no-flags");
        }
    }

    const QString configVdfPath = configDir + QStringLiteral("/config.vdf");
    {
        const QString raw = readTextFile(configVdfPath);
        if (!raw.isEmpty()) {
            const QString sanitized = sanitizeConfigVdfForLogout(raw);
            if (sanitized != raw && writeTextFile(configVdfPath, sanitized))
                cleared << QStringLiteral("config.vdf:AutoLogin/AlreadyDone/ConnectCache");
            else if (sanitized == raw)
                cleared << QStringLiteral("config.vdf:no-autologin-keys");
        }
    }

    // Machine-auth / connect-cache tokens (club silent path)
    auto removeIfExists = [&cleared](const QString &path, const QString &label) {
        if (!QFile::exists(path))
            return;
        if (QFile::remove(path))
            cleared << label;
        else
            cleared << (label + QStringLiteral(":FAILED"));
    };
    removeIfExists(configDir + QStringLiteral("/local.vdf"), QStringLiteral("config/local.vdf"));
    const QString appLocal = localAppDataSteamVdfPath();
    if (!appLocal.isEmpty())
        removeIfExists(appLocal, QStringLiteral("%LOCALAPPDATA%/Steam/local.vdf"));

    // Steam Guard machine auth files (ssfn*)
    QDir steamRoot(steamPath);
    if (steamRoot.exists()) {
        const QFileInfoList ssfns = steamRoot.entryInfoList(
            QStringList{QStringLiteral("ssfn*")},
            QDir::Files | QDir::Hidden | QDir::System);
        for (const QFileInfo &fi : ssfns) {
            if (QFile::remove(fi.absoluteFilePath()))
                cleared << (QStringLiteral("ssfn:") + fi.fileName());
            else
                cleared << (QStringLiteral("ssfn:") + fi.fileName() + QStringLiteral(":FAILED"));
        }
    }

    qWarning().noquote() << "[STEAM] personal: full logout wipe |"
                         << (cleared.isEmpty() ? QStringLiteral("(nothing found)")
                                               : cleared.join(QStringLiteral(", ")));
}

bool SteamAuth::applyCache(const QJsonObject &authData)
{
    const QString login = authData.value(QStringLiteral("login")).toString();
    const QString steamId = authData.value(QStringLiteral("steam_id")).toString();
    const QString persona = authData.value(QStringLiteral("persona_name")).toString(login);
    const QJsonObject authMeta = authData.value(QStringLiteral("auth")).toObject();
    const QString mode = authMeta.value(QStringLiteral("mode")).toString();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    const bool personal = mode.compare(QStringLiteral("personal"), Qt::CaseInsensitive) == 0
                          || platformSource.compare(QStringLiteral("personal_account"),
                                                    Qt::CaseInsensitive) == 0
                          || login.isEmpty();
    m_personalLaunch = personal;
    m_cacheApplied = false;

    // Личный Steam: полный logout wipe → окно входа (не silent club session)
    if (personal) {
        qWarning() << "[STEAM] applyCache: personal — kill Steam + full logout wipe";
        killSteamAndWait(8000);
        wipePersonalSession();
        // Повторный wipe если файлы были залочены
        if (QFile::exists(steamInstallPath() + QStringLiteral("/config/local.vdf"))
            || QFile::exists(localAppDataSteamVdfPath())) {
            qWarning() << "[STEAM] personal: local.vdf ещё на диске — kill+wipe retry";
            killSteamAndWait(4000);
            wipePersonalSession();
        }
        m_needBackup = false;
        return true;
    }

    // Club: wait until Steam releases config locks before writing machine-cache
    killSteamAndWait(6000);

    QJsonObject vdf = authData.value(QStringLiteral("vdf_files")).toObject();
    if (vdf.isEmpty()) {
        const QJsonObject auth = authMeta;
        const QJsonObject cache = auth.value(QStringLiteral("cache")).toObject();
        if (cache.contains(QStringLiteral("vdf_files")))
            vdf = cache.value(QStringLiteral("vdf_files")).toObject();
        else
            vdf = cache;
    }

    const QString steamPath = steamInstallPath();
    const QString configDir = steamPath + QStringLiteral("/config");
    QDir().mkpath(configDir);

    const QString configVdfRaw = vdf.value(QStringLiteral("config_vdf")).toString();
    const QString configVdf = ensureConfigAutoLoginUser(configVdfRaw, login);
    bool wroteConfig = true;
    if (!configVdf.isEmpty())
        wroteConfig = writeTextFile(configDir + QStringLiteral("/config.vdf"), configVdf);

    const QString loginUsers = buildLoginUsersVdf(
        steamId, login, persona, vdf.value(QStringLiteral("loginusers_vdf")).toString()
    );
    const bool wroteLoginUsers =
        writeTextFile(configDir + QStringLiteral("/loginusers.vdf"), loginUsers);

#ifdef Q_OS_WIN
    if (!login.isEmpty()) {
        QSettings steamReg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                           QSettings::NativeFormat);
        steamReg.setValue(QStringLiteral("AutoLoginUser"), login);
        steamReg.setValue(QStringLiteral("RememberPassword"), 1);
        qWarning() << "[STEAM] applyCache: reg AutoLoginUser=" << login
                   << "RememberPassword=1";
    }
#endif

    const QString cachedLocal = vdf.value(QStringLiteral("local_vdf")).toString().trimmed();
    bool wroteLocal = false;
    if (!cachedLocal.isEmpty()) {
        wroteLocal = writeTextFile(configDir + QStringLiteral("/local.vdf"), cachedLocal);
        const QString appLocal = localAppDataSteamVdfPath();
        if (!appLocal.isEmpty())
            wroteLocal = writeTextFile(appLocal, cachedLocal) && wroteLocal;
        if (PathResolver *paths = PathResolver::instance())
            paths->persistFile(appLocal, QStringLiteral("steam/local.vdf"));
    }

    qWarning().noquote() << "[STEAM] applyCache club | login:" << login
                         << "| steamId:" << (steamId.isEmpty() ? QStringLiteral("(empty)") : steamId)
                         << "| mode:" << (mode.isEmpty() ? QStringLiteral("(n/a)") : mode)
                         << "| config_vdf:" << (!configVdf.isEmpty() ? QStringLiteral("yes") : QStringLiteral("no"))
                         << "| loginusers:" << (wroteLoginUsers ? QStringLiteral("ok") : QStringLiteral("FAIL"))
                         << "| config_write:" << (wroteConfig ? QStringLiteral("ok") : QStringLiteral("skip/fail"))
                         << "| local_vdf bytes:" << cachedLocal.size()
                         << "| local_write:" << (wroteLocal ? QStringLiteral("ok") : QStringLiteral("FAIL/empty"));

    if (wroteLocal && wroteLoginUsers) {
        m_cacheApplied = true;
        m_needBackup = false;
        qWarning() << "[STEAM] machine-cache applied — expect silent AutoLogin (scout waits before typing)";
        return true;
    }

    m_cacheApplied = false;
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
    const QString mode = authData.value(QStringLiteral("auth")).toObject()
                             .value(QStringLiteral("mode")).toString();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    const bool personal = m_personalLaunch
        || mode.compare(QStringLiteral("personal"), Qt::CaseInsensitive) == 0
        || platformSource.compare(QStringLiteral("personal_account"), Qt::CaseInsensitive) == 0
        || authData.value(QStringLiteral("login")).toString().trimmed().isEmpty();
    m_personalLaunch = personal;

    QStringList args;
    const bool hasApp = !appId.isEmpty() && appId != QStringLiteral("0");
    if (hasApp)
        args << QStringLiteral("-applaunch") << appId;

    // Personal: без -silent — иначе окно логина прячется / Steam silent-логинится в фон.
    const QStringList passthrough = personal
        ? QStringList{QStringLiteral("-novid"), QStringLiteral("-nojoy"),
                      QStringLiteral("-shutdown")}
        : QStringList{QStringLiteral("-novid"), QStringLiteral("-nojoy"),
                      QStringLiteral("-silent"), QStringLiteral("-shutdown")};
    for (const QString &flag : passthrough) {
        if (argsStr.contains(flag, Qt::CaseInsensitive) && !args.contains(flag))
            args << flag;
    }
    // -shutdown только с игрой: иначе «чистый» Steam стартует и сразу гаснет → чёрные окна
    if (hasApp && !args.contains(QStringLiteral("-shutdown")))
        args << QStringLiteral("-shutdown");
    // Личный вход: явно не тащим -silent из args продукта
    if (personal)
        args.removeAll(QStringLiteral("-silent"));

    process->setWorkingDirectory(steamPath);
    qWarning() << "[STEAM] start:" << steamPath + QStringLiteral("/steam.exe") << args
               << (personal ? "| personal (login UI, no -silent)" : "| club");
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

    // Watch login UI even without password when cache applied (confirm silent success).
    // Interactive typing requires credentials.
    if (login.isEmpty() && password.isEmpty() && !m_cacheApplied) {
        qWarning() << "[STEAM] Scout пропущен (личный аккаунт / нет credentials)";
        return;
    }

    m_scoutTicks = 0;
    m_scoutInjectTick = 0;
    m_loginUiSeenTick = 0;
    m_scoutInjected = false;
    m_scoutAccountConfirmed = false;

    // Cache path: Steam often shows "Войти в Steam" for many seconds while token refresh
    // succeeds — do NOT type password until login UI stays up past this grace.
    // ~350ms * 70 ≈ 24.5s after first login HWND. No-cache: ~4.2s (tick 12).
    const int injectAfterTicks = m_cacheApplied ? 70 : 12;
    const bool canTypeCredentials = !login.isEmpty() && !password.isEmpty();

    if (m_cacheApplied) {
        qWarning() << "[STEAM] Scout: cache applied — wait for silent AutoLogin"
                   << "(interactive only if login UI persists ~"
                   << (injectAfterTicks * 350 / 1000) << "s)";
    }

    m_authScoutTimer = new QTimer(this);
    m_authScoutTimer->setInterval(350);

    connect(m_authScoutTimer, &QTimer::timeout, this,
            [this, login, password, injectAfterTicks, canTypeCredentials]() {
        if (!m_authScoutTimer)
            return;

        ++m_scoutTicks;

        if (m_scoutTicks > 300) {
            if (!m_scoutInjected) {
                if (m_cacheApplied)
                    qWarning() << "[STEAM] silent login OK (cache) — scout timeout, no password typed";
                else
                    qWarning() << "[STEAM] Scout timeout — окно входа не найдено";
            } else if (!m_scoutAccountConfirmed) {
                qWarning() << "[STEAM] Scout timeout — окно выбора аккаунта не подтверждено";
            }
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
        if (ctx.loginHwnd || ctx.pickerHwnd || ctx.steamDlgHwnd) {
            if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                pm->setShellTopmost(true);
        }

        // Silent success: login UI appeared then vanished without us typing.
        if (m_cacheApplied && !m_scoutInjected && m_loginUiSeenTick > 0 && !ctx.loginHwnd
            && !ctx.pickerHwnd && !ctx.steamDlgHwnd
            && m_scoutTicks >= m_loginUiSeenTick + 8) {
            qWarning() << "[STEAM] silent login OK (cache) — login UI closed without typing";
            m_needBackup = false;
            stopScout();
            return;
        }

        // Silent success: never saw login UI after Steam had time to settle.
        if (m_cacheApplied && !m_scoutInjected && m_loginUiSeenTick == 0
            && !ctx.loginHwnd && !ctx.pickerHwnd && m_scoutTicks >= 45) {
            qWarning() << "[STEAM] silent login OK (cache) — no login UI";
            m_needBackup = false;
            stopScout();
            return;
        }

        if (ctx.loginHwnd && m_loginUiSeenTick == 0) {
            m_loginUiSeenTick = m_scoutTicks;
            if (m_cacheApplied) {
                qWarning() << "[STEAM] login UI seen during cache wait — holding off typing:"
                           << ctx.loginTitle;
            }
        }

        const bool loginUiPersisted = ctx.loginHwnd && m_loginUiSeenTick > 0
            && (m_scoutTicks - m_loginUiSeenTick + 1) >= injectAfterTicks;

        if (!m_scoutInjected && loginUiPersisted) {
            if (!canTypeCredentials) {
                if (m_cacheApplied && (m_scoutTicks % 20 == 0)) {
                    qWarning() << "[STEAM] cache miss but no credentials — cannot interactive fallback";
                }
                return;
            }

            m_scoutInjected = true;
            m_scoutInjectTick = m_scoutTicks;
            HWND authHwnd = ctx.loginHwnd;
            if (m_cacheApplied) {
                qWarning() << "[STEAM] cache miss → interactive fallback (login UI still up):"
                           << ctx.loginTitle;
            } else {
                qWarning() << "[STEAM] Интерактивный логин (off-screen):" << ctx.loginTitle;
            }

            // Defocus/clear shell search before Ctrl+V credentials (SendInput leak guard).
            if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                pm->requestClearGameSearch();

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
                if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                    pm->requestClearGameSearch();
                parkWindowOffscreen(authHwnd);
                pasteText(login);
                keybd_event(VK_TAB, 0, 0, 0);
                keybd_event(VK_TAB, 0, KEYEVENTF_KEYUP, 0);

                QTimer::singleShot(300, this, [this, authHwnd, password, pasteText]() {
                    if (!m_authScoutTimer)
                        return;
                    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
                        pm->requestClearGameSearch();
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
            // With cache: don't confirm picker early — AutoLogin may dismiss it; wait like login UI.
            const int pickerAfter = m_cacheApplied ? injectAfterTicks : 12;
            const bool pickerOnly = !m_scoutInjected && ctx.pickerHwnd
                && m_loginUiSeenTick == 0
                && m_scoutTicks >= pickerAfter;
            const bool steamDlgOnly = !m_scoutInjected && !ctx.loginHwnd && ctx.steamDlgHwnd
                                      && m_scoutTicks >= (m_cacheApplied ? injectAfterTicks : 16);
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

void SteamAuth::backupCache(NetworkManager *net, int terminalId, const QString &login,
                            int accountId, int gameId)
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
    if (PathResolver *paths = PathResolver::instance())
        paths->persistFile(localAppDataSteamVdfPath(), QStringLiteral("steam/local.vdf"));

    QJsonObject rootPayload;
    rootPayload.insert(QStringLiteral("login"), login);
    rootPayload.insert(QStringLiteral("terminal_id"), terminalId);
    if (accountId > 0)
        rootPayload.insert(QStringLiteral("account_id"), accountId);
    if (gameId > 0)
        rootPayload.insert(QStringLiteral("game_id"), gameId);
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
    qDebug() << "[STEAM] VDF backup → server, bytes:" << jsonData.size()
             << "account_id:" << accountId << "game_id:" << gameId;

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
    Q_UNUSED(accountId);
    Q_UNUSED(gameId);
#endif
}
