#include "patchcachecoordinator.h"

#include "localhttpserver.h"
#include "networkmanager.h"
#include "pathresolver.h"
#include "stationhealth.h"

#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace {

QString sanitizeRel(QString rel)
{
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (rel.startsWith(QLatin1Char('/')))
        rel = rel.mid(1);
    if (rel.contains(QLatin1String("..")))
        return {};
    return rel;
}

} // namespace

PatchCacheCoordinator::PatchCacheCoordinator(NetworkManager *net, QObject *parent)
    : QObject(parent)
    , m_net(net)
{
    loadConfig();
}

PatchCacheCoordinator::~PatchCacheCoordinator()
{
    stopIngest(QStringLiteral("destroy"));
    ensureSeedServer(false);
}

void PatchCacheCoordinator::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("PatchCache/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_port = qBound(1024, s.value(QStringLiteral("PatchCache/port"), 8745).toInt(), 65000);
    m_maxFiles = qBound(50, s.value(QStringLiteral("PatchCache/max_files"), 400).toInt(), 2000);
    m_maxFileBytes = qMax(qint64(8 * 1024 * 1024),
                          s.value(QStringLiteral("PatchCache/max_file_mb"), 512).toLongLong() * 1024 * 1024);
    const QString ingestRaw = s.value(QStringLiteral("PatchCache/ingest"), true).toString().trimmed().toLower();
    m_ingestEnabled = !(ingestRaw == QLatin1String("0") || ingestRaw == QLatin1String("false")
                        || ingestRaw == QLatin1String("no"));
    m_steamcmdBin = s.value(QStringLiteral("PatchCache/steamcmd")).toString().trimmed();
}

int PatchCacheCoordinator::seedPort() const
{
    return m_server && m_server->isListening() ? int(m_server->serverPort()) : 0;
}

bool PatchCacheCoordinator::seedActive() const
{
    return seedPort() > 0;
}

bool PatchCacheCoordinator::ingestActive() const
{
    return m_ingestBusy || m_ingestWant;
}

void PatchCacheCoordinator::applyIngestPolicy(const QJsonObject &ingest)
{
    if (!m_enabled || !m_ingestEnabled || ingest.isEmpty())
        return;
    if (m_net && !m_net->featureEnabled(QStringLiteral("patch_cache"))) {
        if (m_ingestBusy || m_startedSteam)
            stopIngest(QStringLiteral("feature_off"));
        m_ingestWant = false;
        return;
    }
    const bool want = ingest.value(QStringLiteral("enabled")).toBool(false)
        || ingest.value(QStringLiteral("want")).toBool(false);
    if (m_net && m_net->isGuestSessionActive()) {
        if (m_ingestBusy || m_startedSteam)
            stopIngest(QStringLiteral("guest"));
        m_ingestWant = false;
        return;
    }
    m_ingestWant = want;
    if (want)
        startIngest();
    else
        stopIngest(QStringLiteral("window closed"));
}

QString PatchCacheCoordinator::lanIp() const
{
    return m_net ? m_net->primaryLanIp() : QString();
}

void PatchCacheCoordinator::applySeedPolicy(const QJsonObject &seed)
{
    if (!m_enabled || seed.isEmpty())
        return;
    if (m_net && !m_net->featureEnabled(QStringLiteral("patch_cache"))) {
        ensureSeedServer(false);
        return;
    }
    const bool want = seed.value(QStringLiteral("enabled")).toBool(false)
        || seed.value(QStringLiteral("want")).toBool(false);
    const int port = seed.value(QStringLiteral("port")).toInt(m_port);
    if (port > 0)
        m_port = port;
    ensureSeedServer(want);
}

void PatchCacheCoordinator::applyPullCommand(const QJsonObject &pull)
{
    if (!m_enabled || pull.isEmpty() || m_pullBusy)
        return;
    if (m_net && !m_net->featureEnabled(QStringLiteral("patch_cache")))
        return;
    const qint64 id = pull.value(QStringLiteral("command_id")).toInteger();
    const QJsonArray apps = pull.value(QStringLiteral("apps")).toArray();
    if (id <= 0 || apps.isEmpty())
        return;
    if (id == m_lastPullAckId)
        return;
    if (m_net && m_net->isGuestSessionActive()) {
        ackPull(id, QStringLiteral("busy_session"),
                QStringLiteral("Гость на месте — LAN pull отложен"));
        return;
    }
    startPull(id, apps);
}

void PatchCacheCoordinator::ackPull(qint64 commandId, const QString &result, const QString &message)
{
    m_lastPullAckId = commandId;
    if (m_net)
        m_net->ackPatchPull(commandId, result, message);
}

QString PatchCacheCoordinator::steamInstallDir(const QString &appId) const
{
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    const QString acf = QDir(steam).filePath(
        QStringLiteral("steamapps/appmanifest_%1.acf").arg(appId));
    QFile f(acf);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    static const QRegularExpression re(QStringLiteral("\"installdir\"\\s+\"([^\"]+)\""));
    const auto m = re.match(text);
    if (!m.hasMatch())
        return {};
    return QDir(steam).filePath(QStringLiteral("steamapps/common/") + m.captured(1));
}

QString PatchCacheCoordinator::resolveInstallRoot(const QString &platform, const QString &appId) const
{
    if (platform == QLatin1String("steam"))
        return steamInstallDir(appId);
    if (platform == QLatin1String("epic")) {
        // Epic: ищем .item с AppName и берём InstallLocation.
        const QString programData = qEnvironmentVariable("PROGRAMDATA");
        const QString manifests = programData.isEmpty()
            ? QString()
            : QDir(programData).filePath(QStringLiteral("Epic/EpicGamesLauncher/Data/Manifests"));
        QDir dir(manifests);
        if (!dir.exists())
            return {};
        for (const QString &name : dir.entryList({QStringLiteral("*.item")}, QDir::Files)) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            const auto doc = QJsonDocument::fromJson(f.readAll());
            f.close();
            if (!doc.isObject())
                continue;
            const QJsonObject o = doc.object();
            const QString id = o.value(QStringLiteral("AppName")).toString();
            if (id.compare(appId, Qt::CaseInsensitive) != 0)
                continue;
            return o.value(QStringLiteral("InstallLocation")).toString();
        }
    }
    return {};
}

QJsonArray PatchCacheCoordinator::buildManifest(const QString &platform, const QString &appId) const
{
    QJsonArray out;
    const QString root = resolveInstallRoot(platform, appId);
    if (root.isEmpty() || !QDir(root).exists())
        return out;

    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    int n = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.size() <= 0 || fi.size() > m_maxFileBytes)
            continue;
        QString rel = QDir(root).relativeFilePath(fi.absoluteFilePath());
        rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QJsonObject row;
        row.insert(QStringLiteral("path"), rel);
        row.insert(QStringLiteral("size"), fi.size());
        row.insert(QStringLiteral("mtime"), fi.lastModified().toSecsSinceEpoch());
        out.append(row);
        if (++n >= m_maxFiles)
            break;
    }
    return out;
}

LocalHttpResponse PatchCacheCoordinator::handleSeedRequest(const LocalHttpRequest &req) const
{
    LocalHttpResponse res;
    if (req.method != QLatin1String("GET")) {
        res.status = 405;
        res.body = "{\"error\":\"method\"}";
        return res;
    }
    if (req.path == QLatin1String("/health") || req.path == QLatin1String("/")) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("seed"), true);
        o.insert(QStringLiteral("pc"), m_net ? m_net->getCurrentPcName() : QString());
        res.body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        return res;
    }
    if (req.path == QLatin1String("/manifest")) {
        // Query string уже срезан LocalHttpServer — парсим из raw не можем.
        // Клиент передаёт /manifest/steam/730
        res.status = 400;
        res.body = "{\"error\":\"use /manifest/{platform}/{appId}\"}";
        return res;
    }
    if (req.path.startsWith(QLatin1String("/manifest/"))) {
        const QStringList parts = req.path.mid(10).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() < 2) {
            res.status = 400;
            res.body = "{\"error\":\"platform/appId\"}";
            return res;
        }
        const QString platform = parts.at(0).toLower();
        const QString appId = parts.at(1);
        QJsonObject o;
        o.insert(QStringLiteral("p"), platform);
        o.insert(QStringLiteral("id"), appId);
        o.insert(QStringLiteral("files"), buildManifest(platform, appId));
        res.body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        return res;
    }
    if (req.path.startsWith(QLatin1String("/file/"))) {
        // /file/{platform}/{appId}/{rel...}
        const QString rest = req.path.mid(6);
        const int p1 = rest.indexOf(QLatin1Char('/'));
        const int p2 = p1 > 0 ? rest.indexOf(QLatin1Char('/'), p1 + 1) : -1;
        if (p1 <= 0 || p2 <= p1) {
            res.status = 400;
            res.body = "{\"error\":\"path\"}";
            return res;
        }
        const QString platform = rest.left(p1).toLower();
        const QString appId = rest.mid(p1 + 1, p2 - p1 - 1);
        const QString rel = sanitizeRel(QUrl::fromPercentEncoding(rest.mid(p2 + 1).toUtf8()));
        if (rel.isEmpty()) {
            res.status = 400;
            res.body = "{\"error\":\"bad rel\"}";
            return res;
        }
        const QString root = resolveInstallRoot(platform, appId);
        if (root.isEmpty()) {
            res.status = 404;
            res.body = "{\"error\":\"no install\"}";
            return res;
        }
        const QString abs = QDir(root).filePath(rel);
        const QFileInfo fi(abs);
        if (!fi.exists() || !fi.isFile() || !fi.absoluteFilePath().startsWith(QFileInfo(root).absoluteFilePath())) {
            res.status = 404;
            res.body = "{\"error\":\"missing\"}";
            return res;
        }
        if (fi.size() > m_maxFileBytes) {
            res.status = 413;
            res.body = "{\"error\":\"too large\"}";
            return res;
        }
        res.contentType = "application/octet-stream";
        res.filePath = fi.absoluteFilePath();
        res.body.clear();
        return res;
    }
    res.status = 404;
    res.body = "{\"error\":\"not found\"}";
    return res;
}

void PatchCacheCoordinator::ensureSeedServer(bool want)
{
    if (!want) {
        if (m_server) {
            m_server->close();
            m_server.reset();
            qWarning() << "[PATCH-CACHE] seed stopped";
        }
        return;
    }
    if (m_server && m_server->isListening() && m_server->serverPort() == quint16(m_port))
        return;
    m_server = std::make_unique<LocalHttpServer>(this);
    m_server->setHandler([this](const LocalHttpRequest &req) {
        return handleSeedRequest(req);
    });
    if (!m_server->listenAny(quint16(m_port))) {
        qWarning() << "[PATCH-CACHE] seed listen failed on" << m_port;
        m_server.reset();
        return;
    }
    qWarning() << "[PATCH-CACHE] seed listening on" << m_server->serverPort()
               << "ip" << lanIp();
}

void PatchCacheCoordinator::startPull(qint64 commandId, const QJsonArray &apps)
{
    m_pullBusy = true;
    ackPull(commandId, QStringLiteral("running"),
            QStringLiteral("LAN pull ") + QString::number(apps.size()) + QStringLiteral(" app(s)"));

    // Асинхронно, чтобы не блокировать heartbeat-поток надолго в одном тике.
    QTimer::singleShot(0, this, [this, commandId, apps]() {
        int ok = 0;
        QString lastErr;
        for (const QJsonValue &v : apps) {
            if (!v.isObject())
                continue;
            QString err;
            if (pullOneApp(v.toObject(), &err))
                ++ok;
            else
                lastErr = err;
        }
        const bool all = ok == apps.size();
        ackPull(commandId,
                all ? QStringLiteral("ok") : QStringLiteral("partial"),
                QStringLiteral("Стянуто ") + QString::number(ok) + QLatin1Char('/')
                    + QString::number(apps.size())
                    + (lastErr.isEmpty() ? QString() : (QStringLiteral(" · ") + lastErr.left(120))));
        m_pullBusy = false;
        qWarning() << "[PATCH-CACHE] pull done" << ok << "/" << apps.size() << lastErr;
    });
}

bool PatchCacheCoordinator::httpGetJson(const QString &url, QJsonObject *out, QString *err)
{
    if (!m_net || !out)
        return false;
    QNetworkAccessManager *nam = m_net->networkAccessManager();
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(30000);
    QNetworkReply *reply = nam->get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) {
        if (err)
            *err = reply->errorString();
        reply->deleteLater();
        return false;
    }
    const auto doc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();
    if (!doc.isObject()) {
        if (err)
            *err = QStringLiteral("bad json");
        return false;
    }
    *out = doc.object();
    return true;
}

bool PatchCacheCoordinator::httpGetFile(const QString &url, const QString &dest, QString *err)
{
    if (!m_net)
        return false;
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QNetworkAccessManager *nam = m_net->networkAccessManager();
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(300000);
    QNetworkReply *reply = nam->get(req);
    QFile f(dest + QStringLiteral(".part"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("write fail");
        reply->abort();
        reply->deleteLater();
        return false;
    }
    QObject::connect(reply, &QNetworkReply::readyRead, &f, [&]() {
        f.write(reply->readAll());
    });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    f.write(reply->readAll());
    f.close();
    const bool ok = reply->error() == QNetworkReply::NoError;
    if (!ok && err)
        *err = reply->errorString();
    reply->deleteLater();
    if (!ok) {
        QFile::remove(dest + QStringLiteral(".part"));
        return false;
    }
    if (QFileInfo::exists(dest))
        QFile::remove(dest);
    if (!QFile::rename(dest + QStringLiteral(".part"), dest)) {
        if (err)
            *err = QStringLiteral("rename fail");
        return false;
    }
    return true;
}

bool PatchCacheCoordinator::pullOneApp(const QJsonObject &app, QString *err)
{
    const QString platform = app.value(QStringLiteral("p")).toString().toLower();
    const QString appId = app.value(QStringLiteral("id")).toString();
    const QString peerIp = app.value(QStringLiteral("peer_ip")).toString();
    const int peerPort = app.value(QStringLiteral("peer_port")).toInt();
    if (platform.isEmpty() || appId.isEmpty() || peerIp.isEmpty() || peerPort <= 0) {
        if (err)
            *err = QStringLiteral("bad app descriptor");
        return false;
    }

    const QString base = QStringLiteral("http://%1:%2").arg(peerIp).arg(peerPort);
    QJsonObject manifest;
    if (!httpGetJson(base + QStringLiteral("/manifest/") + platform + QLatin1Char('/') + appId,
                     &manifest, err)) {
        return false;
    }

    const QString root = resolveInstallRoot(platform, appId);
    if (root.isEmpty()) {
        if (err)
            *err = QStringLiteral("local install missing for ") + appId;
        return false;
    }

    const QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    int copied = 0;
    int checked = 0;
    for (const QJsonValue &v : files) {
        if (!v.isObject())
            continue;
        const QJsonObject f = v.toObject();
        const QString rel = sanitizeRel(f.value(QStringLiteral("path")).toString());
        if (rel.isEmpty())
            continue;
        const qint64 size = f.value(QStringLiteral("size")).toInteger();
        const qint64 mtime = f.value(QStringLiteral("mtime")).toInteger();
        const QString dest = QDir(root).filePath(rel);
        const QFileInfo local(dest);
        ++checked;
        if (local.exists() && local.size() == size
            && local.lastModified().toSecsSinceEpoch() >= mtime) {
            continue;
        }
        const QString url = base + QStringLiteral("/file/") + platform + QLatin1Char('/')
            + appId + QLatin1Char('/') + QString::fromUtf8(QUrl::toPercentEncoding(rel, "/"));
        QString fileErr;
        if (!httpGetFile(url, dest, &fileErr)) {
            if (err)
                *err = fileErr;
            continue;
        }
        ++copied;
    }
    qWarning() << "[PATCH-CACHE] app" << platform << appId << "checked" << checked
               << "copied" << copied;
    return copied > 0 || checked > 0;
}

QString PatchCacheCoordinator::steamExePath() const
{
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    const QString exe = QDir(steam).filePath(QStringLiteral("steam.exe"));
    return QFileInfo::exists(exe) ? exe : QString();
}

QString PatchCacheCoordinator::steamcmdPath() const
{
    QStringList candidates;
    if (!m_steamcmdBin.isEmpty())
        candidates << m_steamcmdBin;
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    candidates << QStringLiteral("D:/Tools/steamcmd.exe")
               << QStringLiteral("D:/Tools/steamcmd/steamcmd.exe")
               << QDir(steam).filePath(QStringLiteral("steamcmd.exe"))
               << QDir(steam).filePath(QStringLiteral("steamcmd/steamcmd.exe"));
    for (const QString &c : candidates) {
        if (!c.isEmpty() && QFileInfo::exists(c))
            return c;
    }
    return {};
}

bool PatchCacheCoordinator::processRunning(const QString &image) const
{
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe;
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

void PatchCacheCoordinator::setIngestStatus(const QString &result, const QString &message)
{
    m_ingestResult = result.left(32);
    m_ingestMessage = message.left(240);
}

void PatchCacheCoordinator::startIngest()
{
    if (m_ingestBusy)
        return;
    m_ingestBusy = true;
    m_ingestOk = 0;
    m_ingestTotal = 0;
    setIngestStatus(QStringLiteral("running"), QStringLiteral("Ночной ingest на D:"));
    qWarning() << "[PATCH-CACHE] night ingest start";
    startSteamSilent();
    queueSteamcmdApps();
    startNextSteamcmd();
    if (!m_steamcmd && m_ingestQueue.isEmpty()) {
        if (m_startedSteam || processRunning(QStringLiteral("steam.exe")))
            setIngestStatus(QStringLiteral("running"), QStringLiteral("Steam -silent, steamcmd нет"));
        else {
            setIngestStatus(QStringLiteral("error"), QStringLiteral("Нет steam.exe / steamcmd"));
            m_ingestBusy = false;
        }
    }
}

void PatchCacheCoordinator::stopIngest(const QString &reason)
{
    m_ingestWant = false;
    m_ingestQueue.clear();
    if (m_steamcmd) {
        m_steamcmd->disconnect();
        m_steamcmd->kill();
        m_steamcmd->deleteLater();
        m_steamcmd.release();
    }
    stopSteamIfOurs();
    if (m_ingestBusy || !reason.isEmpty())
        setIngestStatus(m_ingestOk > 0 ? QStringLiteral("ok") : QStringLiteral("idle"),
                        reason.left(240));
    m_ingestBusy = false;
    qWarning() << "[PATCH-CACHE] night ingest stop" << reason;
}

void PatchCacheCoordinator::startSteamSilent()
{
    const QString exe = steamExePath();
    if (exe.isEmpty())
        return;
    if (processRunning(QStringLiteral("steam.exe")))
        return;
    const QString work = QFileInfo(exe).absolutePath();
    const bool ok = QProcess::startDetached(exe,
                                            {QStringLiteral("-silent"), QStringLiteral("-no-browser")},
                                            work);
    m_startedSteam = ok;
    if (ok)
        qWarning() << "[PATCH-CACHE] steam -silent" << exe;
}

void PatchCacheCoordinator::stopSteamIfOurs()
{
    if (!m_startedSteam)
        return;
    const QString exe = steamExePath();
    if (!exe.isEmpty()) {
        QProcess::startDetached(exe, {QStringLiteral("-shutdown")}, QFileInfo(exe).absolutePath());
        qWarning() << "[PATCH-CACHE] steam -shutdown";
    }
    m_startedSteam = false;
}

void PatchCacheCoordinator::queueSteamcmdApps()
{
    m_ingestQueue.clear();
    if (steamcmdPath().isEmpty())
        return;
    PathResolver *paths = PathResolver::instance();
    const StationHealth::GameInventory inv = StationHealth::scanGames(
        paths ? paths->steamPath() : QStringLiteral("D:/Steam"),
        paths ? paths->epicPath() : QString());
    for (const auto &g : inv.games) {
        if (g.platform != QLatin1String("steam") || g.appId.isEmpty())
            continue;
        m_ingestQueue.append(g.appId);
        if (m_ingestQueue.size() >= 20)
            break;
    }
    m_ingestTotal = m_ingestQueue.size();
}

void PatchCacheCoordinator::startNextSteamcmd()
{
    if (!m_ingestWant || m_ingestQueue.isEmpty()) {
        if (m_ingestBusy && m_ingestTotal > 0 && m_ingestQueue.isEmpty() && !m_steamcmd) {
            setIngestStatus(QStringLiteral("running"),
                            QStringLiteral("steamcmd ") + QString::number(m_ingestOk)
                                + QLatin1Char('/') + QString::number(m_ingestTotal)
                                + QStringLiteral(" · Steam silent"));
        }
        return;
    }
    const QString cmd = steamcmdPath();
    if (cmd.isEmpty()) {
        m_ingestQueue.clear();
        return;
    }
    const QString appId = m_ingestQueue.takeFirst();
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    m_steamcmd = std::make_unique<QProcess>(this);
    m_steamcmd->setWorkingDirectory(QFileInfo(cmd).absolutePath());
    m_steamcmd->setProgram(cmd);
    m_steamcmd->setArguments({
        QStringLiteral("+force_install_dir"), steam,
        QStringLiteral("+login"), QStringLiteral("anonymous"),
        QStringLiteral("+app_update"), appId,
        QStringLiteral("+quit"),
    });
    QProcess *proc = m_steamcmd.get();
    connect(proc, &QProcess::finished, this, [this, appId](int code, QProcess::ExitStatus) {
        if (code == 0)
            ++m_ingestOk;
        qWarning() << "[PATCH-CACHE] steamcmd" << appId << "exit" << code;
        if (m_steamcmd) {
            m_steamcmd->deleteLater();
            m_steamcmd.release();
        }
        setIngestStatus(QStringLiteral("running"),
                        QStringLiteral("steamcmd ") + QString::number(m_ingestOk)
                            + QLatin1Char('/') + QString::number(m_ingestTotal)
                            + QStringLiteral(" · app ") + appId);
        QTimer::singleShot(500, this, [this]() { startNextSteamcmd(); });
    });
    m_steamcmd->start();
    qWarning() << "[PATCH-CACHE] steamcmd app_update" << appId;
}


