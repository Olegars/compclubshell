#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include <QWindow>
#include <QRegularExpression>
#include "networkmanager.h"

class ProcessManager : public QObject
{
    Q_OBJECT
public:
    explicit ProcessManager(NetworkManager *netManager, QObject *parent = nullptr);
    ~ProcessManager();

    void setMainWindow(QWindow *window);
    void onGameWindowFound(quintptr hwnd, const QString &className);

public slots:
    void launchGameWithSmartAuth(const QJsonObject &authData, const QString &steamAppId = "");
    void backupAndSendVdfPayload();
    void applyQosPolicies(bool enable);
    void setSystemVolume(int level);
    void toggleSystemLanguage();
    void handleDownloadDecision(bool continueDownload);
    void applyEnterprisePolicies(bool enable);
    Q_INVOKABLE void launchGameWithSmartAuthString(const QString &jsonString, const QString &steamAppId = "");

signals:
    void gameStarted();
    void gameStartedSuccessfully();
    void gameFinished();
    void heavyDownloadDetected(const QString &source);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void monitorNetworkTraffic();
    void checkGameExit();

private:
    bool isProcessRunning(const QString &processName);
    unsigned long getProcessIdByName(const QString &processName);
    void killSteamProcesses();
    bool applySteamAuthFiles(const QJsonObject &authData, const QString &steamPath);
    QString localAppDataSteamVdfPath() const;
    QString buildLoginUsersVdf(const QString &steamId, const QString &login, const QString &persona, const QString &existing) const;
    void startAuthScout(const QString &login, const QString &password);
    void stopAuthScout();
    void startGameExitWatch(quintptr hwnd, const QString &className);
    void finishGameSession(const QString &reason);
    bool isGameWindowAlive() const;

    QProcess *m_process;
    QWindow *m_mainWindow;
    NetworkManager *m_netManager;
    QTimer *m_netWatchTimer;
    QTimer *m_gameExitTimer;
    QTimer *m_authScoutTimer;

    bool m_alertActive;
    unsigned long m_offendingPid;
    int m_highActivityCounter;

    int m_currentTerminalId;
    int m_currentGameId;
    QString m_currentSteamLogin;

    bool m_gameSessionActive;
    quintptr m_gameHwnd;
    QString m_gameWindowClass;
    int m_gameGoneTicks;
    bool m_needVdfBackup; // true только после интерактивного логина (scout) / нет кэша
    int m_scoutTicks;
    bool m_scoutInjected;
};

#endif // PROCESSMANAGER_H
