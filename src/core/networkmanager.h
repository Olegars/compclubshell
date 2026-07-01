#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include "../models/gamemodel.h"
#include "../models/storemodel.h"

class NetworkManager : public QObject
{
    Q_OBJECT
public:
    explicit NetworkManager(GameModel *gModel, StoreModel *sModel, QObject *parent = nullptr);

    // Метод для инициализации терминала по HWID серийнику
    Q_INVOKABLE void fetchTerminalConfig(const QString &hwid);

    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchProducts();
    Q_INVOKABLE void buyItem(int itemId, int terminalId);
    Q_INVOKABLE void callAdmin(int terminalId);

    // Метод для пинга
    Q_INVOKABLE int getLatency(const QString &host);
    // Добавить в src/core/networkmanager.h
    Q_INVOKABLE QString getCachedVideoPath(const QString &remoteUrl);

private slots:
    void onTerminalConfigFetched();
    void onGamesFetched();
    void onProductsFetched();

private:
    QNetworkAccessManager *manager;
    GameModel *gamesModel;
    StoreModel *storeModel;
};