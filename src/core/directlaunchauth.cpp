#include "directlaunchauth.h"

#include <QDebug>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

DirectLaunchAuth::DirectLaunchAuth(const QString &platformId, QObject *parent)
    : IPlatformAuth(parent)
    , m_platformId(platformId.isEmpty() ? QStringLiteral("direct") : platformId.toLower())
{
    if (m_platformId == QLatin1String("riot"))
        m_launcherImage = QStringLiteral("RiotClientServices.exe");
}

void DirectLaunchAuth::killLauncher()
{
#ifdef Q_OS_WIN
    QStringList images;
    if (m_platformId == QLatin1String("riot")) {
        images << QStringLiteral("RiotClientServices.exe")
               << QStringLiteral("RiotClient.exe")
               << QStringLiteral("RiotClientCrashHandler.exe");
    } else if (!m_launcherImage.isEmpty()) {
        images << m_launcherImage;
    }
    for (const QString &image : images) {
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
    }
#else
    Q_UNUSED(m_launcherImage);
#endif
}

bool DirectLaunchAuth::applyCache(const QJsonObject &authData)
{
    Q_UNUSED(authData);
    return true; // кэша нет — scout не нужен
}

void DirectLaunchAuth::startLauncher(QProcess *process,
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

    // Riot: в админке часто путают exe/args (путь к RiotClient в args, LeagueClient в exe).
    if (m_platformId == QLatin1String("riot")) {
        const QString defaultRiot =
            QStringLiteral("C:\\Riot Games\\Riot Client\\RiotClientServices.exe");
        const QString title = authData.value(QStringLiteral("game_title")).toString();

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
            if (recovered.contains(QStringLiteral("RiotClientServices"), Qt::CaseInsensitive)
                || !QFileInfo::exists(exe)) {
                qWarning() << "[RIOT] args выглядят как путь exe — переносим в exe_path:"
                           << recovered;
                exe = recovered;
            } else {
                qWarning() << "[RIOT] игнорируем path-like args:" << argsStr;
            }
            argsStr.clear();
        }

        if (!exe.contains(QStringLiteral("RiotClientServices"), Qt::CaseInsensitive)) {
            if (QFileInfo::exists(defaultRiot)) {
                qWarning() << "[RIOT] exe не RiotClientServices — подменяем на" << defaultRiot
                           << "(было:" << exe << ")";
                exe = defaultRiot;
            }
        }

        if (argsStr.isEmpty()) {
            if (title.contains(QStringLiteral("Valorant"), Qt::CaseInsensitive))
                argsStr = QStringLiteral("--launch-product=valorant --launch-patchline=live");
            else if (title.contains(QStringLiteral("League"), Qt::CaseInsensitive))
                argsStr = QStringLiteral("--launch-product=league_of_legends --launch-patchline=live");
        }
    }

    if (exe.isEmpty()) {
        qCritical() << "[" << m_platformId.toUpper() << "] exe_path пуст";
        return;
    }

    const QFileInfo fi(exe);
    m_launcherImage = fi.fileName();
    process->setWorkingDirectory(fi.absolutePath());

    // URI вида com.epicgames.launcher://... — один аргумент целиком
    QStringList args;
    if (argsStr.startsWith(QStringLiteral("com.epicgames.launcher://"), Qt::CaseInsensitive)
        || argsStr.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        || argsStr.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        args << argsStr;
    } else if (!argsStr.isEmpty()) {
        args = QProcess::splitCommand(argsStr);
    }

    qWarning().noquote() << "[" << m_platformId.toUpper() << "] DirectLaunch exe:" << exe;
    qWarning().noquote() << "[" << m_platformId.toUpper() << "] DirectLaunch args:" << args;
    process->start(exe, args);
}

void DirectLaunchAuth::startScout(const QString &, const QString &)
{
    // Интерактивный логин Epic/др. — следующий этап
}

void DirectLaunchAuth::stopScout()
{
}
