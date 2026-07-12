// Путь: src/core/processmanager.h
#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QWindow>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include "networkmanager.h" // Подключаем инфраструктуру сети REACTOR

#ifdef Q_OS_WIN
#include <winsock2.h> // СТРОГО до windows.h для предотвращения конфликтов в MinGW
#include <windows.h>
#endif

class ProcessManager : public QObject
{
    Q_OBJECT

public:
    // Конструктор жестко требует указатель на NetworkManager для обработки сессий и логаутов
    explicit ProcessManager(NetworkManager *netManager, QObject *parent = nullptr);
    ~ProcessManager();

    void setMainWindow(QWindow *window);

    // СИСТЕМНЫЕ КНОПКИ И ОПТИМИЗАЦИИ ИНТЕРФЕЙСА
    Q_INVOKABLE void applyQosPolicies(bool enable);
    Q_INVOKABLE void setSystemVolume(int level);
    Q_INVOKABLE void toggleSystemLanguage();
    Q_INVOKABLE void handleDownloadDecision(bool continueDownload);

    // УМНЫЙ АВТОНОМНЫЙ ЗАПУСК С УЧЕТОМ КЭША И WinAPI ИНЖЕКЦИИ
    Q_INVOKABLE void launchGameWithSmartAuth(const QJsonObject &authData, const QString &steamAppId);

    // МЕТОДЫ УПРАВЛЕНИЯ КИОСКОМ И БЕЗОПАСНОСТЬЮ
    void enableKioskMode();
    void disableKioskMode();
    void applyEnterprisePolicies(bool enable);

signals:
    void gameStarted();
    void gameStartedSuccessfully(); // Сигнал для QML, чтобы гасить оверлей с часами
    void gameFinished();
    void heavyDownloadDetected(const QString &processName);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void monitorNetworkTraffic();

private:
    bool isProcessRunning(const QString &processName);
    unsigned long getProcessIdByName(const QString &processName);

    QProcess *m_process;
    QWindow *m_mainWindow;
    NetworkManager *m_netManager; // Указатель на сетевой модуль для связи с Laravel

    QTimer *m_netWatchTimer;
    bool m_alertActive;
    unsigned long m_offendingPid;
    int m_highActivityCounter;
    int m_currentTerminalId;     // Фиксация текущего ID ПК клуба для логаута

#ifdef Q_OS_WIN
    static HHOOK keyboardHook;
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif
};

#endif // PROCESSMANAGER_H