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

NetworkManager::NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent)
    : QObject(parent)
    , m_isPcRegistered(false)
    , m_computerId(0)
    , m_lastBookingId(0)
    , m_gamesModel(gamesModel)
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
                if (m_lastBookingId != 0) {
                    m_lastBookingId = 0;
                    emit lastBookingIdChanged();
                }
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
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray responseData = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(responseData);

            // ИСПРАВЛЕНО: Твой Laravel возвращает прямой массив объектов, а не словарь!
            QJsonArray gamesArray = doc.array();

            std::vector<GameItem> gamesVector;
            for (const QJsonValue &value : gamesArray) {
                QJsonObject obj = value.toObject();
                GameItem item;

                item.id = obj.value("id").toInt();
                item.title = obj.value("title").toString();
                item.exePath = obj.value("exe_path").toString();
                item.args = obj.value("args").toString();
                item.poster = obj.value("poster").toString();
                item.category = obj.value("category").toString();
                item.platform = obj.value("platform").toString();

                gamesVector.push_back(item);
            }

            if (m_gamesModel) {
                m_gamesModel->setGames(gamesVector);
            }
        } else {
            qWarning() << "[NET] Ошибка получения списка игр:" << reply->errorString();
        }
    });
}

void NetworkManager::fetchProducts() {
    if (m_serverUrl.isEmpty()) return;

    // Стучимся на прямой роут /api/shell/products, который прописан в web.php
    QUrl url(m_serverUrl + "/api/shell/products");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray responseData = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(responseData);

            // ИСПРАВЛЕНО: Маркет тоже отдает чистый массив объектов без ключей
            QJsonArray productsArray = doc.array();

            std::vector<StoreItem> productsVector;
            for (const QJsonValue &value : productsArray) {
                QJsonObject obj = value.toObject();
                StoreItem item;

                item.id = obj.value("id").toInt();
                item.name = obj.value("name").toString();
                QJsonValue priceVal = obj.value("price");
                if (priceVal.isString()) {
                    item.price = priceVal.toString().toDouble(); // Конвертируем из строки "150.00"
                } else {
                    item.price = priceVal.toDouble(); // Если пришло чистое число
                }
                item.stock = obj.value("stock").toInt();
                item.image = obj.value("image").toString();
                item.category = obj.value("category").toString();

                productsVector.push_back(item);
            }

            if (m_storeModel) {
                m_storeModel->setProducts(productsVector);
            }
        } else {
            qWarning() << "[NET] Ошибка получения товаров маркета:" << reply->errorString();
        }
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
            emit loginSucceeded(
                user.value("name").toString("GUEST"),
                user.value("balance").toDouble(0.0),
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