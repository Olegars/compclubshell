#include "rollbackmarker.h"

#include "ccbootsuperclient.h"
#include "networkmanager.h"
#include "pathresolver.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

namespace {

QString normRel(const QString &rel)
{
    return QDir::fromNativeSeparators(rel).toLower().replace(QRegularExpression(QStringLiteral("^/+")), QString());
}

QString sha256File(const QString &abs, QByteArray *bodyOut, int maxBody)
{
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.readAll();
    f.close();
    if (bodyOut && raw.size() > 0 && raw.size() <= maxBody && !raw.contains('\0'))
        *bodyOut = raw;
    return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
}

} // namespace

RollbackMarkerWatchdog::RollbackMarkerWatchdog(NetworkManager *net,
                                               CcbootSuperClient *ccboot,
                                               QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_ccboot(ccboot)
{
    loadConfig();
    if (m_ccboot) {
        connect(m_ccboot, &CcbootSuperClient::superClientFinished, this,
                [this](bool enable, bool saved, bool ok, const QString &diskMode) {
                    if (enable || !ok || !saved)
                        return;
                    writePending(diskMode);
                    snapshotAndUpload(diskMode, QStringLiteral("super client save"));
                });
    }
    if (m_net) {
        connect(m_net, &NetworkManager::rollbackCommandReceived, this,
                [this](qint64 commandId, qint64 revisionId, const QString &) {
                    onRollbackCommand(commandId, revisionId);
                });
        connect(m_net, &NetworkManager::goldenRevisionFetched, this,
                [this](qint64, const QJsonObject &payload) {
                    applyFiles(payload.value(QStringLiteral("files")).toArray(), m_pendingCommandId);
                });
        connect(m_net, &NetworkManager::goldenRevisionFetchFailed, this,
                [this](qint64, const QString &message) {
                    if (m_pendingCommandId > 0)
                        m_net->ackRollbackCommand(m_pendingCommandId, QStringLiteral("error"), message);
                    m_busy = false;
                    m_pendingCommandId = 0;
                });
        connect(m_net, &NetworkManager::clubFeaturesChanged, this, [this]() {
            if (!clubEnabled())
                return;
            const QString pending = takePending();
            if (!pending.isEmpty())
                snapshotAndUpload(pending, QStringLiteral("pending after reboot"));
        });
    }
    QTimer::singleShot(6000, this, [this]() {
        detectCrash();
        const QString pending = takePending();
        if (!pending.isEmpty())
            snapshotAndUpload(pending, QStringLiteral("pending after reboot"));
    });
}

void RollbackMarkerWatchdog::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("Rollback/enabled"), true).toString().trimmed().toLower();
    m_localEnabled = !(raw == QLatin1String("0") || raw == QLatin1String("false") || raw == QLatin1String("no"));
}

bool RollbackMarkerWatchdog::clubEnabled() const
{
    if (!m_localEnabled)
        return false;
    return !m_net || m_net->featureEnabled(QStringLiteral("rollback_markers"));
}

void RollbackMarkerWatchdog::markCleanShutdown()
{
    const QString path = markerPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("ok\n");
        f.close();
    }
}

QString RollbackMarkerWatchdog::markerPath() const
{
    PathResolver *paths = PathResolver::instance();
    if (!paths || paths->dataRoot().isEmpty())
        return {};
    return paths->persistentFile(QStringLiteral("power/clean_shutdown.ok"));
}

QString RollbackMarkerWatchdog::pendingPath() const
{
    PathResolver *paths = PathResolver::instance();
    if (!paths || paths->dataRoot().isEmpty())
        return {};
    return paths->persistentFile(QStringLiteral("rollback/pending_save.txt"));
}

QString RollbackMarkerWatchdog::everBootedPath() const
{
    PathResolver *paths = PathResolver::instance();
    if (!paths || paths->dataRoot().isEmpty())
        return {};
    return paths->persistentFile(QStringLiteral("power/ever_booted.ok"));
}

void RollbackMarkerWatchdog::writePending(const QString &diskMode) const
{
    const QString path = pendingPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write((diskMode.isEmpty() ? QStringLiteral("image") : diskMode).toUtf8());
        f.close();
    }
}

QString RollbackMarkerWatchdog::takePending() const
{
    const QString path = pendingPath();
    if (path.isEmpty() || !QFile::exists(path))
        return {};
    QFile f(path);
    QString mode = QStringLiteral("image");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString raw = QString::fromUtf8(f.readAll()).trimmed().toLower();
        f.close();
        if (raw == QLatin1String("disk") || raw == QLatin1String("both") || raw == QLatin1String("image"))
            mode = raw;
    }
    QFile::remove(path);
    return mode;
}

void RollbackMarkerWatchdog::detectCrash()
{
    if (!clubEnabled() || !m_net)
        return;
    const QString ever = everBootedPath();
    const QString clean = markerPath();
    if (!ever.isEmpty() && !QFile::exists(ever)) {
        QDir().mkpath(QFileInfo(ever).absolutePath());
        QFile f(ever);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("ok\n");
            f.close();
        }
        if (!clean.isEmpty() && QFile::exists(clean))
            QFile::remove(clean);
        return;
    }
    if (!clean.isEmpty() && QFile::exists(clean)) {
        QFile::remove(clean);
        return;
    }
    if (m_ccboot && m_ccboot->superClientActive())
        return;

    m_crashReason = classifyCrash();
    m_crashDetail = QStringLiteral("Нештатная перезагрузка (%1)").arg(m_crashReason);
    m_net->setCrashTelemetry(true, m_crashReason, m_crashDetail);
    m_net->reportShellIncident(QStringLiteral("golden_image_crash"),
                               QStringLiteral("high"),
                               QString(),
                               QJsonObject{
                                   {QStringLiteral("reason"), m_crashReason},
                                   {QStringLiteral("detail"), m_crashDetail},
                               });
    qWarning() << "[ROLLBACK] crash" << m_crashReason;
}

QString RollbackMarkerWatchdog::classifyCrash() const
{
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("wevtutil"), QStringList{
        QStringLiteral("qe"),
        QStringLiteral("System"),
        QStringLiteral("/q:*[System[(EventID=1001 or EventID=41 or EventID=6008)]]"),
        QStringLiteral("/c:1"),
        QStringLiteral("/rd:true"),
        QStringLiteral("/f:text"),
    });
    if (p.waitForFinished(1500)) {
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        if (out.contains(QStringLiteral("1001")) || out.contains(QStringLiteral("Bugcheck"), Qt::CaseInsensitive)
            || out.contains(QStringLiteral("Bug Check"), Qt::CaseInsensitive))
            return QStringLiteral("bsod");
        if (out.contains(QStringLiteral("Event ID: 41")) || out.contains(QStringLiteral("Kernel-Power")))
            return QStringLiteral("driver");
        if (!out.trimmed().isEmpty())
            return QStringLiteral("unexpected");
    }
#endif
    return QStringLiteral("unexpected");
}

void RollbackMarkerWatchdog::snapshotAndUpload(const QString &diskMode, const QString &note)
{
    if (!clubEnabled() || !m_net || m_busy)
        return;
    const QJsonArray files = collectFiles();
    if (files.isEmpty()) {
        qWarning() << "[ROLLBACK] snapshot empty";
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("disk_mode"), diskMode.isEmpty() ? QStringLiteral("image") : diskMode);
    payload.insert(QStringLiteral("note"), note);
    payload.insert(QStringLiteral("files"), files);
    m_net->postGoldenRevision(payload);
    qWarning() << "[ROLLBACK] snapshot" << files.size() << "files" << note;
}

QJsonObject RollbackMarkerWatchdog::fileEntry(const QString &abs, const QString &rel, const QString &kind) const
{
    QByteArray body;
    const QString hash = sha256File(abs, &body, 24576);
    if (hash.isEmpty())
        return {};
    QJsonObject o;
    o.insert(QStringLiteral("rel"), normRel(rel));
    o.insert(QStringLiteral("sha256"), hash);
    o.insert(QStringLiteral("kind"), kind);
    if (!body.isEmpty())
        o.insert(QStringLiteral("body"), QString::fromUtf8(body));
    return o;
}

void RollbackMarkerWatchdog::appendDirMasked(QJsonArray *out, const QString &dir, const QString &mask,
                                            const QString &relPrefix, const QString &kind) const
{
    if (!out || dir.isEmpty())
        return;
    QDir d(dir);
    if (!d.exists())
        return;
    const auto names = d.entryList({mask}, QDir::Files);
    for (const QString &name : names) {
        if (out->size() >= 280)
            return;
        const QJsonObject o = fileEntry(d.filePath(name),
                                        relPrefix.isEmpty() ? name : (relPrefix + QLatin1Char('/') + name),
                                        kind);
        if (!o.isEmpty())
            out->append(o);
    }
}

QJsonArray RollbackMarkerWatchdog::collectFiles() const
{
    QJsonArray out;
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    const QString epic = paths ? paths->epicPath() : QString();

    appendDirMasked(&out, QDir(steam).filePath(QStringLiteral("steamapps")),
                    QStringLiteral("appmanifest_*.acf"),
                    QStringLiteral("steamapps"), QStringLiteral("steam_manifest"));
    const QString libVdf = QDir(steam).filePath(QStringLiteral("steamapps/libraryfolders.vdf"));
    if (QFileInfo::exists(libVdf) && out.size() < 280) {
        const QJsonObject o = fileEntry(libVdf, QStringLiteral("steamapps/libraryfolders.vdf"),
                                        QStringLiteral("steam_config"));
        if (!o.isEmpty())
            out.append(o);
        QFile vf(libVdf);
        if (vf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(vf.readAll());
            vf.close();
            static const QRegularExpression pathRe(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
            auto it = pathRe.globalMatch(text);
            const QString self = QDir::cleanPath(steam).toLower();
            while (it.hasNext() && out.size() < 280) {
                const QString p = QDir::cleanPath(it.next().captured(1).replace(QLatin1Char('\\'), QLatin1Char('/')));
                if (p.isEmpty() || p.toLower() == self)
                    continue;
                appendDirMasked(&out, QDir(p).filePath(QStringLiteral("steamapps")),
                                QStringLiteral("appmanifest_*.acf"),
                                QStringLiteral("steamapps"), QStringLiteral("steam_manifest"));
            }
        }
    }

    const QString programData = qEnvironmentVariable("PROGRAMDATA");
    const QString epicDefault = programData.isEmpty()
        ? QString()
        : QDir(programData).filePath(QStringLiteral("Epic/EpicGamesLauncher/Data/Manifests"));
    appendDirMasked(&out, epicDefault, QStringLiteral("*.item"),
                    QStringLiteral("epic"), QStringLiteral("epic_item"));
    if (!epic.isEmpty()) {
        appendDirMasked(&out, QDir(epic).filePath(QStringLiteral("Manifests")),
                        QStringLiteral("*.item"), QStringLiteral("epic"), QStringLiteral("epic_item"));
        appendDirMasked(&out, epic, QStringLiteral("*.item"),
                        QStringLiteral("epic"), QStringLiteral("epic_item"));
    }

    const QString ini = PathResolver::findConfigIni();
    if (QFileInfo::exists(ini) && out.size() < 280) {
        const QJsonObject o = fileEntry(ini, QStringLiteral("config.ini"), QStringLiteral("shell_config"));
        if (!o.isEmpty())
            out.append(o);
    }
    return out;
}

void RollbackMarkerWatchdog::onRollbackCommand(qint64 commandId, qint64 revisionId)
{
    if (!clubEnabled() || !m_net || commandId <= 0 || revisionId <= 0)
        return;
    if (m_busy)
        return;
    m_busy = true;
    m_pendingCommandId = commandId;
    m_net->ackRollbackCommand(commandId, QStringLiteral("running"),
                              QStringLiteral("Загрузка ревизии ") + QString::number(revisionId));
    m_net->fetchGoldenRevision(revisionId);
}

void RollbackMarkerWatchdog::applyFiles(const QJsonArray &files, qint64 commandId)
{
    int ok = 0;
    int n = 0;
    for (const QJsonValue &v : files) {
        const QJsonObject o = v.toObject();
        const QString body = o.value(QStringLiteral("body")).toString();
        if (body.isEmpty())
            continue;
        ++n;
        const QString dest = destFor(o);
        if (dest.isEmpty())
            continue;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            continue;
        f.write(body.toUtf8());
        f.close();
        ++ok;
    }
    m_busy = false;
    m_pendingCommandId = 0;
    if (m_net) {
        const bool allOk = n == 0 || ok == n;
        m_net->ackRollbackCommand(commandId,
                                  allOk ? QStringLiteral("ok") : QStringLiteral("partial"),
                                  QStringLiteral("Восстановлено ") + QString::number(ok)
                                      + QLatin1Char('/') + QString::number(n));
    }
    qWarning() << "[ROLLBACK] applied" << ok << "/" << n;
}

QString RollbackMarkerWatchdog::destFor(const QJsonObject &file) const
{
    PathResolver *paths = PathResolver::instance();
    const QString kind = file.value(QStringLiteral("kind")).toString();
    const QString rel = file.value(QStringLiteral("rel")).toString();
    if (kind == QLatin1String("shell_config") || rel == QLatin1String("config.ini"))
        return PathResolver::findConfigIni();
    if (kind.startsWith(QLatin1String("epic"))) {
        const QString programData = qEnvironmentVariable("PROGRAMDATA");
        const QString epicDefault = programData.isEmpty()
            ? QString()
            : QDir(programData).filePath(QStringLiteral("Epic/EpicGamesLauncher/Data/Manifests"));
        return QDir(epicDefault).filePath(QFileInfo(rel).fileName());
    }
    const QString steam = paths ? paths->steamPath() : QStringLiteral("D:/Steam");
    return QDir(steam).filePath(rel);
}
