#ifndef SECURITYMANAGER_H
#define SECURITYMANAGER_H

#include <QObject>

class SecurityManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    Q_PROPERTY(bool production READ production CONSTANT)
public:
    explicit SecurityManager(QObject *parent = nullptr);
    ~SecurityManager();

    bool locked() const { return m_locked; }
    bool production() const { return m_production; }
    void setProduction(bool on) { m_production = on; }

    // Главный метод, запускающий весь комплекс защитных процедур Windows
    Q_INVOKABLE void lockDownSystem();

    // Только из C++: Super Client и режим разработки. Из QML не вызывается.
    void unlockSystem();

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
    bool m_production = false;
};

#endif // SECURITYMANAGER_H