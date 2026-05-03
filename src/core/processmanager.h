#pragma once

#include <QObject>
#include <QProcess>
#include <QWindow>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class ProcessManager : public QObject
{
    Q_OBJECT
public:
    explicit ProcessManager(QObject *parent = nullptr);
    ~ProcessManager();

    Q_INVOKABLE void launch(const QString &exePath, const QString &args);
    void setMainWindow(QWindow *window);

    void enableKioskMode();
    void disableKioskMode();

    // ENTERPRISE / LTSC ФУНКЦИИ
    void applyEnterprisePolicies(bool enable);
    void optimizeSystemServices(); // ЭТА СТРОКА ДОЛЖНА БЫТЬ ТУТ
    void purgeUserGarbage();       // Оставь для гибкости, если решишь не ребутать ПК

signals:
    void gameStarted();
    void gameFinished();

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    QProcess *m_process;
    QWindow *m_mainWindow;

#ifdef Q_OS_WIN
    static HHOOK keyboardHook;
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif
};