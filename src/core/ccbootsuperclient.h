#ifndef CCBOOTSUPERCLIENT_H
#define CCBOOTSUPERCLIENT_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QWindow>

class SecurityManager;

/**
 * Обслуживание гибридного образа с клиента: киоск снять, открыть
 * CCBoot Client и нажать Enable/Disable Super Client (пароль админа CCBoot).
 * Сохранение образа делает CCBoot, не booking.
 */
class CcbootSuperClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool clientFound READ clientFound NOTIFY statusChanged)
    Q_PROPERTY(QString clientPath READ clientPath NOTIFY statusChanged)
    Q_PROPERTY(bool superClientActive READ superClientActive NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY statusChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY statusChanged)
    Q_PROPERTY(bool kioskUnlocked READ kioskUnlocked NOTIFY statusChanged)

public:
    explicit CcbootSuperClient(SecurityManager *security, QObject *parent = nullptr);

    void setMainWindow(QWindow *window);

    bool clientFound() const { return !m_clientPath.isEmpty(); }
    QString clientPath() const { return m_clientPath; }
    bool superClientActive() const { return m_superClientActive; }
    bool busy() const { return m_busy; }
    QString lastMessage() const { return m_lastMessage; }
    bool lastOk() const { return m_lastOk; }
    bool kioskUnlocked() const { return m_kioskUnlocked; }

    Q_INVOKABLE void refresh();
    /** Снять киоск и показать explorer — правки образа после Super Client. */
    Q_INVOKABLE void unlockForMaintenance();
    Q_INVOKABLE void lockKiosk();
    /** diskMode: image | disk | both */
    Q_INVOKABLE void enableSuperClient(const QString &password, const QString &diskMode);
    Q_INVOKABLE void disableSuperClient(const QString &password, bool saveImage);
    Q_INVOKABLE void openCcbootClient();

signals:
    void statusChanged();
    void busyChanged();

private:
    enum class Phase {
        Idle,
        WaitMain,
        WaitTypeDialog,
        WaitPassword,
        WaitConfirm,
        WaitRebootPrompt,
        Failed
    };

    void setBusy(bool busy);
    void finish(bool ok, const QString &message);
    void startAutomation(bool enable, const QString &password, const QString &diskMode, bool saveImage);
    void tick();
    bool launchClient();
    void hideShellForDialogs();
    void restoreShellWindow();
    void detectClientPath();
    void detectSuperClientFlag();
    QString readIniValue(const QString &filePath, const QString &key) const;

    SecurityManager *m_security = nullptr;
    QWindow *m_mainWindow = nullptr;
    QTimer *m_tick = nullptr;

    QString m_clientPath;
    QString m_lastMessage;
    QString m_password;
    QString m_diskMode;
    bool m_lastOk = true;
    bool m_busy = false;
    bool m_kioskUnlocked = false;
    bool m_superClientActive = false;
    bool m_enable = true;
    bool m_saveImage = true;
    int m_ticks = 0;
    Phase m_phase = Phase::Idle;
    bool m_typedPassword = false;
    bool m_pickedType = false;
};

#endif
