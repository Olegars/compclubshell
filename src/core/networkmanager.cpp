#include "networkmanager.h"
#include "../models/gamemodel.h"
#include "../models/storemodel.h"
#include <QFile>

NetworkManager::NetworkManager(GameModel* gamesModel, StoreModel* storeModel, QObject *parent)
    : QObject(parent)
    , m_isPcRegistered(false)
    , m_gamesModel(gamesModel)
    , m_storeModel(storeModel)
{
    m_networkManager = new QNetworkAccessManager(this);

    // Перебираем пути, чтобы точно зацепить конфиг бездиска в любой среде сборки
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

    // Читаем твои нативные ключи из секции [Network]
    QSettings settings(m_configFilePath, QSettings::IniFormat);
    QString apiIp = settings.value("Network/api_ip", "192.168.222.2").toString().trimmed();
    QString apiPort = settings.value("Network/api_port", "22222").toString().trimmed();

    // Собираем итоговый базовый URL
    m_serverUrl = "http://" + apiIp + ":" + apiPort;

    qDebug() << "[REACTOR-SHELL] Обнаружен config.ini по пути:" << m_configFilePath;
    qDebug() << "[REACTOR-SHELL] Инициализация сети. Целевой хост:" << m_serverUrl;
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

// 1. ЭТОТ МЕТОД КУСАЕТ БЭКЕНД ПРИ СТАРТЕ ЛАУНЧЕРА
void NetworkManager::checkTerminalStatus() {
    if (m_hwid.isEmpty() || m_hwid == "UNKNOWN_HWID_FALLBACK") {
        qWarning() << "[REACTOR-SHELL] Ошибка: HWID пустой, невозможно проверить статус в БД.";
        emit setupRequired();
        return;
    }

    // Стучимся строго на /api/shell/check, как прописано в Laravel!
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
        } else {
            qDebug() << "[REACTOR-SHELL] Оборудование не зарегистрировано. Переключение на экран выбора зоны.";
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

// 2. ЭТОТ МЕТОД СРАБАТЫВАЕТ ПРИ НАЖАТИИ НА КНОПКУ ЗОНЫ
void NetworkManager::registerStation(const QString &zoneType) {
    if (m_hwid.isEmpty()) return;

    QUrl url(m_serverUrl + "/api/shell/register-terminal");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject json;
    json["hwid"] = m_hwid;

    // ИСПРАВЛЕНО: Убрали .toLower(), отправляем оригинальную строку ("Trio", "Bootcamp" и т.д.)
    json["type"] = zoneType;

    QJsonDocument doc(json);
    QByteArray data = doc.toJson();

    qDebug() << "[REACTOR-SHELL] Отправка данных регистрации (тип:" << zoneType << "):" << url.toString();

    QNetworkReply* reply = m_networkManager->post(request, data);

    connect(reply, &QNetworkReply::finished, this, [this, reply, zoneType]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[REACTOR-SHELL] Критическая ошибка при отправке запроса:" << reply->errorString();
            // Выводим тело ответа сервера, чтобы точно видеть, на чём споткнулась валидация Laravel
            qWarning() << "[REACTOR-SHELL] Детали ответа сервера:" << reply->readAll();
            return;
        }

        QByteArray responseData = reply->readAll();
        QJsonDocument responseDoc = QJsonDocument::fromJson(responseData);
        QJsonObject responseObj = responseDoc.object();

        if (responseObj.value("status").toString() == "success") {
            int terminalId = responseObj.value("terminal_id").toInt();

            m_pcNameString = "PC-" + QString::number(terminalId);
            m_isPcRegistered = true;

            qDebug() << "[REACTOR-SHELL] Успешная привязка к свободному слоту карты! ID ПК:" << terminalId;

            emit pcRegistrationChanged();
            emit authRequired();
        } else {
            qWarning() << "[REACTOR-SHELL] Регистрация отклонена бэкендом:" << responseObj.value("message").toString();
        }
    });
}

QStringList NetworkManager::getAvailableZones() {
    return QStringList() << "Single" << "Duo" << "Trio" << "Quatro" << "Bootcamp";
}

void NetworkManager::fetchGames() {
    qDebug() << "[NET] Синхронизация списка игр...";
}

void NetworkManager::fetchProducts() {
    qDebug() << "[NET] Синхронизация товаров маркета...";
}

QString NetworkManager::getLocalPath(const QString &remoteUrl, const QString &blockId) {
    Q_UNUSED(remoteUrl);
    Q_UNUSED(blockId);
    return "";
}