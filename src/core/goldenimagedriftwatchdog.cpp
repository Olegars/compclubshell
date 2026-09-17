#include "goldenimagedriftwatchdog.h"

#include "networkmanager.h"
#include "processmanager.h"
#include "pathresolver.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QCryptographicHash>
#include <QTimer>

namespace {

QString normPath(const QString &path)
{
    return QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()).toLower();
}

QString relFromRoot(const QString &root, const QString &abs)
{
    const QString r = normPath(root);
    const QString a = normPath(abs);
    if (a.startsWith(r + QLatin1Char('/')))
        return a.mid(r.size() + 1);
    return QFileInfo(abs).fileName();
}

QString localRelKey(const QString &abs)
{
    const QString a = normPath(abs);
    const int idx = a.indexOf(QStringLiteral("/steam/"));
    if (idx >= 0)
        return a.mid(idx + 1);
    const int g = a.indexOf(QStringLiteral("/games/"));
    if (g >= 0)
        return a.mid(g + 1);
    return QFileInfo(abs).fileName();
}

} // namespace

GoldenImageDriftWatchdog::GoldenImageDriftWatchdog(NetworkManager *net,
                                                   ProcessManager *launcher,
                                                   QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_launcher(launcher)
{
    Q_UNUSED(m_launcher);
    loadConfig();
    if (m_net) {
        connect(m_net, &NetworkManager::loginSucceeded, this, [this]() {
            QTimer::singleShot(0, this, &GoldenImageDriftWatchdog::snapshotAtSessionStart);
        });
        connect(m_net, &NetworkManager::resyncCommandReceived, this,
                [this](qint64 commandId, const QString &action) {
                    Q_UNUSED(action);
                    if (m_resyncBusy)
                        return;
                    if (m_sessionHashes.isEmpty())
                        snapshotAtSessionStart();
                    if (!m_templateLoaded) {
                        m_templateHashes = loadTemplateHashes();
                        m_templateLoaded = true;
                    }
                    const QStringList paths = diffPaths(m_sessionHashes, m_templateHashes);
                    if (paths.isEmpty()) {
                        m_net->ackResyncCommand(commandId, QStringLiteral("ok"),
                                                QStringLiteral("Drift не обнаружен"));
                        return;
                    }
                    runResync(paths, commandId);
                });
    }
}

void GoldenImageDriftWatchdog::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("Integrity/enabled"), true).toString().trimmed().toLower();
    m_enabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
    m_templateRoot = s.value(QStringLiteral("Integrity/template_root")).toString().trimmed();
    if (m_templateRoot.isEmpty()) {
        const QString disklessIp = s.value(QStringLiteral("Diskless/server_ip"), QStringLiteral("192.168.20.10")).toString().trimmed();
        m_templateRoot = QStringLiteral("//%1/GoldenImage").arg(disklessIp);
    }
    m_copyTool = s.value(QStringLiteral("Integrity/tool"), QStringLiteral("robocopy")).toString().trimmed().toLower();
    m_maxFiles = qBound(8, s.value(QStringLiteral("Integrity/max_files"), 64).toInt(), 256);
    m_maxResyncFiles = qBound(1, s.value(QStringLiteral("Integrity/max_resync_files"), 12).toInt(), 40);
}

QStringList GoldenImageDriftWatchdog::criticalPaths() const
{
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    const QString games = paths ? paths->gamesPath() : QStringLiteral("D:/Games");
    const QString riot = paths ? paths->riotPath() : QString();
    const QString ea = paths ? paths->eaPath() : QString();

    QStringList candidates = {
        steam + QStringLiteral("/steam.exe"),
        steam + QStringLiteral("/steamclient64.dll"),
        steam + QStringLiteral("/EasyAntiCheat/EasyAntiCheat.exe"),
        steam + QStringLiteral("/EasyAntiCheat/EasyAntiCheat_EOS.exe"),
        steam + QStringLiteral("/bin/steamservice.exe"),
        games + QStringLiteral("/Riot Games/Riot Client/RiotClientServices.exe"),
        games + QStringLiteral("/Riot Games/Riot Client/RiotClientUx.exe"),
        games + QStringLiteral("/Riot Games/VALORANT/live/VALORANT.exe"),
        games + QStringLiteral("/Epic Games/Launcher/Portal/Binaries/Win64/EpicGamesLauncher.exe"),
    };
    if (!riot.isEmpty()) {
        candidates << riot + QStringLiteral("/RiotClientServices.exe");
        candidates << riot + QStringLiteral("/VALORANT/live/VALORANT.exe");
    }
    if (!ea.isEmpty())
        candidates << ea + QStringLiteral("/EA Desktop/EADesktop.exe");

    QStringList out;
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p) && !out.contains(p))
            out << p;
        if (out.size() >= m_maxFiles)
            break;
    }
    return out;
}

QHash<QString, QString> GoldenImageDriftWatchdog::hashFiles(const QStringList &paths) const
{
    QHash<QString, QString> out;
    for (const QString &abs : paths) {
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        out.insert(normPath(abs),
                   QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex()));
    }
    return out;
}

QString GoldenImageDriftWatchdog::aggregateHash(const QHash<QString, QString> &hashes) const
{
    QStringList keys = hashes.keys();
    keys.sort();
    QCryptographicHash agg(QCryptographicHash::Sha256);
    for (const QString &k : keys)
        agg.addData(k.toUtf8() + '=' + hashes.value(k).toUtf8() + '\n');
    return QString::fromLatin1(agg.result().toHex());
}

QHash<QString, QString> GoldenImageDriftWatchdog::loadTemplateHashes() const
{
    QHash<QString, QString> map;
    const QString manifestPath = QDir(m_templateRoot).filePath(QStringLiteral("manifest.json"));
    QFile mf(manifestPath);
    if (mf.open(QIODevice::ReadOnly)) {
        const QJsonObject files = QJsonDocument::fromJson(mf.readAll()).object().value(QStringLiteral("files")).toObject();
        for (auto it = files.begin(); it != files.end(); ++it) {
            const QString rel = normPath(it.key());
            const QString hash = it.value().toString().trimmed().toLower();
            if (!hash.isEmpty())
                map.insert(rel, hash);
        }
        if (!map.isEmpty())
            return map;
    }

    if (!QDir(m_templateRoot).exists())
        return map;

    QDirIterator it(m_templateRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && map.size() < m_maxFiles) {
        const QString abs = it.next();
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        map.insert(normPath(relFromRoot(m_templateRoot, abs)),
                     QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex()));
    }
    return map;
}

QStringList GoldenImageDriftWatchdog::diffPaths(const QHash<QString, QString> &local,
                                                const QHash<QString, QString> &templateHashes) const
{
    QStringList drift;
    for (auto it = local.constBegin(); it != local.constEnd(); ++it) {
        const QString rel = localRelKey(it.key());
        const QString tplHash = templateHashes.value(normPath(rel));
        if (tplHash.isEmpty())
            continue;
        if (tplHash != it.value())
            drift << it.key();
        if (drift.size() >= m_maxResyncFiles)
            break;
    }
    return drift;
}

void GoldenImageDriftWatchdog::snapshotAtSessionStart()
{
    if (!m_enabled || !m_net)
        return;
    const QStringList paths = criticalPaths();
    m_sessionHashes = hashFiles(paths);
    if (!m_templateLoaded) {
        m_templateHashes = loadTemplateHashes();
        m_templateLoaded = true;
    }
    const QString agg = aggregateHash(m_sessionHashes);
    m_net->setIntegrityTelemetry(QStringLiteral("ok"), agg, QString(), {});
    qWarning() << "[INTEGRITY] snapshot" << m_sessionHashes.size() << "files hash" << agg.left(12);
}

bool GoldenImageDriftWatchdog::shouldCheckAfterFailure(const QString &reason) const
{
    return reason.contains(QStringLiteral("launcher failed"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("launcher exited"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("launcher closed"), Qt::CaseInsensitive);
}

void GoldenImageDriftWatchdog::onGameSessionFinished(const QString &reason)
{
    if (!m_enabled || !m_net || !shouldCheckAfterFailure(reason))
        return;
    if (m_sessionHashes.isEmpty())
        snapshotAtSessionStart();
    if (!m_templateLoaded) {
        m_templateHashes = loadTemplateHashes();
        m_templateLoaded = true;
    }
    const QStringList drift = diffPaths(m_sessionHashes, m_templateHashes);
    if (drift.isEmpty())
        return;
    reportDrift(drift, reason);
}

void GoldenImageDriftWatchdog::reportDrift(const QStringList &paths, const QString &trigger)
{
    const QString pc = m_net->getCurrentPcName();
    const QString msg = QStringLiteral("%1 файл(ов) не совпали с эталоном (%2)")
                            .arg(paths.size())
                            .arg(trigger);
    QJsonArray arr;
    for (const QString &p : paths)
        arr.append(p);

    m_net->setIntegrityTelemetry(QStringLiteral("drift"),
                                 aggregateHash(m_sessionHashes),
                                 msg,
                                 paths);
    m_net->reportShellIncident(QStringLiteral("golden_image_drift"),
                               QStringLiteral("high"),
                               QStringLiteral("На %1 повреждены файлы игрового диска — нужен тихий re-sync").arg(pc),
                               QJsonObject{
                                   {QStringLiteral("trigger"), trigger},
                                   {QStringLiteral("files"), arr},
                               });
    qWarning() << "[INTEGRITY] drift" << paths.size() << trigger;
}

bool GoldenImageDriftWatchdog::copyFromTemplate(const QString &relPath) const
{
    const QString rel = relPath.startsWith(QLatin1Char('/')) ? relPath.mid(1) : relPath;
    const QString src = QDir(m_templateRoot).filePath(rel);
    if (!QFileInfo::exists(src))
        return false;

    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    const QString games = paths ? paths->gamesPath() : QStringLiteral("D:/Games");
    QString dst;
    if (rel.startsWith(QStringLiteral("steam/"), Qt::CaseInsensitive))
        dst = steam + QLatin1Char('/') + rel.mid(6);
    else if (rel.startsWith(QStringLiteral("games/"), Qt::CaseInsensitive))
        dst = games + QLatin1Char('/') + rel.mid(6);
    else
        dst = QDir(steam).filePath(rel);

    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QFileInfo::exists(dst))
        QFile::remove(dst);
    return QFile::copy(src, dst);
}

void GoldenImageDriftWatchdog::runResync(const QStringList &paths, qint64 commandId)
{
    m_resyncBusy = true;
    m_pendingResyncId = commandId;
    m_net->ackResyncCommand(commandId, QStringLiteral("running"),
                            QStringLiteral("Копирование ") + QString::number(paths.size()) + QStringLiteral(" файлов"));

    const QStringList batch = paths.mid(0, m_maxResyncFiles);
    int ok = 0;
    for (const QString &abs : batch) {
        if (copyFromTemplate(localRelKey(abs)))
            ++ok;
    }

    m_sessionHashes = hashFiles(criticalPaths());
    const bool allOk = ok == batch.size();
    m_net->setIntegrityTelemetry(allOk ? QStringLiteral("ok") : QStringLiteral("drift"),
                                 aggregateHash(m_sessionHashes),
                                 allOk ? QStringLiteral("Re-sync завершён") : QStringLiteral("Часть файлов не скопирована"),
                                 {});
    m_net->ackResyncCommand(commandId,
                            allOk ? QStringLiteral("ok") : QStringLiteral("partial"),
                            QStringLiteral("Скопировано ") + QString::number(ok) + QLatin1Char('/') + QString::number(batch.size()));
    m_resyncBusy = false;
    m_pendingResyncId = 0;
    qWarning() << "[INTEGRITY] resync done" << ok << "/" << batch.size();
}
