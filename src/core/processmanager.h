// Путь: src/core/processmanager.h
#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QWindow>
#include <QProcess>
#include <QTimer>
#include "networkmanager.h" // Подключаем инфраструктуру сети REACTOR

#ifdef Q_OS_WIN
#include <winsock2.h> // СТРОГО до windows.h для предотвращения конфликтов в MinGW
#include <windows.h>
#endif

class ProcessManager : public QObject
{
    Q_OBJECT

public:
    // Конструктор теперь жестко требует указатель на NetworkManager для обработки логаутов
    explicit ProcessManager(NetworkManager *netManager, QObject *parent = nullptr);
    ~ProcessManager();

    void setMainWindow(QWindow *window);

    // СИСТЕМНЫЕ КНОПКИ И ОПТИМИЗАЦИИ ИНТЕРФЕЙСА
    Q_INVOKABLE void applyQosPolicies(bool enable);
    Q_INVOKABLE void setSystemVolume(int level);
    Q_INVOKABLE void toggleSystemLanguage();
    Q_INVOKABLE void handleDownloadDecision(bool continueDownload);

    // ЕДИНСТВЕННЫЙ И УНИВЕРСАЛЬНЫЙ МЕТОД LAUNCH
    // Заменяет старую версию, убирая конфликт двусмысленности при компиляции MOC
    Q_INVOKABLE void launch(const QString &exePath,
                            const QString &args,
                            const QString &login = "",
                            const QString &steamId = "",
                            const QString &token = "");

    // МЕТОДЫ УПРАВЛЕНИЯ КИОСКОМ И БЕЗОПАСНОСТЬЮ
    void enableKioskMode();
    void disableKioskMode();
    void applyEnterprisePolicies(bool enable);
    void purgeUserGarbage();
    void optimizeSystemServices();

    // ИНЖЕКЦИЯ ТОКЕНА АВТOРИЗАЦИИ VALVE
    void writeSteamToken(const QString &login, const QString &steamId, const QString &token);

signals:
    void gameStarted();
    void gameFinished();
    void heavyDownloadDetected(const QString &processName);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void monitorNetworkTraffic();

private:
    bool isProcessRunning(const QString &processName);
    unsigned long getProcessIdByName(const QString &processName);
    QString generateSteam2FA(const QString &sharedSecret);

    QProcess *m_process;
    QWindow *m_mainWindow;
    NetworkManager *m_netManager; // Указатель на сетевой модуль для связи с Laravel

    QTimer *m_netWatchTimer;
    bool m_alertActive;
    unsigned long m_offendingPid;
    QString m_offendingProcessName;
    int m_highActivityCounter;
    int m_currentTerminalId;     // Фиксация текущего ID ПК клуба для логаута

#ifdef Q_OS_WIN
    static HHOOK keyboardHook;
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif
};

#endif // PROCESSMANAGER_H