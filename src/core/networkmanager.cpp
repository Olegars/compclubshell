#include "networkmanager.h"
#include "../models/gamemodel.h"
#include "../models/storemodel.h"
#include <QCoreApplication> // <-- СЮДА ДОБАВЛЯЕМ СТРОКУ!
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

NetworkManager::NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent)
    : QObject(parent)
    , m_isPcRegistered(false)
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
            int computerId = responseObj.value("computer_id").toInt();
            m_pcNameString = "PC-" + QString::number(computerId);
            m_isPcRegistered = true;

            qDebug() << "[REACTOR-SHELL] Терминал найден в системе. Присвоен ID:" << computerId;

            emit pcRegistrationChanged();
            emit authRequired();

            if (m_rootQml) {
                qDebug() << "[NET] Успешная авторизация железа. Принудительно вызываем fetchOverlays()...";
                QMetaObject::invokeMethod(m_rootQml, "fetchOverlays");
            }
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

void NetworkManager::registerStation(const QString &zoneType, const QString &pcName) {
    if (m_serverUrl.isEmpty()) {
        qDebug() << "[CRITICAL-C++ SETUP] Ошибка: m_serverUrl пустой! Запрос отменён.";
        return;
    }

    QUrl url(m_serverUrl + "/api/shell/register-terminal");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    // Собираем пакет
    QJsonObject json;
    json["hwid"] = m_hwid;
    json["zone_type"] = zoneType; // Обрати внимание: в QML у тебя "Duo" с большой буквы, Laravel может ждать "duo"
    json["name"] = pcName;

    QJsonDocument doc(json);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    qDebug() << "[DEBUG-C++ SETUP] ---> ОТПРАВКА ПОСТ-ЗАПРОСА";
    qDebug() << "[DEBUG-C++ SETUP] URL:" << url.toString();
    qDebug() << "[DEBUG-C++ SETUP] JSON DATA:" << jsonData;

    QNetworkReply *reply = m_networkManager->post(request, jsonData);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        qDebug() << "[DEBUG-C++ SETUP] <--- ОТВЕТ СЕТИ ПОЛУЧЕН";

        // Проверяем HTTP статус-код (200, 422, 500 и т.д.)
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

void NetworkManager::fetchGames() { qDebug() << "[NET] Синхронизация списка игр..."; }
void NetworkManager::fetchProducts() { qDebug() << "[NET] Синхронизация товаров маркета..."; }