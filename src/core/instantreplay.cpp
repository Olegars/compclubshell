#include "instantreplay.h"
#include "networkmanager.h"
#include "pathresolver.h"
#include "voicehotkeymonitor.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QUuid>
#include <algorithm>

#ifdef Q_OS_WIN
namespace {
InstantReplay *g_replay = nullptr;

LRESULT CALLBACK replayLowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_replay) {
        const KBDLLHOOKSTRUCT *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (info && (info->flags & LLKHF_INJECTED) == 0
            && g_replay->capturing()
            && int(info->vkCode) == g_replay->virtualKey()
            && (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)) {
            QMetaObject::invokeMethod(g_replay, "handleHotkeyTap", Qt::QueuedConnection);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
} // namespace
#endif

InstantReplay::InstantReplay(NetworkManager *net, PathResolver *paths, QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_paths(paths)
{
    loadConfig();
    resolveFfmpeg();
    if (m_available)
        detectNvenc();
    m_logoutWatchdog.setSingleShot(true);
    connect(&m_logoutWatchdog, &QTimer::timeout, this, &InstantReplay::finishLogout);
    if (m_enabled && m_available)
        installHook();

    if (m_net) {
        connect(m_net, &NetworkManager::loginSucceeded, this, [this]() { start(); });
        connect(m_net, &NetworkManager::sessionForceEnded, this, [this]() { stop(); });
        connect(m_net, &NetworkManager::clipUploadSucceeded, this, [this](const QString &url) {
            m_saving = false;
            emit statusChanged();
            emit clipSaved(url);
            setMessage(QStringLiteral("Клип в облачном профиле"));
            QFile::remove(m_exportPath);
            finishLogout();
        });
        connect(m_net, &NetworkManager::clipUploadFailed, this, [this](const QString &err) {
            m_saving = false;
            emit statusChanged();
            emit clipFailed(err);
            setMessage(err);
            finishLogout();
        });
    }
    if (m_net) {
        connect(m_net, &NetworkManager::killHighlight, this, &InstantReplay::onKillHighlight);
        connect(m_net, &NetworkManager::clubFeaturesChanged, this, &InstantReplay::applyClubConfig);
        applyClubConfig();
    }
}

InstantReplay::~InstantReplay()
{
    uninstallHook();
    stop();
}

void InstantReplay::loadConfig()
{
    const QString ini = PathResolver::findConfigIni();
    QSettings s(ini, QSettings::IniFormat);
    const QString en = s.value(QStringLiteral("Replay/enabled"), QStringLiteral("true")).toString().trimmed().toLower();
    m_enabled = (en != QLatin1String("0") && en != QLatin1String("false") && en != QLatin1String("no"));
    m_hotkeyName = s.value(QStringLiteral("Replay/hotkey"), QStringLiteral("F8")).toString().trimmed();
    m_seconds = qBound(10, s.value(QStringLiteral("Replay/seconds"), 60).toInt(), 120);
    m_killSeconds = qBound(8, s.value(QStringLiteral("Replay/kill_seconds"), 12).toInt(), 40);
    const QString sl = s.value(QStringLiteral("Replay/save_on_logout"), QStringLiteral("true")).toString().trimmed().toLower();
    m_saveOnLogout = (sl != QLatin1String("0") && sl != QLatin1String("false") && sl != QLatin1String("no"));
    const QString vert = s.value(QStringLiteral("Replay/vertical"), QStringLiteral("true")).toString().trimmed().toLower();
    m_verticalEnabled = (vert != QLatin1String("0") && vert != QLatin1String("false") && vert != QLatin1String("no"));
    const QString autoKill = s.value(QStringLiteral("Replay/auto_on_kill"), QStringLiteral("true")).toString().trimmed().toLower();
    m_autoOnKill = (autoKill != QLatin1String("0") && autoKill != QLatin1String("false") && autoKill != QLatin1String("no"));
    m_killCooldownSec = qBound(20, s.value(QStringLiteral("Replay/kill_cooldown_sec"), 75).toInt(), 300);
    m_logoPath = s.value(QStringLiteral("Replay/logo")).toString().trimmed();
    if (m_logoPath.isEmpty() && m_paths && !m_paths->dataRoot().isEmpty())
        m_logoPath = m_paths->dataRoot() + QStringLiteral("/branding/logo.png");
    m_ffmpeg = s.value(QStringLiteral("Replay/ffmpeg")).toString().trimmed();
    m_vk = VoiceHotkeyMonitor::parseHotkeyName(m_hotkeyName);
}

void InstantReplay::applyClubConfig()
{
    if (!m_net)
        return;
    const bool clubOn = m_net->featureEnabled(QStringLiteral("instant_replay"));
    const QVariantMap cfg = m_net->clubFeatures().value(QStringLiteral("instant_replay")).toMap();
    if (cfg.contains(QStringLiteral("auto_on_kill")))
        m_autoOnKill = cfg.value(QStringLiteral("auto_on_kill")).toBool();
    if (cfg.contains(QStringLiteral("save_on_logout")))
        m_saveOnLogout = cfg.value(QStringLiteral("save_on_logout")).toBool();
    if (cfg.contains(QStringLiteral("kill_seconds")))
        m_killSeconds = qBound(8, cfg.value(QStringLiteral("kill_seconds")).toInt(), 40);
    if (cfg.contains(QStringLiteral("kill_cooldown_sec")))
        m_killCooldownSec = qBound(20, cfg.value(QStringLiteral("kill_cooldown_sec")).toInt(), 300);

    if (clubOn == m_clubEnabled)
        return;
    m_clubEnabled = clubOn;
    if (!m_clubEnabled) {
        uninstallHook();
        stop();
    } else if (m_enabled && m_available) {
        installHook();
    }
    emit statusChanged();
}

void InstantReplay::resolveFfmpeg()
{
    QStringList candidates;
    if (!m_ffmpeg.isEmpty())
        candidates << m_ffmpeg;
    if (m_paths && !m_paths->dataRoot().isEmpty()) {
        candidates << m_paths->dataRoot() + QStringLiteral("/tools/ffmpeg.exe");
        candidates << m_paths->dataRoot() + QStringLiteral("/ffmpeg.exe");
    }
    candidates << QStringLiteral("D:/Tools/ffmpeg.exe")
               << QStringLiteral("D:/ffmpeg/bin/ffmpeg.exe")
               << QStringLiteral("C:/ffmpeg/bin/ffmpeg.exe");

    const QString pathEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
    for (const QString &dir : pathEnv.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        candidates << QDir(dir).filePath(QStringLiteral("ffmpeg.exe"));
        candidates << QDir(dir).filePath(QStringLiteral("ffmpeg"));
    }

    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) {
            m_ffmpeg = QFileInfo(c).absoluteFilePath();
            m_available = m_enabled;
            qWarning() << "[REPLAY] ffmpeg" << m_ffmpeg;
            emit statusChanged();
            return;
        }
    }
    m_available = false;
    if (m_enabled)
        qWarning() << "[REPLAY] ffmpeg не найден — клипы выкл. Положите ffmpeg.exe в D:/Tools или Replay/ffmpeg=";
    emit statusChanged();
}

void InstantReplay::detectNvenc()
{
    QProcess p;
    p.start(m_ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
    if (!p.waitForFinished(4000)) {
        p.kill();
        return;
    }
    const QString out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
    m_useNvenc = out.contains(QLatin1String("h264_nvenc"));
    qWarning() << "[REPLAY] encoder" << (m_useNvenc ? "h264_nvenc" : "libx264");
}

QString InstantReplay::bufferDir() const
{
    const QString root = m_paths ? m_paths->dataRoot() : QStringLiteral("D:/ShellData");
    return root + QStringLiteral("/replay");
}

void InstantReplay::start()
{
    if (!m_enabled || !m_available || !m_clubEnabled || m_capturing)
        return;
    if (m_paths && !m_paths->cacheOk()) {
        setMessage(QStringLiteral("Нет D: для буфера клипов"));
        return;
    }
    QDir().mkpath(bufferDir() + QStringLiteral("/buf"));
    startCapture(false);
}

void InstantReplay::startCapture(bool allowSoftware)
{
    if (m_capture) {
        m_capture->kill();
        m_capture->deleteLater();
        m_capture = nullptr;
    }

    const QString pattern = bufferDir() + QStringLiteral("/buf/seg%03d.mp4");
    QStringList args = {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"),
        QStringLiteral("-f"), QStringLiteral("gdigrab"),
        QStringLiteral("-framerate"), QStringLiteral("30"),
        QStringLiteral("-draw_mouse"), QStringLiteral("0"),
        QStringLiteral("-i"), QStringLiteral("desktop"),
    };
    if (m_useNvenc && !allowSoftware) {
        args << QStringLiteral("-c:v") << QStringLiteral("h264_nvenc")
             << QStringLiteral("-preset") << QStringLiteral("p5")
             << QStringLiteral("-rc") << QStringLiteral("vbr")
             << QStringLiteral("-cq") << QStringLiteral("28")
             << QStringLiteral("-b:v") << QStringLiteral("4M")
             << QStringLiteral("-maxrate") << QStringLiteral("6M");
    } else {
        args << QStringLiteral("-c:v") << QStringLiteral("libx264")
             << QStringLiteral("-preset") << QStringLiteral("veryfast")
             << QStringLiteral("-crf") << QStringLiteral("28");
    }
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-an")
         << QStringLiteral("-f") << QStringLiteral("segment")
         << QStringLiteral("-segment_time") << QStringLiteral("10")
         << QStringLiteral("-segment_wrap") << QStringLiteral("8")
         << QStringLiteral("-reset_timestamps") << QStringLiteral("1")
         << pattern;

    m_capture = new QProcess(this);
    connect(m_capture, &QProcess::finished, this, &InstantReplay::onCaptureFinished);
    m_capture->start(m_ffmpeg, args);
    if (!m_capture->waitForStarted(3000)) {
        setMessage(QStringLiteral("ffmpeg не стартовал"));
        m_capture->deleteLater();
        m_capture = nullptr;
        m_capturing = false;
        emit statusChanged();
        return;
    }
    m_capturing = true;
    emit statusChanged();
    qWarning() << "[REPLAY] capture started nvenc=" << (m_useNvenc && !allowSoftware);
}

void InstantReplay::onCaptureFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if (!m_capturing)
        return;
    if (m_useNvenc && exitCode != 0) {
        qWarning() << "[REPLAY] nvenc failed, fallback libx264";
        m_useNvenc = false;
        m_capturing = false;
        startCapture(true);
        return;
    }
    m_capturing = false;
    emit statusChanged();
}

void InstantReplay::stop()
{
    m_capturing = false;
    if (m_vertical) {
        m_vertical->disconnect(this);
        m_vertical->kill();
        m_vertical->deleteLater();
        m_vertical = nullptr;
    }
    if (m_capture) {
        m_capture->disconnect(this);
        m_capture->terminate();
        if (!m_capture->waitForFinished(1500))
            m_capture->kill();
        m_capture->deleteLater();
        m_capture = nullptr;
    }
    emit statusChanged();
}

void InstantReplay::saveClip()
{
    saveClipWithSource(QStringLiteral("manual"));
}

void InstantReplay::saveClipWithSource(const QString &source)
{
    if (m_saving)
        return;
    if (!m_available) {
        emit clipFailed(QStringLiteral("ffmpeg не установлен на D:"));
        return;
    }
    m_pendingSource = source;
    m_exportDuration = (source == QLatin1String("kill")) ? m_killSeconds : m_seconds;

    QDir dir(bufferDir() + QStringLiteral("/buf"));
    QFileInfoList files = dir.entryInfoList({QStringLiteral("seg*.mp4")}, QDir::Files);
    std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() < b.lastModified();
    });
    if (m_capturing && !files.isEmpty())
        files.removeLast();
    const int need = qMax(1, (m_exportDuration + 9) / 10);
    while (files.size() > need)
        files.removeFirst();
    if (files.isEmpty()) {
        emit clipFailed(QStringLiteral("Буфер клипа ещё пуст"));
        finishLogout();
        return;
    }

    const QString listPath = bufferDir() + QStringLiteral("/concat.txt");
    QFile list(listPath);
    if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        emit clipFailed(QStringLiteral("Не записать concat"));
        finishLogout();
        return;
    }
    QTextStream out(&list);
    for (const QFileInfo &fi : files) {
        QString p = fi.absoluteFilePath();
        p.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        out << "file '" << p << "'\n";
    }
    list.close();

    m_exportPath = bufferDir() + QStringLiteral("/export.mp4");
    QFile::remove(m_exportPath);

    if (m_concat) {
        m_concat->kill();
        m_concat->deleteLater();
    }
    m_concat = new QProcess(this);
    connect(m_concat, &QProcess::finished, this, &InstantReplay::onConcatFinished);
    m_saving = true;
    emit statusChanged();
    m_concat->start(m_ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("concat"),
        QStringLiteral("-safe"), QStringLiteral("0"),
        QStringLiteral("-i"), listPath,
        QStringLiteral("-c"), QStringLiteral("copy"),
        m_exportPath
    });
}

void InstantReplay::onConcatFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if (exitCode != 0 || !QFileInfo::exists(m_exportPath) || QFileInfo(m_exportPath).size() < 1024) {
        m_saving = false;
        emit statusChanged();
        emit clipFailed(QStringLiteral("Не собрался mp4"));
        finishLogout();
        return;
    }
    if (m_verticalEnabled) {
        startVerticalExport();
        return;
    }
    uploadReadyFile();
}

void InstantReplay::startVerticalExport()
{
    m_shareToken = QUuid::createUuid().toString(QUuid::Id128);
    m_verticalPath = bufferDir() + QStringLiteral("/export_reels.mp4");
    QFile::remove(m_verticalPath);

    const QString qrPath = bufferDir() + QStringLiteral("/qr.png");
    QString shareUrl;
    if (m_net && !m_net->serverUrl().isEmpty())
        shareUrl = m_net->serverUrl() + QStringLiteral("/clips/") + m_shareToken;
    const QString qrFile = shareUrl.isEmpty() ? QString() : downloadQrPng(shareUrl, qrPath);
    const QString logo = (QFileInfo::exists(m_logoPath) ? m_logoPath : QString());
    const QString font = fontFile();
    const QString nick = ffmpegText(m_net ? m_net->playerName() : QString());
    const QString club = ffmpegText(m_net ? m_net->clubName() : QString());

    QStringList inputs = {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"),
        QStringLiteral("-i"), m_exportPath,
    };
    int qrIdx = -1;
    int logoIdx = -1;
    int next = 1;
    if (!logo.isEmpty()) {
        inputs << QStringLiteral("-i") << logo;
        logoIdx = next++;
    }
    if (!qrFile.isEmpty()) {
        inputs << QStringLiteral("-i") << qrFile;
        qrIdx = next++;
    }

    QString filter = (m_pendingSource == QLatin1String("kill"))
        ? QStringLiteral(
            "[0:v]crop='min(iw\\,ih*9/16)*0.82':'min(ih\\,iw*16/9)*0.82':'(iw-ow)/2':'(ih-oh)/2',"
            "scale=1080:1920:flags=fast_bilinear,setsar=1[base]")
        : QStringLiteral(
            "[0:v]crop='min(iw\\,ih*9/16)':'min(ih\\,iw*16/9)':'(iw-ow)/2':'(ih-oh)/2',"
            "scale=1080:1920:flags=fast_bilinear,setsar=1[base]");
    QString last = QStringLiteral("base");
    if (!font.isEmpty() && (!nick.isEmpty() || !club.isEmpty())) {
        QString line = club.isEmpty() ? nick : (nick.isEmpty() ? club : (club + QStringLiteral("  ·  ") + nick));
        filter += QStringLiteral(";[") + last + QStringLiteral("]drawtext=fontfile='")
            + ffmpegText(font)
            + QStringLiteral("':text='") + line
            + QStringLiteral("':fontsize=48:fontcolor=white:borderw=3:bordercolor=black:"
                             "x=(w-text_w)/2:y=h-th-220[txt]");
        last = QStringLiteral("txt");
    }
    if (logoIdx >= 0) {
        filter += QStringLiteral(";[") + QString::number(logoIdx) + QStringLiteral(":v]scale=200:-1[logo];[")
            + last + QStringLiteral("][logo]overlay=48:48[lg]");
        last = QStringLiteral("lg");
    }
    if (qrIdx >= 0) {
        filter += QStringLiteral(";[") + QString::number(qrIdx) + QStringLiteral(":v]scale=220:220[qr];[")
            + last + QStringLiteral("][qr]overlay=W-260:H-260[outv]");
        last = QStringLiteral("outv");
    }

    QStringList args = inputs;
    args << QStringLiteral("-filter_complex") << filter
         << QStringLiteral("-map") << (QStringLiteral("[") + last + QStringLiteral("]"));
    if (m_useNvenc) {
        args << QStringLiteral("-c:v") << QStringLiteral("h264_nvenc")
             << QStringLiteral("-preset") << QStringLiteral("p5")
             << QStringLiteral("-cq") << QStringLiteral("28");
    } else {
        args << QStringLiteral("-c:v") << QStringLiteral("libx264")
             << QStringLiteral("-preset") << QStringLiteral("veryfast")
             << QStringLiteral("-crf") << QStringLiteral("28");
    }
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-an")
         << QStringLiteral("-movflags") << QStringLiteral("+faststart")
         << m_verticalPath;

    if (m_vertical) {
        m_vertical->kill();
        m_vertical->deleteLater();
    }
    m_vertical = new QProcess(this);
    connect(m_vertical, &QProcess::finished, this, &InstantReplay::onVerticalFinished);
    setMessage(QStringLiteral("Reels 9:16…"));
    m_vertical->start(m_ffmpeg, args);
}

void InstantReplay::onVerticalFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if (exitCode == 0 && QFileInfo::exists(m_verticalPath) && QFileInfo(m_verticalPath).size() > 1024) {
        m_exportPath = m_verticalPath;
        uploadReadyFile();
        return;
    }
    qWarning() << "[REPLAY] vertical failed, upload 16:9";
    m_shareToken.clear();
    m_exportPath = bufferDir() + QStringLiteral("/export.mp4");
    uploadReadyFile();
}

void InstantReplay::uploadReadyFile()
{
    if (!m_net) {
        m_saving = false;
        emit clipFailed(QStringLiteral("Нет сети"));
        finishLogout();
        return;
    }
    setMessage(QStringLiteral("Загрузка в профиль…"));
    const QString aspect = (m_verticalEnabled && m_exportPath.endsWith(QLatin1String("export_reels.mp4")))
        ? QStringLiteral("9:16") : QString();
    m_net->uploadClip(m_exportPath, m_exportDuration, m_shareToken, aspect, m_pendingSource);
}

void InstantReplay::flushAndLogout(int terminalId)
{
    m_pendingLogoutId = terminalId;
    if (m_saveOnLogout && m_available && (m_capturing || QDir(bufferDir() + QStringLiteral("/buf")).exists())) {
        m_logoutWatchdog.start(45000);
        saveClipWithSource(QStringLiteral("logout"));
        return;
    }
    finishLogout();
}

void InstantReplay::finishLogout()
{
    m_logoutWatchdog.stop();
    const int tid = m_pendingLogoutId;
    m_pendingLogoutId = 0;
    if (tid <= 0)
        return;
    stop();
    if (m_net)
        m_net->logoutTerminal(tid);
}

void InstantReplay::setMessage(const QString &msg)
{
    if (m_lastMessage == msg)
        return;
    m_lastMessage = msg;
    emit statusChanged();
}

void InstantReplay::handleHotkeyTap()
{
    if (!m_capturing || m_saving)
        return;
    saveClipWithSource(QStringLiteral("manual"));
}

void InstantReplay::onKillHighlight()
{
    if (!m_autoOnKill || !m_capturing || m_saving)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastKillSaveMs > 0 && now - m_lastKillSaveMs < qint64(m_killCooldownSec) * 1000)
        return;
    m_lastKillSaveMs = now;
    setMessage(QStringLiteral("Killcam…"));
    saveClipWithSource(QStringLiteral("kill"));
}

QString InstantReplay::ffmpegText(const QString &raw) const
{
    QString t = raw.trimmed();
    t.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    t.replace(QLatin1Char(':'), QStringLiteral("\\:"));
    t.replace(QLatin1Char('\''), QString());
    t.replace(QLatin1Char('%'), QString());
    t.replace(QLatin1Char('='), QString());
    t.replace(QLatin1Char(';'), QString());
    t.replace(QLatin1Char('['), QString());
    t.replace(QLatin1Char(']'), QString());
    return t.left(40);
}

QString InstantReplay::fontFile() const
{
    const QStringList fonts = {
        QStringLiteral("C:/Windows/Fonts/arialbd.ttf"),
        QStringLiteral("C:/Windows/Fonts/arial.ttf"),
        QStringLiteral("C:/Windows/Fonts/segoeui.ttf"),
    };
    for (const QString &f : fonts) {
        if (QFileInfo::exists(f))
            return f;
    }
    return {};
}

QString InstantReplay::downloadQrPng(const QString &payload, const QString &dest) const
{
    if (!m_net || payload.isEmpty())
        return {};
    QUrl url(QStringLiteral("https://api.qrserver.com/v1/create-qr-code/?size=280x280&margin=6&data=")
             + QString::fromUtf8(QUrl::toPercentEncoding(payload)));
    QNetworkRequest req(url);
    req.setTransferTimeout(4000);
    QNetworkReply *reply = m_net->networkAccessManager()->get(req);
    QEventLoop loop;
    QTimer killer;
    killer.setSingleShot(true);
    QObject::connect(&killer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    killer.start(4000);
    loop.exec();
    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return {};
    }
    QFile f(dest);
    if (!f.open(QIODevice::WriteOnly)) {
        reply->deleteLater();
        return {};
    }
    f.write(reply->readAll());
    f.close();
    reply->deleteLater();
    return QFileInfo(dest).size() > 64 ? dest : QString();
}

void InstantReplay::installHook()
{
#ifdef Q_OS_WIN
    if (m_hook)
        return;
    g_replay = this;
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, replayLowLevelKeyboardProc,
                               GetModuleHandleW(nullptr), 0);
    if (!m_hook) {
        qWarning() << "[REPLAY] WH_KEYBOARD_LL failed" << GetLastError();
        g_replay = nullptr;
    }
#else
    Q_UNUSED(this);
#endif
}

void InstantReplay::uninstallHook()
{
#ifdef Q_OS_WIN
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (g_replay == this)
        g_replay = nullptr;
#endif
}
