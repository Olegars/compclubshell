#include "pathresolver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

PathResolver *PathResolver::s_instance = nullptr;

namespace {

const QStringList kSkipDirNames = {
    QStringLiteral("Cache"),
    QStringLiteral("Cache_Data"),
    QStringLiteral("GPUCache"),
    QStringLiteral("Code Cache"),
    QStringLiteral("DawnCache"),
    QStringLiteral("GrShaderCache"),
    QStringLiteral("ShaderCache"),
    QStringLiteral("Crashpad"),
    QStringLiteral("logs"),
    QStringLiteral("Logs"),
};

bool shouldSkipName(const QString &name)
{
    for (const QString &skip : kSkipDirNames) {
        if (name.compare(skip, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool copyFileOverwrite(const QString &from, const QString &to)
{
    if (!QFile::exists(from))
        return false;
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (QFile::exists(to))
        QFile::remove(to);
    return QFile::copy(from, to);
}

void copyTreeFiltered(const QString &from, const QString &to)
{
    QDir src(from);
    if (!src.exists())
        return;
    QDir().mkpath(to);
    const auto entries = src.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries) {
        if (shouldSkipName(fi.fileName()))
            continue;
        const QString dest = to + QLatin1Char('/') + fi.fileName();
        if (fi.isDir())
            copyTreeFiltered(fi.absoluteFilePath(), dest);
        else
            copyFileOverwrite(fi.absoluteFilePath(), dest);
    }
}

QString localAppData()
{
    const QString env = qEnvironmentVariable("LOCALAPPDATA");
    if (!env.isEmpty())
        return QDir::fromNativeSeparators(env);
    return QDir::fromNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}

#ifdef Q_OS_WIN
QString volumeRootForPath(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    if (native.size() >= 2 && native.at(1) == QLatin1Char(':'))
        return native.left(2).toUpper() + QStringLiteral("\\");
    return {};
}
#endif

} // namespace

PathResolver *PathResolver::instance()
{
    return s_instance;
}

QString PathResolver::findConfigIni()
{
    const QString base = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        base + QStringLiteral("/config.ini"),
        base + QStringLiteral("/../config.ini"),
        base + QStringLiteral("/../../config.ini"),
    };
    for (const QString &c : candidates) {
        if (QFile::exists(c))
            return QDir(c).absolutePath();
    }
    return candidates.first();
}

PathResolver::PathResolver(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    m_configPath = findConfigIni();
    loadConfig();
    resolveDataRoot();
    ensureLayout();
    updateFreeSpace();
    if (m_cacheOk)
        restoreLauncherCaches();

    connect(&m_readyTimer, &QTimer::timeout, this, [this]() {
        ++m_readyAttempts;
        resolveDataRoot();
        ensureLayout();
        updateFreeSpace();
        if (m_cacheOk) {
            restoreLauncherCaches();
            m_readyTimer.stop();
            qWarning() << "[STORAGE] том кэша готов:" << m_dataRoot
                       << "free_gb" << m_cacheFreeGb;
            return;
        }
        if (m_readyAttempts * 2 >= m_readyTimeoutSec) {
            m_readyTimer.stop();
            qWarning() << "[STORAGE] том кэша не появился за"
                       << m_readyTimeoutSec << "с — игры заблокированы";
        }
    });
    if (!m_cacheOk && m_readyTimeoutSec > 0)
        m_readyTimer.start(2000);
}

void PathResolver::loadConfig()
{
    QSettings s(m_configPath, QSettings::IniFormat);
    m_configuredRoot = QDir::fromNativeSeparators(
        s.value(QStringLiteral("Storage/data_root"), QStringLiteral("D:/ShellData")).toString().trimmed());
    m_volumeLabel = s.value(QStringLiteral("Storage/volume_label"), QStringLiteral("GAMES")).toString().trimmed();
    m_readyTimeoutSec = s.value(QStringLiteral("Storage/ready_timeout_sec"), 90).toInt();
    if (m_readyTimeoutSec < 0)
        m_readyTimeoutSec = 0;

    auto readPath = [&s](const QString &key, const QString &fallback) {
        const QString v = QDir::fromNativeSeparators(s.value(key, fallback).toString().trimmed());
        return v.isEmpty() ? fallback : v;
    };
    m_steamPath = readPath(QStringLiteral("Paths/steam"), QStringLiteral("D:/Steam"));
    m_gamesPath = readPath(QStringLiteral("Paths/games"), QStringLiteral("D:/Games"));
    m_epicPath = readPath(QStringLiteral("Paths/epic"), QString());
    m_eaPath = readPath(QStringLiteral("Paths/ea"), QString());
    m_riotPath = readPath(QStringLiteral("Paths/riot"), QString());

    if (m_epicPath.isEmpty()) {
        m_epicPath = firstExisting({
            QStringLiteral("D:/Epic Games/Launcher/Portal/Binaries/Win64/EpicGamesLauncher.exe"),
            QStringLiteral("D:/Program Files/Epic Games/Launcher/Portal/Binaries/Win64/EpicGamesLauncher.exe"),
            QStringLiteral("C:/Program Files/Epic Games/Launcher/Portal/Binaries/Win64/EpicGamesLauncher.exe"),
            QStringLiteral("C:/Program Files (x86)/Epic Games/Launcher/Portal/Binaries/Win32/EpicGamesLauncher.exe"),
        }, QStringLiteral("C:/Program Files/Epic Games/Launcher/Portal/Binaries/Win32/EpicGamesLauncher.exe"));
    }
    if (m_eaPath.isEmpty()) {
        m_eaPath = firstExisting({
            QStringLiteral("D:/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe"),
            QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe"),
            QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EADesktop.exe"),
        }, QStringLiteral("C:/Program Files/Electronic Arts/EA Desktop/EA Desktop/EADesktop.exe"));
    }
    if (m_riotPath.isEmpty()) {
        m_riotPath = firstExisting({
            QStringLiteral("D:/Riot Games/Riot Client/RiotClientServices.exe"),
            QStringLiteral("C:/Riot Games/Riot Client/RiotClientServices.exe"),
        }, QStringLiteral("C:/Riot Games/Riot Client/RiotClientServices.exe"));
    }

    if (!QFileInfo::exists(m_steamPath + QStringLiteral("/steam.exe"))) {
        const QString cSteam = QStringLiteral("C:/Program Files (x86)/Steam");
        if (QFileInfo::exists(cSteam + QStringLiteral("/steam.exe")))
            m_steamPath = cSteam;
    }

    emit pathsChanged();
}

QString PathResolver::firstExisting(const QStringList &candidates, const QString &fallback) const
{
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return fallback;
}

bool PathResolver::tryUsePath(const QString &path, bool requireWritable)
{
    if (path.isEmpty())
        return false;
    QDir dir(path);
    const QString abs = dir.absolutePath();
    if (!dir.exists()) {
        if (!requireWritable)
            return false;
        if (!QDir().mkpath(abs))
            return false;
    }
    if (requireWritable) {
        QFile probe(abs + QStringLiteral("/.reactor-write"));
        if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        probe.write("ok");
        probe.close();
        probe.remove();
    }
    m_dataRoot = abs;
#ifdef Q_OS_WIN
    const QString root = volumeRootForPath(abs);
    m_volumeLetter = root.isEmpty() ? QString() : root.left(1);
#else
    m_volumeLetter.clear();
#endif
    return true;
}

QString PathResolver::findVolumeByLabel(const QString &label) const
{
    if (label.isEmpty())
        return {};
#ifdef Q_OS_WIN
    const DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26; ++i) { // skip A: B:
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[] = L"X:\\";
        root[0] = static_cast<wchar_t>(L'A' + i);
        wchar_t volName[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(root, volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0))
            continue;
        if (QString::fromWCharArray(volName).compare(label, Qt::CaseInsensitive) == 0)
            return QString::fromWCharArray(root);
    }
#endif
    Q_UNUSED(label);
    return {};
}

void PathResolver::resolveDataRoot()
{
    const bool wasOk = m_cacheOk;
    const QString wasRoot = m_dataRoot;

    if (tryUsePath(m_configuredRoot, true)) {
        m_cacheOk = true;
    } else {
        const QString labeled = findVolumeByLabel(m_volumeLabel);
        if (!labeled.isEmpty() && tryUsePath(labeled + QStringLiteral("ShellData"), true)) {
            m_cacheOk = true;
        } else if (tryUsePath(QStringLiteral("D:/ShellData"), true)) {
            m_cacheOk = true;
        } else {
            m_cacheOk = false;
            tryUsePath(QStringLiteral("C:/ShellVideo"), true);
            if (wasOk || wasRoot != m_dataRoot) {
                qWarning() << "[STORAGE] persistent SSD недоступен, fallback"
                           << m_dataRoot << "(cache_ok=false)";
            }
        }
    }

    if (wasOk != m_cacheOk || wasRoot != m_dataRoot)
        emit cacheStatusChanged();
}

void PathResolver::ensureLayout()
{
    if (m_dataRoot.isEmpty())
        return;
    QDir().mkpath(overlayCachePath());
    QDir().mkpath(voiceDir());
    QDir().mkpath(lobbyDir());
    QDir().mkpath(m_dataRoot + QStringLiteral("/cache/steam"));
    QDir().mkpath(m_dataRoot + QStringLiteral("/cache/epic"));
    QDir().mkpath(m_dataRoot + QStringLiteral("/cache/ea"));
    QDir().mkpath(m_dataRoot + QStringLiteral("/cache/riot"));
    QDir().mkpath(m_dataRoot + QStringLiteral("/logs"));
}

void PathResolver::updateFreeSpace()
{
    if (m_dataRoot.isEmpty()) {
        m_cacheFreeGb = 0;
        return;
    }
    const QStorageInfo info(m_dataRoot);
    const qint64 bytes = info.isValid() ? info.bytesAvailable() : 0;
    m_cacheFreeGb = bytes > 0 ? (bytes / 1024.0 / 1024.0 / 1024.0) : 0.0;
    emit cacheStatusChanged();
}

void PathResolver::refresh()
{
    resolveDataRoot();
    ensureLayout();
    updateFreeSpace();
    if (m_cacheOk)
        restoreLauncherCaches();
}

QString PathResolver::overlayCachePath() const
{
    return m_dataRoot + QStringLiteral("/Cache");
}

QString PathResolver::fallbackVideoUrl() const
{
    return fileUrl(overlayCachePath() + QStringLiteral("/fallback_bg.mp4"));
}

QString PathResolver::debugLogPath() const
{
    return m_dataRoot + QStringLiteral("/logs/shell-debug.log");
}

QString PathResolver::voiceDir() const
{
    return m_dataRoot + QStringLiteral("/Voice");
}

QString PathResolver::lobbyDir() const
{
    return m_dataRoot;
}

QString PathResolver::fileUrl(const QString &absolutePath) const
{
    return QUrl::fromLocalFile(QDir::fromNativeSeparators(absolutePath)).toString();
}

QString PathResolver::persistentFile(const QString &relative) const
{
    return m_dataRoot + QStringLiteral("/cache/") + relative;
}

void PathResolver::persistFile(const QString &volatilePath, const QString &relativeName)
{
    if (!m_cacheOk || volatilePath.isEmpty() || !QFile::exists(volatilePath))
        return;
    copyFileOverwrite(volatilePath, persistentFile(relativeName));
}

void PathResolver::restoreFile(const QString &volatilePath, const QString &relativeName)
{
    const QString src = persistentFile(relativeName);
    if (!QFile::exists(src))
        return;
    copyFileOverwrite(src, volatilePath);
}

void PathResolver::restoreLauncherCaches()
{
    if (!m_cacheOk || m_restoredLaunchers)
        return;
    const QString local = localAppData();
    restoreFile(local + QStringLiteral("/Steam/local.vdf"), QStringLiteral("steam/local.vdf"));
    copyTreeFiltered(persistentFile(QStringLiteral("epic/Config")),
                     local + QStringLiteral("/EpicGamesLauncher/Saved/Config"));
    copyTreeFiltered(persistentFile(QStringLiteral("ea")),
                     local + QStringLiteral("/Electronic Arts/EA Desktop"));
    copyTreeFiltered(persistentFile(QStringLiteral("riot/Data")),
                     local + QStringLiteral("/Riot Games/Riot Client/Data"));
    copyTreeFiltered(persistentFile(QStringLiteral("riot/Config")),
                     local + QStringLiteral("/Riot Games/Riot Client/Config"));
    m_restoredLaunchers = true;
    qWarning() << "[STORAGE] machine-cache лаунчеров восстановлен с" << m_dataRoot;
}

void PathResolver::persistLauncherCaches()
{
    if (!m_cacheOk)
        return;
    const QString local = localAppData();
    persistFile(local + QStringLiteral("/Steam/local.vdf"), QStringLiteral("steam/local.vdf"));
    copyTreeFiltered(local + QStringLiteral("/EpicGamesLauncher/Saved/Config"),
                     persistentFile(QStringLiteral("epic/Config")));
    copyTreeFiltered(local + QStringLiteral("/Electronic Arts/EA Desktop"),
                     persistentFile(QStringLiteral("ea")));
    copyTreeFiltered(local + QStringLiteral("/Riot Games/Riot Client/Data"),
                     persistentFile(QStringLiteral("riot/Data")));
    copyTreeFiltered(local + QStringLiteral("/Riot Games/Riot Client/Config"),
                     persistentFile(QStringLiteral("riot/Config")));
}

QStringList PathResolver::expandLauncherCandidates(const QStringList &paths) const
{
    QStringList out;
    auto add = [&out](const QString &p) {
        const QString n = QDir::fromNativeSeparators(p);
        if (n.isEmpty() || out.contains(n, Qt::CaseInsensitive))
            return;
        out.append(n);
    };

    add(m_steamPath + QStringLiteral("/steam.exe"));
    add(m_epicPath);
    add(m_eaPath);
    add(m_riotPath);

    for (const QString &raw : paths) {
        add(raw);
        QString onD = raw;
        onD.replace(QStringLiteral("C:/"), QStringLiteral("D:/"));
        onD.replace(QStringLiteral("C:\\"), QStringLiteral("D:\\"));
        add(onD);
        const QString native = QDir::fromNativeSeparators(raw);
        const int pf = native.indexOf(QStringLiteral("/Program Files"), 0, Qt::CaseInsensitive);
        if (pf >= 0 && !m_gamesPath.isEmpty())
            add(m_gamesPath + native.mid(pf));
        if (native.contains(QStringLiteral("Steam"), Qt::CaseInsensitive) && !m_steamPath.isEmpty()) {
            const int steamIdx = native.lastIndexOf(QStringLiteral("/Steam"), -1, Qt::CaseInsensitive);
            if (steamIdx >= 0)
                add(m_steamPath + native.mid(steamIdx + 6));
        }
    }
    return out;
}

bool PathResolver::waitForCache(int timeoutMs)
{
    Q_UNUSED(timeoutMs);
    refresh();
    return m_cacheOk;
}
