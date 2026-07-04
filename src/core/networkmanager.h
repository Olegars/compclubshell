#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QDir>
#include <QCoreApplication>
#include "../models/gamemodel.h"
#include "../models/storemodel.h"

class NetworkManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)

signals:
    // Сигнал теперь передает еще и target (имя оверлея или "bg" для главного фона)
    void fileDownloaded(const QString &remoteUrl, const QString &localPath, const QString &target);

public:
    explicit NetworkManager(GameModel *gModel, StoreModel *sModel, QObject *parent = nullptr);

    QString serverUrl() const { return m_serverUrl; }

    Q_INVOKABLE void fetchTerminalConfig(const QString &hwid);
    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchProducts();
    Q_INVOKABLE void buyItem(int itemId, int terminalId);
    Q_INVOKABLE void callAdmin(int terminalId);
    Q_INVOKABLE int getLatency(const QString &host);

    // Метод генерации одноразового ПИН-кода для паузы
    Q_INVOKABLE void requestPausePin(int terminalId);
    Q_INVOKABLE void checkOrderStatus(int terminalId);
    // Добавили аргумент target с дефолтным значением для совместимости
    Q_INVOKABLE QString getLocalPath(const QString &remotePath, const QString &target = "");

private slots:
    void onTerminalConfigFetched();
    void onGamesFetched();
    void onProductsFetched();
    void onPausePinFetched();

private:
    QNetworkAccessManager *manager;
    GameModel *gamesModel;
    StoreModel *storeModel;
    QString m_serverUrl;
    QString m_cachePath;
};