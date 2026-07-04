#include "networkmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QDebug>
#include <QProcess>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QUrlQuery>  // Добавлено для передачи параметров в GET запросе
#include <vector>
#include <QDir>
#include <QUrl>
#include <QFileInfo>
#include <QFile>

NetworkManager::NetworkManager(GameModel *gModel, StoreModel *sModel, QObject *parent)
    : QObject(parent), gamesModel(gModel), storeModel(sModel)
{
    manager = new QNetworkAccessManager(this);
    m_serverUrl = "http://192.168.222.2:22222";
    m_cachePath = "C:/ShellVideo/Cache/";
    QDir().mkpath(m_cachePath);
}

void NetworkManager::fetchTerminalConfig(const QString &hwid) {
    QNetworkRequest request(QUrl(m_serverUrl + "/api/shell/check"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject json;
    json["hwid"] = hwid;

    QNetworkReply *reply = manager->post(request, QJsonDocument(json).toJson());
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onTerminalConfigFetched);
}

void NetworkManager::onTerminalConfigFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject rootObj = doc.object();

        if (rootObj["status"].toString() == "success") {
            int computerId = rootObj["computer_id"].toInt();
            QString pcType = rootObj["type"].toString("standard");

            if (!qApp->property("engine").isNull()) {
                QQmlApplicationEngine *engine = qobject_cast<QQmlApplicationEngine*>(qApp->property("engine").value<QObject*>());
                if (engine && !engine->rootObjects().isEmpty()) {
                    QObject *rootQml = engine->rootObjects().first();
                    rootQml->setProperty("terminalId", computerId);
                    rootQml->setProperty("pcTypeFromDatabase", pcType);
                    rootQml->setProperty("isReady", true);
                    qDebug() << "[NET] Успешная авторизация железа. ID ПК:" << computerId << "| Зона:" << pcType;

                    QMetaObject::invokeMethod(rootQml, "fetchOverlays");
                }
            }
        } else {
            qDebug() << "[NET] Оборудование не зарегистрировано в панели REACTOR CONTROL";
        }
    } else {
        qDebug() << "[NET] Ошибка сети при запросе к API Shell:" << reply->errorString();
    }
    reply->deleteLater();
}

QString NetworkManager::getLocalPath(const QString &remotePath, const QString &target) {
    if (remotePath.isEmpty()) return "";

    QString fullUrl = remotePath;
    if (!remotePath.startsWith("http")) {
        fullUrl = m_serverUrl + "/" + QString(remotePath).remove(QRegularExpression("^/+"));
    }

    QUrl url(fullUrl);
    QString fileName = url.fileName();
    if (fileName.isEmpty()) {
        fileName = QString::number(qHash(remotePath)) + ".mp4";
    }
    QString localFilePath = m_cachePath + fileName;

    if (QFile::exists(localFilePath) && QFileInfo(localFilePath).size() > 0) {
        return QUrl::fromLocalFile(localFilePath).toString();
    }

    qDebug() << "[CACHE] Файла нет в кэше. Цель:" << (target.isEmpty() ? "SYSTEM" : target)
             << "| Качаем в фоне:" << fileName << "| Стримим напрямую из:" << fullUrl;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ReactorShell/1.0");
    QNetworkReply *reply = manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, localFilePath, remotePath, target]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            QFile file(localFilePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reply->readAll());
                file.close();
                qDebug() << "[CACHE] Фоновое скачивание завершено для" << target << ". Сохранено в:" << localFilePath;

                emit fileDownloaded(remotePath, "file:///" + localFilePath, target);
            }
        } else {
            qDebug() << "[CACHE ERROR] Не удалось скачать файл для" << target << ":" << reply->errorString();
        }
    });

    return fullUrl;
}

void NetworkManager::fetchGames() {
    QNetworkRequest request(QUrl(m_serverUrl + "/api/shell/games"));
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onGamesFetched);
}

void NetworkManager::onGamesFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray gamesArray = doc.array();

        std::vector<GameItem> games;
        for (const auto &val : gamesArray) {
            QJsonObject obj = val.toObject();
            GameItem item;
            item.id = obj["id"].toInt();
            item.title = obj["title"].toString();
            item.category = obj["category"].toString();
            item.platform = obj["platform"].toString();
            item.poster = obj["poster"].toString();
            item.exePath = obj["exe_path"].toString();
            item.args = obj["args"].toString();
            games.push_back(item);
        }
        gamesModel->setGames(games);
    }
    reply->deleteLater();
}

void NetworkManager::fetchProducts() {
    int terminalId = 0;
    if (!qApp->property("engine").isNull()) {
        QQmlApplicationEngine *engine = qobject_cast<QQmlApplicationEngine*>(qApp->property("engine").value<QObject*>());
        if (engine && !engine->rootObjects().isEmpty()) {
            terminalId = engine->rootObjects().first()->property("terminalId").toInt();
        }
    }

    QUrl url(m_serverUrl + "/api/shell/store/products");
    QUrlQuery query;
    if (terminalId > 0) {
        query.addQueryItem("terminal_id", QString::number(terminalId));
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onProductsFetched);
}

void NetworkManager::onProductsFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray responseData = reply->readAll();

        // КОНТРОЛЬ В КОНСОЛИ: Выводим то, что вернул доработанный бэкенд Laravel
        qDebug() << "[NET-TRACKER] Комбинированный ответ сервера:" << responseData;

        QJsonDocument doc = QJsonDocument::fromJson(responseData);
        QJsonObject rootObj = doc.object();

        // 1. Парсим статус активного заказа и прокидываем напрямую в корень Main.qml
        bool hasActiveOrder = rootObj["has_active_order"].toBool();
        QString statusText = rootObj["status_text"].toString();

        if (!qApp->property("engine").isNull()) {
            QQmlApplicationEngine *engine = qobject_cast<QQmlApplicationEngine*>(qApp->property("engine").value<QObject*>());
            if (engine && !engine->rootObjects().isEmpty()) {
                QObject *rootQml = engine->rootObjects().first();
                rootQml->setProperty("hasActiveOrder", hasActiveOrder);
                rootQml->setProperty("orderStatusText", statusText.isEmpty() ? "ЗАКАЗ В РАБОТЕ" : statusText.toUpper());
            }
        }

        // 2. Распаковываем массив товаров из ключа "products"
        QJsonArray productsArray = rootObj["products"].toArray();
        std::vector<StoreItem> products;

        for (const auto &val : productsArray) {
            QJsonObject obj = val.toObject();
            StoreItem item;
            item.id = obj["id"].toInt();
            item.name = obj["name"].toString();
            item.category = obj["category"].toString();

            // Железный парсинг цены из строки "90.00" / "180.00"
            if (obj["price"].isString()) {
                item.price = obj["price"].toString().toDouble();
            } else {
                item.price = obj["price"].toDouble();
            }

            item.image = obj["image"].toString();
            item.stock = obj["stock"].toInt();
            products.push_back(item);
        }
        storeModel->setProducts(products);
        qDebug() << "[NET] Модель обновлена. Успешно загружено товаров:" << products.size() << "| Статус заказа:" << hasActiveOrder;
    } else {
        qDebug() << "[NET ERROR] Не удалось получить маркет и статус заказа:" << reply->errorString();
    }
    reply->deleteLater();
}

void NetworkManager::buyItem(int itemId, int terminalId) {
    QNetworkRequest request(QUrl(m_serverUrl + "/api/shell/checkout"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject json;
    json["product_id"] = itemId;
    json["terminal_id"] = terminalId;
    manager->post(request, QJsonDocument(json).toJson());
}

void NetworkManager::callAdmin(int terminalId) {
    QNetworkRequest request(QUrl(m_serverUrl + "/api/shell/admin/call"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject json;
    json["terminal_id"] = terminalId;
    manager->post(request, QJsonDocument(json).toJson());
}

void NetworkManager::requestPausePin(int terminalId) {
    QNetworkRequest request(QUrl(m_serverUrl + "/api/shell/games/pause"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject json;
    json["computer_id"] = terminalId;

    QNetworkReply *reply = manager->post(request, QJsonDocument(json).toJson());
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onPausePinFetched);
}

void NetworkManager::onPausePinFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject rootObj = doc.object();

        if (rootObj["status"].toString() == "success") {
            QString pin = rootObj["pin_code"].toString();
            if (pin.isEmpty() && rootObj.contains("pin")) {
                pin = rootObj["pin"].toString();
            }

            if (!qApp->property("engine").isNull()) {
                QQmlApplicationEngine *engine = qobject_cast<QQmlApplicationEngine*>(qApp->property("engine").value<QObject*>());
                if (engine && !engine->rootObjects().isEmpty()) {
                    QObject *rootQml = engine->rootObjects().first();
                    rootQml->setProperty("temporaryPausePin", pin);
                    qDebug() << "[NET] Успешно получен PIN паузы из базы:" << pin;
                }
            }
        }
    } else {
        qDebug() << "[NET] Ошибка получения PIN паузы:" << reply->errorString();
    }
    reply->deleteLater();
}

int NetworkManager::getLatency(const QString &host) {
    Q_UNUSED(host);
    return 24 + (rand() % 4);
}

// Пустая заглушка — fetchProducts теперь обновляет статус и товары одновременно
void NetworkManager::checkOrderStatus(int terminalId) {
    Q_UNUSED(terminalId);
}