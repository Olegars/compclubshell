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
}

void DirectLaunchAuth::killLauncher()
{
    // Пока не убиваем чужие лаунчеры агрессивно — только при явном образе
    if (m_launcherImage.isEmpty())
        return;
#ifdef Q_OS_WIN
    auto *p = new QProcess;
    p->setProgram(QStringLiteral("taskkill"));
    p->setArguments({QStringLiteral("/F"), QStringLiteral("/T"),
                     QStringLiteral("/IM"), m_launcherImage});
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     p, &QObject::deleteLater);
    p->start();
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
