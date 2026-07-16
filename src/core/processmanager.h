#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include <QWindow>
#include "networkmanager.h"

class IPlatformAuth;

class ProcessManager : public QObject
{
    Q_OBJECT
public:
    explicit ProcessManager(NetworkManager *netManager, QObject *parent = nullptr);
    ~ProcessManager();

    void setMainWindow(QWindow *window);
    void onGameWindowFound(quintptr hwnd, const QString &className);
    Q_INVOKABLE void setShellTopmost(bool enabled);
    Q_INVOKABLE void hideShellForGame();
    Q_INVOKABLE void showShellAfterGame();

public slots:
    // Универсальный вход: authData.platform = steam|epic|direct|…
    void launchPlatformSession(const QJsonObject &authData, const QString &appIdHint = QString());
    Q_INVOKABLE void launchPlatformSessionString(const QString &jsonString,
                                                 const QString &appIdHint = QString());

    // Совместимость со старым QML
    void launchGameWithSmartAuth(const QJsonObject &authData, const QString &steamAppId = QString());
    Q_INVOKABLE void launchGameWithSmartAuthString(const QString &jsonString,
                                                   const QString &steamAppId = QString());

    // Быстрый старт exe без take-account (кнопки EPIC/RIOT в Dashboard)
    Q_INVOKABLE void launch(const QString &exePath,
                            const QString &args = QString(),
                            const QString &a = QString(),
                            const QString &b = QString(),
                            const QString &c = QString());

    void backupAndSendVdfPayload();
    void applyQosPolicies(bool enable);
    void setSystemVolume(int level);
    void toggleSystemLanguage();
    void handleDownloadDecision(bool continueDownload);
    void applyEnterprisePolicies(bool enable);

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
    void pollForGameWindow();

private:
    IPlatformAuth *createPlatformAuth(const QString &platform);
    bool isProcessRunning(const QString &processName);
    unsigned long getProcessIdByName(const QString &processName);
    void startGameFindPoll();
    void startGameExitWatch(quintptr hwnd, const QString &className);
    void acceptGameWindow(quintptr hwnd, const QString &className);
    void finishGameSession(const QString &reason);
    bool isGameWindowAlive() const;
    bool findAliveGameWindow(quintptr *outHwnd = nullptr) const;

    QProcess *m_process;
    QWindow *m_mainWindow;
    NetworkManager *m_netManager;
    IPlatformAuth *m_platformAuth;
    QTimer *m_netWatchTimer;
    QTimer *m_gameExitTimer;
    QTimer *m_gameFindTimer;

    bool m_alertActive;
    unsigned long m_offendingPid;
    int m_highActivityCounter;

    int m_currentTerminalId;
    int m_currentGameId;
    QString m_currentLogin;
    QString m_currentPlatform;

    bool m_gameSessionActive;
    quintptr m_gameHwnd;
    QString m_gameWindowClass;
    quint32 m_gamePid = 0;
    QString m_gameProcessImage;
    int m_gameGoneTicks;
    qint64 m_gameAcceptedAtMs = 0;
    // Окно игры, пришедшее во время логина Epic (пока allowsGameDetect=false)
    quintptr m_pendingGameHwnd = 0;
    QString m_pendingGameClass;
    bool m_shellHiddenForGame = false;
    int m_hideShellGeneration = 0;
};

#endif // PROCESSMANAGER_H
