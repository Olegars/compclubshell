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
#include <QThread>
#include <QMetaObject>

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

QString hashFileSha256(const QString &abs)
{
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(64 * 1024);
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QHash<QString, QString> hashPathsSha256(const QStringList &paths)
{
    QHash<QString, QString> out;
    for (const QString &abs : paths) {
        const QString hex = hashFileSha256(abs);
        if (!hex.isEmpty())
            out.insert(normPath(abs), hex);
    }
    return out;
}

QHash<QString, QString> readTemplateHashes(const QString &templateRoot, int maxFiles)
{
    QHash<QString, QString> map;
    const QString manifestPath = QDir(templateRoot).filePath(QStringLiteral("manifest.json"));
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

    if (!QDir(templateRoot).exists())
        return map;

    QDirIterator it(templateRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && map.size() < maxFiles) {
        const QString abs = it.next();
        const QString hex = hashFileSha256(abs);
        if (hex.isEmpty())
            continue;
        map.insert(normPath(relFromRoot(templateRoot, abs)), hex);
    }
    return map;
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
            // После логина UI должен сразу показать каталог. Хеш — через паузу и не на UI-потоке.
            QTimer::singleShot(4000, this, [this]() { startSnapshotAsync(true); });
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
    return hashPathsSha256(paths);
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
    return readTemplateHashes(m_templateRoot, m_maxFiles);
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
    startSnapshotAsync(false);
}

void GoldenImageDriftWatchdog::startSnapshotAsync(bool loadTemplate)
{
    if (!m_enabled || !m_net)
        return;
    bool expected = false;
    if (!m_snapshotBusy.compare_exchange_strong(expected, true))
        return;

    const QStringList paths = criticalPaths();
    const QString templateRoot = m_templateRoot;
    const int maxFiles = m_maxFiles;
    const bool needTemplate = loadTemplate && !m_templateLoaded;

    QThread *thread = QThread::create([this, paths, templateRoot, maxFiles, needTemplate]() {
        const QHash<QString, QString> hashes = hashPathsSha256(paths);
        QHash<QString, QString> tpl;
        if (needTemplate)
            tpl = readTemplateHashes(templateRoot, maxFiles);
        QMetaObject::invokeMethod(this, [this, hashes, tpl, needTemplate]() {
            applySnapshot(hashes, tpl, needTemplate);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void GoldenImageDriftWatchdog::applySnapshot(const QHash<QString, QString> &hashes,
                                             const QHash<QString, QString> &templateHashes,
                                             bool loadedTemplate)
{
    m_sessionHashes = hashes;
    if (loadedTemplate) {
        m_templateHashes = templateHashes;
        m_templateLoaded = true;
    }
    const QString agg = aggregateHash(m_sessionHashes);
    if (m_net)
        m_net->setIntegrityTelemetry(QStringLiteral("ok"), agg, QString(), {});
    qWarning() << "[INTEGRITY] snapshot" << m_sessionHashes.size() << "files hash" << agg.left(12);
    m_snapshotBusy.store(false);
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
    if (m_sessionHashes.isEmpty() || !m_templateLoaded) {
        startSnapshotAsync(true);
        return;
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
