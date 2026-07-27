#include "networkmanager.h"
#include "hwidprovider.h"
#include "../models/gamemodel.h"
#include "../models/storemodel.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QDateTime>
#include <QUrlQuery>
#include <QHash>
#include <QPair>
#include <QVariantMap>
#include <QVariantList>
#include <QtGlobal>
#include <algorithm>
#include <vector>

NetworkManager::NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent)
    : QObject(parent)
    , m_isPcRegistered(false)
    , m_computerId(0)
    , m_lastBookingId(0)
    , m_userId(0)
    , m_featuredLabel(QStringLiteral("Популярно в клубе"))
    , m_featuredMode(QStringLiteral("club"))
    , m_gamesModel(gamesModel)
    , m_featuredGamesModel(nullptr)
    , m_storeModel(storeModel)
    , m_rootQml(nullptr)
{
    m_networkManager = new QNetworkAccessManager(this);

    QString pathCurrent = QCoreApplication::applicationDirPath() + "/config.ini";
    QString pathUp = QCoreApplication::applicationDirPath() + "/../config.ini";
    QString pathTwoUp = QCoreApplication::applicationDirPath() + "/../../config.ini";

    if (QFile::exists(pathCurrent)) {
        m_configFilePath = pathCurrent;
    } else if (QFile::exists(pathUp)) {
        m_configFilePath = pathUp;
    } else if (QFile::exists(pathTwoUp)) {
        m_configFilePath = pathTwoUp;
    } else {
        m_configFilePath = pathCurrent;
    }

    QSettings settings(m_configFilePath, QSettings::IniFormat);
    QString apiIp = settings.value("Network/api_ip", "192.168.222.2").toString().trimmed();
    QString apiPort = settings.value("Network/api_port", "22222").toString().trimmed();
    m_serverUrl = "http://" + apiIp + ":" + apiPort;

    m_cachePath = "C:/ShellVideo/Cache/";
    QDir().mkpath(m_cachePath);

    qDebug() << "[REACTOR-SHELL] Путь к кэшу оверлеев:" << m_cachePath;
    qDebug() << "[REACTOR-SHELL] Инициализация сети завершена. Сервер:" << m_serverUrl;
}

bool NetworkManager::isPcRegistered() const {
    return m_isPcRegistered;
}

QString NetworkManager::serverUrl() const {
    return m_serverUrl;
}

QString NetworkManager::getMachineHwid() const
{
    return HwidProvider::machineHwid();
}

double NetworkManager::jsonToDouble(const QJsonValue &value, double defaultValue)
{
    if (value.isDouble())
        return value.toDouble(defaultValue);
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        return ok ? parsed : defaultValue;
    }
    if (value.isBool() || value.isNull() || value.isUndefined() || value.isArray() || value.isObject())
        return defaultValue;
    return value.toVariant().toDouble();
}

double NetworkManager::userBalanceFromJson(const QJsonObject &user)
{
    static const char *keys[] = {
        "balance", "deposit_balance", "total_balance", "money", "wallet_balance"
    };
    for (const char *key : keys) {
        if (!user.contains(QLatin1String(key)))
            continue;
        const double value = jsonToDouble(user.value(QLatin1String(key)), -1.0);
        if (value >= 0.0)
            return value;
    }

    if (user.value(QStringLiteral("wallet")).isObject()) {
        const QJsonObject wallet = user.value(QStringLiteral("wallet")).toObject();
        for (const char *key : keys) {
            if (!wallet.contains(QLatin1String(key)))
                continue;
            const double value = jsonToDouble(wallet.value(QLatin1String(key)), -1.0);
            if (value >= 0.0)
                return value;
        }
        const double bonus = jsonToDouble(wallet.value(QStringLiteral("bonus_balance")), 0.0);
        if (bonus > 0.0)
            return bonus;
    }

    return 0.0;
}

QString NetworkManager::cleanDigits(const QString &value)
{
    QString digits;
    digits.reserve(value.size());
    for (const QChar ch : value) {
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }
    return digits;
}

void NetworkManager::fetchTerminalConfig(const QString &hwid) {
    m_hwid = hwid;
    qDebug() << "[REACTOR-SHELL] Нативный HWID зафиксирован:" << m_hwid;
}

void NetworkManager::checkTerminalStatus() {
    if (m_hwid.isEmpty() || m_hwid == "UNKNOWN_HWID_FALLBACK") {
        qWarning() << "[REACTOR-SHELL] Ошибка: HWID пустой, невозможно проверить статус в БД.";
        emit setupRequired();
        return;
    }

    QUrl url(m_serverUrl + "/api/shell/check");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject json;
    json["hwid"] = m_hwid;

    QJsonDocument doc(json);
    QByteArray data = doc.toJson();

    qDebug() << "[REACTOR-SHELL] Запрос статуса железа на бэкенд:" << url.toString();

    QNetworkReply* reply = m_networkManager->post(request, data);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[REACTOR-SHELL] Сетевая ошибка при опросе бэкенда:" << reply->errorString();
            emit setupRequired();
            return;
        }

        QByteArray responseData = reply->readAll();
        QJsonDocument responseDoc = QJsonDocument::fromJson(responseData);
        QJsonObject responseObj = responseDoc.object();

        if (responseObj.value("status").toString() == "success") {
            m_computerId = responseObj.value("computer_id").toInt();
            emit computerIdChanged();

            QString dbName = responseObj.value("name").toString().trimmed();
            m_pcNameString = dbName.isEmpty() ? ("PC-" + QString::number(m_computerId)) : dbName;

            m_isPcRegistered = true;

            qDebug() << "[REACTOR-SHELL] Терминал авторизован под именем:" << m_pcNameString << "| ID записи в БД:" << m_computerId;

            emit pcRegistrationChanged();
            emit authRequired();

            const int overlayTerminalId = m_computerId > 0 ? m_computerId : 1;
            fetchOverlays(overlayTerminalId);
        } else {
            qDebug() << "[REACTOR-SHELL] Оборудование не зарегистрировано. Переключение на Setup.";
            m_pcNameString = "PC-UNKNOWN";
            m_isPcRegistered = false;
            emit pcRegistrationChanged();
            emit setupRequired();
        }
    });
}

QString NetworkManager::getCurrentPcName() {
    return m_pcNameString.isEmpty() ? "PC-UNKNOWN" : m_pcNameString;
}

int NetworkManager::computerId() const {
    return m_computerId;
}

void NetworkManager::registerStation(const QString &zoneType, const QString &pcName) {
    if (m_serverUrl.isEmpty()) {
        qDebug() << "[CRITICAL-C++ SETUP] Ошибка: m_serverUrl пустой! Запрос отменён.";
        return;
    }

    QUrl url(m_serverUrl + "/api/shell/register-terminal");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    QJsonObject json;
    json["hwid"] = m_hwid;
    json["zone_type"] = zoneType;
    json["name"] = pcName;

    QJsonDocument doc(json);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    qDebug() << "[DEBUG-C++ SETUP] ---> ОТПРАВКА ПОСТ-ЗАПРОСА РЕГИСТРАЦИИ";
    qDebug() << "[DEBUG-C++ SETUP] URL:" << url.toString();
    qDebug() << "[DEBUG-C++ SETUP] JSON DATA:" << jsonData;

    QNetworkReply *reply = m_networkManager->post(request, jsonData);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        qDebug() << "[DEBUG-C++ SETUP] <--- ОТВЕТ СЕТИ ПОЛУЧЕН";

        QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (statusCode.isValid()) {
            qDebug() << "[DEBUG-C++ SETUP] HTTP Status Code:" << statusCode.toInt();
        }

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray responseData = reply->readAll();
            qDebug() << "[DEBUG-C++ SETUP] Сырой ответ от Laravel:" << responseData;

            QJsonDocument responseDoc = QJsonDocument::fromJson(responseData);
            QJsonObject responseJson = responseDoc.object();

            if (responseJson["status"].toString() == "success") {
                qDebug() << "[DEBUG-C++ SETUP] УСПЕХ: База обновилась. Запускаем переопрос терминала...";
                this->checkTerminalStatus();
            } else {
                qDebug() << "[DEBUG-C++ SETUP] ВНИМАНИЕ: Бэкенд ответил JSON-ом, но статус не success! Текст:" << responseDoc.toJson();
            }
        } else {
            qDebug() << "[DEBUG-C++ SETUP] КРИТИЧЕСКАЯ СЕТЕВАЯ ОШИБКА ОПЕРАЦИИ:" << reply->errorString();
            qDebug() << "[DEBUG-C++ SETUP] Ответ сервера при ошибке:" << reply->readAll();
        }
    });
}

// ДОБАВЛЕННЫЙ МЕТОД: ПОЛНОЕ ЗАКРЫТИЕ СЕССИИ И ОЧИСТКА СОСТОЯНИЯ ТЕРМИНАЛА
void NetworkManager::logoutTerminal(int terminalId) {
    if (m_serverUrl.isEmpty()) return;

    QUrl url(m_serverUrl + "/api/shell/logout");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    QJsonObject json;
    json["terminal_id"] = terminalId;

    QJsonDocument doc(json);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    qDebug() << "[DEBUG-C++ LOGOUT] ---> ОТПРАВКА ЗАПРОСА НА ЗАКРЫТИЕ СЕССИИ. ID терминала:" << terminalId;

    QNetworkReply *reply = m_networkManager->post(request, jsonData);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray responseData = reply->readAll();
            QJsonDocument responseDoc = QJsonDocument::fromJson(responseData);
            QJsonObject responseJson = responseDoc.object();

            if (responseJson["status"].toString() == "success") {
                qDebug() << "[DEBUG-C++ LOGOUT] <--- СЕССИЯ УСПЕШНО ЗАКРЫТА НА БЭКЕНДЕ";
                clearSessionUser();
                // Сбрасываем кэш-имя и принудительно обновляем статус харда
                this->checkTerminalStatus();
            } else {
                qDebug() << "[DEBUG-C++ LOGOUT] Внимание: Бэкенд вернул ошибку логаута:" << responseDoc.toJson();
            }
        } else {
            qWarning() << "[DEBUG-C++ LOGOUT] Критическая сетевая ошибка при логауте:" << reply->errorString();
        }
    });
}

QString NetworkManager::getLocalPath(const QString &remotePath, const QString &target) {
    if (remotePath.isEmpty()) return "";

    QString fileName = remotePath.split('/').last();
    if (fileName.isEmpty()) fileName = "overlay_video.mp4";
    QString localFilePath = m_cachePath + fileName;

    if (QFile::exists(localFilePath) && QFileInfo(localFilePath).size() > 0) {
        return QUrl::fromLocalFile(localFilePath).toString();
    }

    if (m_activeDownloads.contains(target)) {
        return "";
    }

    m_activeDownloads.append(target);

    QString fullUrl = remotePath;
    if (!remotePath.startsWith("http")) {
        QString cleanRemote = remotePath;
        if (cleanRemote.startsWith("/")) cleanRemote.remove(0, 1);
        fullUrl = m_serverUrl + "/" + cleanRemote;
    }

    qDebug() << "[CACHE-OPTIMIZED] Запуск одиночного скачивания файла для зоны:" << target << "URL:" << fullUrl;

    QNetworkRequest request((QUrl(fullUrl)));
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 ReactorShell/1.0");

    QNetworkReply *reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, localFilePath, remotePath, target]() {
        reply->deleteLater();

        this->m_activeDownloads.removeAll(target);

        if (reply->error() == QNetworkReply::NoError) {
            QFile file(localFilePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reply->readAll());
                file.close();
                qDebug() << "[CACHE] Фоновое скачивание успешно завершено для зоны:" << target;

                QString qmlSafePath = QUrl::fromLocalFile(localFilePath).toString();
                emit fileDownloaded(remotePath, qmlSafePath, target);
            }
        } else {
            qWarning() << "[CACHE] Ошибка скачивания оверлея для" << target << ":" << reply->errorString();
        }
    });

    return "";
}

int NetworkManager::getLatency(const QString &host) {
    Q_UNUSED(host);
    return 24 + (rand() % 4);
}

QStringList NetworkManager::getAvailableZones() {
    return QStringList() << "Single" << "Duo" << "Trio" << "Quatro" << "Bootcamp";
}


void NetworkManager::fetchGames() {
    if (m_serverUrl.isEmpty()) return;

    QUrl url(m_serverUrl + "/api/shell/games");
    if (m_userId > 0) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("user_id"), QString::number(m_userId));
        url.setQuery(q);
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            applyGamesPayload(QJsonDocument::fromJson(reply->readAll()));
            emit gamesLoaded();
        } else {
            qWarning() << "[NET] Ошибка получения списка игр:" << reply->errorString();
        }
    });
}

void NetworkManager::applyGamesPayload(const QJsonDocument &doc)
{
    auto parseItem = [](const QJsonObject &obj) {
        GameItem item;
        // Prefer id; accept game_id so featured payloads never land as id=0 (breaks catalog sync).
        item.id = obj.value(QStringLiteral("id")).toInt();
        if (item.id <= 0)
            item.id = obj.value(QStringLiteral("game_id")).toInt();
        item.title = obj.value(QStringLiteral("title")).toString();
        item.exePath = obj.value(QStringLiteral("exe_path")).toString();
        item.args = obj.value(QStringLiteral("args")).toString();
        item.poster = obj.value(QStringLiteral("poster")).toString();
        item.category = obj.value(QStringLiteral("category")).toString();
        item.platform = obj.value(QStringLiteral("platform")).toString();
        return item;
    };

    auto parseArray = [&parseItem](const QJsonArray &gamesArray) {
        std::vector<GameItem> gamesVector;
        gamesVector.reserve(static_cast<size_t>(gamesArray.size()));
        for (const QJsonValue &value : gamesArray)
            gamesVector.push_back(parseItem(value.toObject()));
        return gamesVector;
    };

    QJsonArray gamesArray;
    QJsonArray featuredArray;
    QString featuredLabel = QStringLiteral("Популярно в клубе");
    QString featuredMode = QStringLiteral("club");

    if (doc.isArray()) {
        gamesArray = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        gamesArray = root.value(QStringLiteral("games")).toArray();
        const QJsonObject featured = root.value(QStringLiteral("featured")).toObject();
        featuredArray = featured.value(QStringLiteral("games")).toArray();
        const QString label = featured.value(QStringLiteral("label")).toString();
        const QString mode = featured.value(QStringLiteral("mode")).toString();
        if (!label.isEmpty())
            featuredLabel = label;
        if (!mode.isEmpty())
            featuredMode = mode;
    }

    auto allGames = parseArray(gamesArray);
    auto featuredGames = parseArray(featuredArray);
    if (static_cast<int>(featuredGames.size()) > GameModel::kMaxFeatured)
        featuredGames.resize(static_cast<size_t>(GameModel::kMaxFeatured));

    // Keep featured title/poster/launch fields in sync with the main catalog by id
    // (avoids mismatched poster vs label when featured payload is incomplete/stale).
    {
        QHash<int, GameItem> byId;
        byId.reserve(static_cast<int>(allGames.size()));
        for (const auto &g : allGames) {
            if (g.id > 0)
                byId.insert(g.id, g);
        }

        featuredGames.erase(
            std::remove_if(featuredGames.begin(), featuredGames.end(),
                           [&byId](GameItem &fg) {
                               if (fg.id <= 0)
                                   return true;
                               const auto it = byId.constFind(fg.id);
                               if (it == byId.cend())
                                   return true; // drop unknown ids — never show orphan title/poster
                               const GameItem &full = it.value();
                               fg.title = full.title;
                               fg.poster = full.poster;
                               fg.exePath = full.exePath;
                               fg.args = full.args;
                               fg.category = full.category;
                               fg.platform = full.platform;
                               return false;
                           }),
            featuredGames.end());
    }

    if (m_featuredGamesModel)
        m_featuredGamesModel->setGames(featuredGames);

    if (m_gamesModel) {
        m_gamesModel->setFeaturedGames(featuredGames);
        m_gamesModel->setGames(allGames);
    }

    if (m_featuredLabel != featuredLabel || m_featuredMode != featuredMode) {
        m_featuredLabel = featuredLabel;
        m_featuredMode = featuredMode;
        emit featuredChanged();
    } else {
        m_featuredLabel = featuredLabel;
        m_featuredMode = featuredMode;
        emit featuredChanged();
    }
}

void NetworkManager::recordGameLaunch(int gameId)
{
    if (m_serverUrl.isEmpty() || m_userId <= 0 || gameId <= 0)
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/games/record-launch"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QJsonObject json;
    json.insert(QStringLiteral("user_id"), m_userId);
    json.insert(QStringLiteral("game_id"), gameId);

    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, gameId]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            qWarning() << "[NET] record-launch failed game" << gameId << reply->errorString();
        else
            qDebug() << "[NET] record-launch OK game" << gameId;
    });
}

void NetworkManager::clearGamesSearch()
{
    if (m_gamesModel)
        m_gamesModel->setSearchQuery(QString());
}

void NetworkManager::sendSos(const QString &reasonCode, const QString &reasonLabel)
{
    if (m_serverUrl.isEmpty() || m_computerId <= 0) {
        qWarning() << "[SOS] cannot send: server or computer_id missing";
        emit sosSent(false);
        return;
    }
    if (reasonCode.isEmpty() || reasonLabel.isEmpty()) {
        qWarning() << "[SOS] cannot send: empty reason";
        emit sosSent(false);
        return;
    }

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/sos"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QJsonObject reason;
    reason.insert(QStringLiteral("code"), reasonCode);
    reason.insert(QStringLiteral("label"), reasonLabel);

    QJsonObject json;
    json.insert(QStringLiteral("computer_id"), m_computerId);
    if (m_lastBookingId > 0)
        json.insert(QStringLiteral("booking_id"), m_lastBookingId);
    json.insert(QStringLiteral("reason"), reason);
    json.insert(QStringLiteral("timestamp"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, reasonCode]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        if (!ok)
            qWarning() << "[SOS] POST failed:" << reasonCode << reply->errorString();
        else
            qDebug() << "[SOS] posted:" << reasonCode << "computer" << m_computerId;
        emit sosSent(ok);
    });
}

void NetworkManager::clearSessionUser()
{
    if (m_userId != 0) {
        m_userId = 0;
        emit userIdChanged();
    }
    if (m_lastBookingId != 0) {
        m_lastBookingId = 0;
        emit lastBookingIdChanged();
    }
    m_featuredLabel = QStringLiteral("Популярно в клубе");
    m_featuredMode = QStringLiteral("club");
    if (m_featuredGamesModel)
        m_featuredGamesModel->setGames({});
    if (m_gamesModel)
        m_gamesModel->setFeaturedGames({});
    emit featuredChanged();
}

int NetworkManager::resolveTerminalId(int terminalId) const
{
    if (terminalId > 0)
        return terminalId;
    if (m_computerId > 0)
        return m_computerId;
    if (m_rootQml) {
        const int fromRoot = m_rootQml->property("terminalId").toInt();
        if (fromRoot > 0)
            return fromRoot;
    }
    return 0;
}

void NetworkManager::applyOrderStatusFromJson(const QJsonObject &rootObj)
{
    if (!m_rootQml)
        return;

    const bool hasActiveOrder = rootObj.value(QStringLiteral("has_active_order")).toBool();
    QString statusCode = rootObj.value(QStringLiteral("order_status")).toString();
    if (statusCode.isEmpty())
        statusCode = rootObj.value(QStringLiteral("status")).toString();

    QString statusText = rootObj.value(QStringLiteral("status_text")).toString();
    if (statusText.isEmpty())
        statusText = rootObj.value(QStringLiteral("status_label")).toString();
    if (statusText.isEmpty() && hasActiveOrder) {
        if (statusCode == QLatin1String("cooking"))
            statusText = QStringLiteral("В РАБОТЕ");
        else if (statusCode == QLatin1String("pending"))
            statusText = QStringLiteral("ЗАКАЗ ПРИНЯТ");
        else
            statusText = QStringLiteral("В РАБОТЕ");
    }

    m_rootQml->setProperty("hasActiveOrder", hasActiveOrder);
    m_rootQml->setProperty("orderStatusCode", statusCode);
    if (!statusText.isEmpty())
        m_rootQml->setProperty("orderStatusText", statusText.toUpper());

    const int orderId = rootObj.value(QStringLiteral("order_id")).toInt(
        rootObj.value(QStringLiteral("order_id")).toVariant().toInt());
    if (orderId > 0)
        m_rootQml->setProperty("trackedOrderId", orderId);

    // Aggregate active order lines (pending/cooking) for Dashboard contents panel.
    // Prefer per-order items[] (multi-item checkout); fall back to legacy product_name.
    QVariantList items;
    double total = 0.0;
    QHash<QString, QPair<int, double>> aggregated;
    auto addLine = [&](const QString &name, int qty, double lineTotal) {
        if (name.isEmpty() || qty <= 0)
            return;
        auto &entry = aggregated[name];
        entry.first += qty;
        entry.second += lineTotal;
        total += lineTotal;
    };

    const QJsonArray ordersArr = rootObj.value(QStringLiteral("orders")).toArray();
    for (const QJsonValue &val : ordersArr) {
        const QJsonObject o = val.toObject();
        const QString st = o.value(QStringLiteral("status")).toString();
        if (st != QLatin1String("pending") && st != QLatin1String("cooking"))
            continue;

        const QJsonArray itemsArr = o.value(QStringLiteral("items")).toArray();
        if (!itemsArr.isEmpty()) {
            for (const QJsonValue &iv : itemsArr) {
                const QJsonObject item = iv.toObject();
                const QString name = item.value(QStringLiteral("name")).toString();
                const int qty = qMax(1, item.value(QStringLiteral("qty")).toInt(1));
                double lineTotal = item.value(QStringLiteral("line_total")).toDouble(
                    item.value(QStringLiteral("line_total")).toVariant().toDouble());
                if (lineTotal <= 0.0) {
                    const double unit = item.value(QStringLiteral("unit_price")).toDouble(
                        item.value(QStringLiteral("price")).toDouble(
                            item.value(QStringLiteral("price")).toVariant().toDouble()));
                    lineTotal = unit * qty;
                }
                addLine(name, qty, lineTotal);
            }
            continue;
        }

        const QString name = o.value(QStringLiteral("product_name")).toString();
        if (name.isEmpty())
            continue;
        const double price = o.value(QStringLiteral("price")).toDouble(
            o.value(QStringLiteral("price")).toVariant().toDouble());
        addLine(name, 1, price);
    }
    for (auto it = aggregated.constBegin(); it != aggregated.constEnd(); ++it) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), it.key());
        row.insert(QStringLiteral("qty"), it.value().first);
        row.insert(QStringLiteral("price"), it.value().second);
        items.append(row);
    }
    m_rootQml->setProperty("orderItems", items);
    m_rootQml->setProperty("orderItemsTotal", total);

    qDebug() << "[SHOP] order status → active=" << hasActiveOrder
             << "code=" << statusCode << "text=" << statusText
             << "order_id=" << orderId << "items=" << items.size();
}

void NetworkManager::fetchProducts() {
    if (m_serverUrl.isEmpty()) {
        qWarning() << "[SHOP] fetchProducts skipped: empty serverUrl";
        return;
    }

    const int terminalId = resolveTerminalId(0);
    QUrl url(m_serverUrl + "/api/shell/store/products");
    QUrlQuery query;
    if (terminalId > 0)
        query.addQueryItem(QStringLiteral("terminal_id"), QString::number(terminalId));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    qDebug() << "[SHOP] fetch products →" << url.toString();

    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[SHOP] fetch products error:" << reply->errorString();
            return;
        }

        const QByteArray responseData = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(responseData);
        QJsonArray productsArray;

        if (doc.isObject()) {
            const QJsonObject rootObj = doc.object();
            applyOrderStatusFromJson(rootObj);
            productsArray = rootObj.value(QStringLiteral("products")).toArray();
        } else if (doc.isArray()) {
            // Legacy: bare product array without order status
            productsArray = doc.array();
        }

        std::vector<StoreItem> productsVector;
        for (const QJsonValue &value : productsArray) {
            QJsonObject obj = value.toObject();
            StoreItem item;

            item.id = obj.value("id").toInt();
            item.name = obj.value("name").toString();
            QJsonValue priceVal = obj.value("price");
            if (priceVal.isString()) {
                item.price = priceVal.toString().toDouble();
            } else {
                item.price = priceVal.toDouble();
            }
            item.stock = obj.value("stock").toInt();
            item.image = obj.value("image").toString();
            item.category = obj.value("category").toString();

            productsVector.push_back(item);
        }

        qDebug() << "[SHOP] products loaded:" << productsVector.size();
        if (m_storeModel) {
            m_storeModel->setProducts(productsVector);
        }
    });
}

void NetworkManager::checkOrderStatus(int terminalId, int orderId)
{
    if (m_serverUrl.isEmpty()) {
        qWarning() << "[SHOP] checkOrderStatus skipped: empty serverUrl";
        return;
    }

    const int tid = resolveTerminalId(terminalId);
    if (tid <= 0) {
        qWarning() << "[SHOP] checkOrderStatus skipped: no terminalId";
        return;
    }

    int oid = orderId;
    if (oid <= 0 && m_rootQml)
        oid = m_rootQml->property("trackedOrderId").toInt();

    QUrl url(m_serverUrl + "/api/shell/store/order-status");
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("terminal_id"), QString::number(tid));
    if (oid > 0)
        query.addQueryItem(QStringLiteral("order_id"), QString::number(oid));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    qDebug() << "[SHOP] poll order status →" << url.toString();

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[SHOP] order status error:" << reply->errorString();
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject())
            return;
        applyOrderStatusFromJson(doc.object());
    });
}

void NetworkManager::login(const QString &phone, const QString &pin, int terminalId)
{
    if (m_serverUrl.isEmpty()) {
        emit loginFailed(tr("Сервер не настроен"));
        emit loginRequestFinished();
        return;
    }

    const QString cleanPhone = cleanDigits(phone);
    const QString cleanPin = cleanDigits(pin);
    if (cleanPhone.isEmpty() || cleanPin.isEmpty()) {
        emit loginFailed(tr("Заполните телефон и PIN-код"));
        emit loginRequestFinished();
        return;
    }

    QUrl url(m_serverUrl + "/api/shell/login");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    QJsonObject json;
    json["phone"] = cleanPhone;
    json["pin"] = cleanPin;
    json["terminal_id"] = terminalId;

    qDebug() << "[NET] Login →" << url.toString();

    QNetworkReply *reply = m_networkManager->post(request, QJsonDocument(json).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, cleanPhone]() {
        reply->deleteLater();

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray responseData = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[NET] Ошибка авторизации:" << reply->errorString();
            emit loginFailed(tr("Сервер не отвечает (%1)").arg(httpStatus > 0 ? httpStatus : reply->error()));
            emit loginRequestFinished();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(responseData);
        const QJsonObject response = doc.object();

        if (httpStatus == 200 && response.value("status").toString() == "success") {
            const QJsonObject user = response.value("user").toObject();
            const int bookingId = response.value("booking_id").toInt(0);
            if (m_lastBookingId != bookingId) {
                m_lastBookingId = bookingId;
                emit lastBookingIdChanged();
            }
            const int uid = user.value(QStringLiteral("id")).toInt(0);
            if (m_userId != uid) {
                m_userId = uid;
                emit userIdChanged();
            }
            const double balance = userBalanceFromJson(user);
            qDebug() << "[NET] Login OK user=" << user.value("id").toInt(0)
                     << "balance=" << balance
                     << "rawKeys=" << user.keys();
            emit loginSucceeded(
                user.value("name").toString("GUEST"),
                balance,
                user.value("time_remaining").toString("00:00:00"),
                cleanPhone);
        } else {
            emit loginFailed(response.value("message").toString(
                tr("Неверный логин или PIN-код")));
        }

        emit loginRequestFinished();
    });
}

void NetworkManager::fetchOverlays(int terminalId)
{
    if (m_serverUrl.isEmpty()) {
        return;
    }

    const int targetId = terminalId > 0 ? terminalId : (m_computerId > 0 ? m_computerId : 1);
    QUrl url(m_serverUrl + "/api/shell/overlays?terminal_id=" + QString::number(targetId)
             + "&t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));

    qDebug() << "[NET] Запрос оверлеев:" << url.toString();

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    QNetworkReply *reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, targetId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[NET] Ошибка загрузки оверлеев:" << reply->errorString();
            return;
        }

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus != 200) {
            qWarning() << "[NET] Бэкенд вернул HTTP" << httpStatus << "для оверлеев";
            return;
        }

        const QByteArray raw = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        const QJsonObject rootObject = doc.object();
        const QJsonObject payload = rootObject.contains("data")
            ? rootObject.value("data").toObject()
            : rootObject;

        qDebug() << "[NET] Оверлеи получены для terminal_id=" << targetId
                 << "| ключи:" << payload.keys()
                 << "| байт:" << raw.size();

        emit overlaysReady(payload.toVariantMap());
    });
}

void NetworkManager::freeGameAccount(int terminalId, int gameId)
{
    if (m_serverUrl.isEmpty() || terminalId <= 0 || gameId <= 0) {
        emit freeAccountFinished(false);
        return;
    }

    QUrl url(m_serverUrl + "/api/shell/games/free-account");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    QJsonObject json;
    json["terminal_id"] = terminalId;
    json["game_id"] = gameId;

    QNetworkReply *reply = m_networkManager->post(request, QJsonDocument(json).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const bool success = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;

        if (!success) {
            qWarning() << "[NET] Не удалось освободить игровой аккаунт:" << reply->errorString();
        }

        emit freeAccountFinished(success);
    });
}