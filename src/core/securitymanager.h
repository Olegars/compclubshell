#ifndef SECURITYMANAGER_H
#define SECURITYMANAGER_H

#include <QObject>

class SecurityManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
public:
    explicit SecurityManager(QObject *parent = nullptr);
    ~SecurityManager();

    bool locked() const { return m_locked; }

    // Главный метод, запускающий весь комплекс защитных процедур Windows
    Q_INVOKABLE void lockDownSystem();

    // Откат для обслуживания образа / Super Client
    Q_INVOKABLE void unlockSystem();

signals:
    void lockedChanged();

private:
    // Внутренние утилиты для работы с реестром Windows (Registry)
    void setRegistryValue(const QString &keyPath, const QString &valueName, uint32_t value);
    void setRegistryString(const QString &keyPath, const QString &valueName, const QString &value);

    // Функции для управления конкретными уязвимостями
    void disableCmdAndRegistry();
    void disableTaskMgrAndCtrlAltDel();
    void setupCustomShell(bool enable);
    void disableStickyKeys();
    void startExplorer();

    bool m_locked = false;
};

#endif // SECURITYMANAGER_H