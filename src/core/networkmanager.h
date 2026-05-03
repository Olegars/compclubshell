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

    Q_INVOKABLE void fetchGames();
    Q_INVOKABLE void fetchProducts();
    Q_INVOKABLE void buyItem(int itemId, const QString &terminalId);
    Q_INVOKABLE void callAdmin(const QString &terminalId);

    // Метод для пинга
    Q_INVOKABLE int getLatency(const QString &host);

private slots:
    void onGamesFetched();
    void onProductsFetched();

private:
    QNetworkAccessManager *manager;
    GameModel *gamesModel;
    StoreModel *storeModel;
};