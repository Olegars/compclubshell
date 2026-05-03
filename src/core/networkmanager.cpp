#include "networkmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QDebug>
#include <QProcess>
#include <vector>

// Конструктор: проверь, что в .h он такой же!
NetworkManager::NetworkManager(GameModel *gModel, StoreModel *sModel, QObject *parent)
    : QObject(parent), gamesModel(gModel), storeModel(sModel)
{
    manager = new QNetworkAccessManager(this);
}

void NetworkManager::fetchGames() {
    QNetworkRequest request(QUrl("http://127.0.0.1:22222/api/shell/games"));
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onGamesFetched);
}

void NetworkManager::onGamesFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray gamesArray = doc.object()["games"].toArray();
        std::vector<GameItem> games;
        for (const auto &val : gamesArray) {
            QJsonObject obj = val.toObject();
            GameItem item;
            item.id = obj["id"].toInt();
            item.title = obj["title"].toString();
            item.category = obj["category"].toString();
            item.platform = obj["platform"].toString();
            item.poster = obj["poster"].toString();
            item.exePath = obj["exe_path"].toString(); // CamelCase как в .h
            item.args = obj["launch_args"].toString();    // args как в .h
            games.push_back(item);
        }
        gamesModel->setGames(games);
    }
    reply->deleteLater();
}

void NetworkManager::fetchProducts() {
    QNetworkRequest request(QUrl("http://127.0.0.1:22222/api/shell/products"));
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onProductsFetched);
}

void NetworkManager::onProductsFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray productsArray = doc.object()["products"].toArray();
        std::vector<StoreItem> products;
        for (const auto &val : productsArray) {
            QJsonObject obj = val.toObject();
            StoreItem item;
            item.id = obj["id"].toInt();
            item.name = obj["name"].toString();
            item.category = obj["category"].toString();
            item.price = obj["price"].toDouble();
            item.image = obj["image"].toString();
            item.stock = obj["stock"].toInt();
            products.push_back(item);
        }
        storeModel->setProducts(products);
    }
    reply->deleteLater();
}

void NetworkManager::buyItem(int itemId, const QString &terminalId) {
    QNetworkRequest request(QUrl("http://127.0.0.1:22222/api/shell/store/checkout"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject json;
    json["product_id"] = itemId;
    json["terminal_id"] = terminalId;
    manager->post(request, QJsonDocument(json).toJson());
}

void NetworkManager::callAdmin(const QString &terminalId) {
    QNetworkRequest request(QUrl("http://127.0.0.1:22222/api/shell/admin/call"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject json;
    json["terminal_id"] = terminalId;
    manager->post(request, QJsonDocument(json).toJson());
}

int NetworkManager::getLatency(const QString &host) {
#ifdef Q_OS_WIN
    QProcess ping;
    ping.start("ping", QStringList() << "-n" << "1" << "-w" << "500" << host);
    if (!ping.waitForFinished(600)) return 999;
    QString output = QString::fromLocal8Bit(ping.readAllStandardOutput());
    QRegularExpression re("(?:время[=<]|time[=<])(\\d+)\\s*m?s");
    QRegularExpressionMatch match = re.match(output);
    if (match.hasMatch()) return match.captured(1).toInt();
#endif
    return 999;
}