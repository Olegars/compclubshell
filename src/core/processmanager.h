// Путь: src/core/processmanager.h
#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QWindow>
#include <QProcess>
#include <QTimer>

#ifdef Q_OS_WIN
#include <winsock2.h> // СТРОГО до windows.h для предотвращения конфликтов в MinGW
#include <windows.h>
#endif

class ProcessManager : public QObject
{
    Q_OBJECT

public:
    explicit ProcessManager(QObject *parent = nullptr);
    ~ProcessManager();

    void setMainWindow(QWindow *window);
    Q_INVOKABLE void applyQosPolicies(bool enable);
    Q_INVOKABLE void launch(const QString &exePath, const QString &args);
    Q_INVOKABLE void setSystemVolume(int level);
    Q_INVOKABLE void toggleSystemLanguage();

    // Фиксация решения пользователя из Попапа QML или нативного MessageBox
    Q_INVOKABLE void handleDownloadDecision(bool continueDownload);

    void enableKioskMode();
    void disableKioskMode();
    void applyEnterprisePolicies(bool enable);
    void purgeUserGarbage();
    void optimizeSystemServices();

signals:
    void gameStarted();
    void gameFinished();
    void heavyDownloadDetected(const QString &processName);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void monitorNetworkTraffic(); // Ежесекундный WinAPI трекер сокетов

private:
    QProcess *m_process;
    QWindow *m_mainWindow;

    // Переменные универсального сетевого шейпера
    QTimer *m_netWatchTimer;
    bool m_alertActive;
    unsigned long m_offendingPid;
    QString m_offendingProcessName;
    int m_highActivityCounter;

    // Вспомогательные методы
    unsigned long getProcessIdByName(const QString &processName);

#ifdef Q_OS_WIN
    static HHOOK keyboardHook;
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif
};

#endif // PROCESSMANAGER_H