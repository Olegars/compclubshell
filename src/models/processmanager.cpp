#include "processmanager.h"

ProcessManager::ProcessManager(QObject *parent) : QObject(parent) {}

void ProcessManager::launch(const QString &path, const QString &args) {
    if (path.isEmpty()) return;

    QProcess *process = new QProcess(this);
    QFileInfo info(path);

    // Устанавливаем рабочую директорию, чтобы игра видела свои ресурсы
    process->setWorkingDirectory(info.absolutePath());

    // Запускаем процесс
    process->start(path, args.split(" ", Qt::SkipEmptyParts));

    // Автоматическая очистка памяти после завершения игры
    connect(process, &QProcess::finished, process, &QProcess::deleteLater);
}