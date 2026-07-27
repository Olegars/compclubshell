#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include <QWindow>
#include "networkmanager.h"

class IPlatformAuth;
class ShellToggleWindow;

class ProcessManager : public QObject
{
    Q_OBJECT
    // Session still alive (game process/window watched). Independent of shell visibility.
    Q_PROPERTY(bool hasActiveGame READ hasActiveGame NOTIFY hasActiveGameChanged)
    Q_PROPERTY(QString gameTitle READ gameTitle NOTIFY gameTitleChanged)
    // true while shell is Hidden and floating toggle is shown over the game
    Q_PROPERTY(bool shellHiddenForGame READ shellHiddenForGame NOTIFY shellHiddenForGameChanged)
public:
    explicit ProcessManager(NetworkManager *netManager, QObject *parent = nullptr);
    ~ProcessManager();

    void setMainWindow(QWindow *window);
    void onGameWindowFound(quintptr hwnd, const QString &className);
    Q_INVOKABLE void setShellTopmost(bool enabled);
    Q_INVOKABLE void hideShellForGame();
    // Ends visual return after game exit — NOT for mid-session shell toggle.
    Q_INVOKABLE void showShellAfterGame();
    // Mid-session: restore shell fullscreen WITHOUT ending game / backup / forceKill.
    Q_INVOKABLE void showShellKeepGame();
    Q_INVOKABLE void switchToShell(); // alias → showShellKeepGame
    Q_INVOKABLE void switchToGame();  // hide shell again, focus game, show float btn
    // true while club/personal session launch is in progress (blocks Dashboard tile re-clicks)
    Q_INVOKABLE bool isSessionBusy() const;

    bool hasActiveGame() const;
    QString gameTitle() const;
    bool shellHiddenForGame() const;

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

    // Первый существующий путь из списка (Battle.net / Ubisoft / Lesta / VK …)
    Q_INVOKABLE void launchFirstExisting(const QStringList &candidatePaths,
                                         const QString &args = QString());

    void backupAndSendVdfPayload();
    void applyQosPolicies(bool enable);
    Q_INVOKABLE void setSystemVolume(int level);
    void toggleSystemLanguage();
    void handleDownloadDecision(bool continueDownload);
    void applyEnterprisePolicies(bool enable);
    Q_INVOKABLE void rebootPC();
    // Clear/blur game search before SendInput credentials and when shell returns
    Q_INVOKABLE void requestClearGameSearch();

signals:
    void gameStarted();
    void gameStartedSuccessfully();
    void gameFinished();
    void heavyDownloadDetected(const QString &source);
    void hasActiveGameChanged();
    void gameTitleChanged();
    void shellHiddenForGameChanged();
    void clearGameSearchRequested();

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void monitorNetworkTraffic();
    void checkGameExit();
    void pollForGameWindow();
    void reassertShellToggleTopmost();

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
    void setShellHiddenForGame(bool hidden);
    void setHasActiveGame(bool active);
    void setGameTitle(const QString &title);
    void showShellToggle(bool show);
    void focusGameWindow();
    void restoreShellUi(bool endSessionPath);

    QProcess *m_process;
    QWindow *m_mainWindow;
    NetworkManager *m_netManager;
    IPlatformAuth *m_platformAuth;
    QTimer *m_netWatchTimer;
    QTimer *m_gameExitTimer;
    QTimer *m_gameFindTimer;
    QTimer *m_shellToggleTopmostTimer = nullptr;
    ShellToggleWindow *m_shellToggle = nullptr;

    bool m_alertActive;
    unsigned long m_offendingPid;
    int m_highActivityCounter;

    int m_currentTerminalId;
    int m_currentGameId;
    int m_currentAccountId = 0;
    QString m_currentLogin;
    QString m_currentPlatform;
    QString m_gameTitle;
    bool m_personalAccount = false;
    // Личный логин: ждём окно игры, НЕ watch PID лаунчера как «игру»
    bool m_personalLoginWait = false;

    bool m_gameSessionActive;
    bool m_hasActiveGame = false; // accepted game window / switchable session
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
