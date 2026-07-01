// Путь: src/core/processmanager.h
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
    Q_INVOKABLE void setSystemVolume(int level);
    Q_INVOKABLE void toggleSystemLanguage();

    explicit ProcessManager(QObject *parent = nullptr);
    ~ProcessManager();

    Q_INVOKABLE void launch(const QString &exePath, const QString &args);
    void setMainWindow(QWindow *window);

    void enableKioskMode();
    void disableKioskMode();

    void applyEnterprisePolicies(bool enable);
    void optimizeSystemServices();
    void purgeUserGarbage();


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