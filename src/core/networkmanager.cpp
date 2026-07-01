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
#include <vector>
#include <QDir>
#include <QUrl>
#include <QFileInfo>
#include <QEventLoop>

NetworkManager::NetworkManager(GameModel *gModel, StoreModel *sModel, QObject *parent)
    : QObject(parent), gamesModel(gModel), storeModel(sModel)
{
    manager = new QNetworkAccessManager(this);
}

// =========================================================================
// ИНИЦИАЛИЗАЦИЯ ТЕРМИНАЛА (Запрос к /api/shell/check)
// =========================================================================
void NetworkManager::fetchTerminalConfig(const QString &hwid) {
    QNetworkRequest request(QUrl("http://192.168.222.2:22222/api/shell/check"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject json;
    json["hwid"] = hwid;

    QNetworkReply *reply = manager->post(request, QJsonDocument(json).toJson());
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onTerminalConfigFetched);
}

void NetworkManager::onTerminalConfigFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject rootObj = doc.object();

        if (rootObj["status"].toString() == "success") {
            int computerId = rootObj["computer_id"].toInt();
            QString pcType = rootObj["type"].toString("standard");

            // Запись параметров напрямую в свойства главного Main.qml
            if (!qApp->property("engine").isNull()) {
                QQmlApplicationEngine *engine = qobject_cast<QQmlApplicationEngine*>(qApp->property("engine").value<QObject*>());
                if (engine && !engine->rootObjects().isEmpty()) {
                    QObject *rootQml = engine->rootObjects().first();
                    rootQml->setProperty("terminalId", computerId);
                    rootQml->setProperty("pcTypeFromDatabase", pcType);
                    rootQml->setProperty("isReady", true); // Скрывает прелоадер
                    qDebug() << "[NET] Успешная авторизация железа. ID ПК:" << computerId << "| Зона:" << pcType;
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

// =========================================================================
// БИБЛИОТЕКА ИГР (/api/shell/games)
// =========================================================================
void NetworkManager::fetchGames() {
    QNetworkRequest request(QUrl("http://192.168.222.2:22222/api/shell/games"));
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onGamesFetched);
}

void NetworkManager::onGamesFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
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

// =========================================================================
// ТОВАРЫ МАГАЗИНА (/api/shell/products)
// =========================================================================
void NetworkManager::fetchProducts() {
    QNetworkRequest request(QUrl("http://192.168.222.2:22222/api/shell/products"));
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, &NetworkManager::onProductsFetched);
}

void NetworkManager::onProductsFetched() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray productsArray = doc.array();

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

// =========================================================================
// ПОКУПКА И ОБРАТНАЯ СВЯЗЬ
// =========================================================================
void NetworkManager::buyItem(int itemId, int terminalId) {
    QNetworkRequest request(QUrl("http://192.168.222.2:22222/api/shell/checkout"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject json;
    json["product_id"] = itemId;
    json["terminal_id"] = terminalId;
    manager->post(request, QJsonDocument(json).toJson());
}

void NetworkManager::callAdmin(int terminalId) {
    QNetworkRequest request(QUrl("http://192.168.222.2:22222/api/shell/admin/call"));
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


QString NetworkManager::getCachedVideoPath(const QString &remoteUrl) {
    if (remoteUrl.isEmpty()) return "";

    // Если бэкенд уже прислал локальный путь (C:/... или D:/...)
    if (remoteUrl.startsWith("C:") || remoteUrl.startsWith("D:")) {
        return "file:///" + remoteUrl;
    }

    // Задаем локальную папку для кэша видео оверлеев
    QString cacheDir = "C:/ShellVideo/Cache/";
    QDir().mkpath(cacheDir);

    // Хешируем имя файла из URL, чтобы не было конфликтов
    QUrl url(remoteUrl);
    QString fileName = url.fileName();
    if (fileName.isEmpty()) {
        fileName = QString::number(qHash(remoteUrl)) + ".mp4";
    }
    QString localFilePath = cacheDir + fileName;

    // Проверяем: если файл уже был скачан ранее — отдаем локальный путь без нагрузки на сеть!
    if (QFileInfo::exists(localFilePath) && QFileInfo(localFilePath).size() > 0) {
        return "file:///" + localFilePath;
    }

    // Если файла нет в кэше — скачиваем его (Синхронный легковесный прелоадер)
    QNetworkRequest request(url);
    QNetworkReply *reply = manager->get(request);

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec(); // Ждем скачивания файла

    if (reply->error() == QNetworkReply::NoError) {
        QFile file(localFilePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(reply->readAll());
            file.close();
            qDebug() << "[CACHE] Видео успешно кэшировано на диск:" << localFilePath;
        }
    } else {
        qDebug() << "[CACHE] Ошибка скачивания видео в кэш:" << reply->errorString();
        reply->deleteLater();
        return remoteUrl; // Если ошибка, откатываемся на онлайн поток
    }

    reply->deleteLater();
    return "file:///" + localFilePath;
}