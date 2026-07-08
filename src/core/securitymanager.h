#ifndef SECURITYMANAGER_H
#define SECURITYMANAGER_H

#include <QObject>

class SecurityManager : public QObject
{
    Q_OBJECT
public:
    explicit SecurityManager(QObject *parent = nullptr);
    ~SecurityManager();

    // Главный метод, запускающий весь комплекс защитных процедур Windows
    void lockDownSystem();

    // Метод для отката изменений (если понадобится для административного обслуживания)
    void unlockSystem();

private:
    // Внутренние утилиты для работы с реестром Windows (Registry)
    void setRegistryValue(const QString &keyPath, const QString &valueName, uint32_t value);
    void setRegistryString(const QString &keyPath, const QString &valueName, const QString &value);

    // Функции для управления конкретными уязвимостями
    void disableCmdAndRegistry();
    void disableTaskMgrAndCtrlAltDel();
    void setupCustomShell(bool enable);
    void disableStickyKeys();
};

#endif // SECURITYMANAGER_H