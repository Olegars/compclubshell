#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QProcess>
#include <QFileInfo>

class ProcessManager : public QObject
{
    Q_OBJECT
public:
    explicit ProcessManager(QObject *parent = nullptr);

    // Этот метод мы будем вызывать из QML: Launcher.launch(path, args)
    Q_INVOKABLE void launch(const QString &path, const QString &args);
};

#endif // PROCESSMANAGER_H