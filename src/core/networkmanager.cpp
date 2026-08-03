#include "networkmanager.h"
#include "hwidprovider.h"
#include "thermalmonitor.h"
#include "fanrelaycontroller.h"
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
#include <QTimer>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QNetworkInterface>
#include <QEventLoop>
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
    m_serverUrl = buildServerUrl(apiIp, apiPort);

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

QString NetworkManager::buildServerUrl(const QString &rawHost, const QString &rawPort)
{
    QString host = rawHost.trimmed();
    QString port = rawPort.trimmed();

    if (host.isEmpty())
        return QString();

    // Схему разрешаем писать прямо в api_ip: "https://0451.space".
    QString scheme;
    const int schemeSep = host.indexOf(QStringLiteral("://"));
    if (schemeSep > 0) {
        scheme = host.left(schemeSep).toLower();
        host = host.mid(schemeSep + 3);
    }

    // К serverUrl везде дописываются пути вида "/api/...", поэтому хвост
    // ("0451.space/" или "0451.space/api") нужно отбросить.
    const int slash = host.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        host = host.left(slash);

    // Порт, записанный в адресе, приоритетнее api_port. IPv6 в конфиге не
    // используется, поэтому достаточно последнего двоеточия.
    const int colon = host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool numeric = false;
        const QString tail = host.mid(colon + 1);
        tail.toInt(&numeric);
        if (numeric) {
            port = tail;
            host = host.left(colon);
        }
    }

    if (host.isEmpty())
        return QString();

    if (scheme.isEmpty())
        scheme = (port == QLatin1String("443")) ? QStringLiteral("https") : QStringLiteral("http");

    // Стандартный порт в адресе не нужен: он ломает сравнение origin
    // (например, в CORS и в secure-origin флаге WebView2).
    const bool defaultPort = port.isEmpty()
            || (scheme == QLatin1String("https") && port == QLatin1String("443"))
            || (scheme == QLatin1String("http") && port == QLatin1String("80"));

    QString url = scheme + QStringLiteral("://") + host;
    if (!defaultPort)
        url += QLatin1Char(':') + port;
    return url;
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

            m_zoneName = responseObj.value(QStringLiteral("zone_name")).toString().trimmed();
            m_zoneSlug = responseObj.value(QStringLiteral("zone_slug")).toString().trimmed();
            if (m_zoneSlug.isEmpty())
                m_zoneSlug = responseObj.value(QStringLiteral("type")).toString().trimmed();
            if (m_zoneSlug.isEmpty())
                m_zoneSlug = QStringLiteral("singl");
            m_zoneColor = responseObj.value(QStringLiteral("zone_color")).toString().trimmed();
            emit zoneInfoChanged();

            m_isPcRegistered = true;

            qDebug() << "[REACTOR-SHELL] Терминал авторизован под именем:" << m_pcNameString
                    << "| ID записи в БД:" << m_computerId
                    << "| зона:" << (m_zoneName.isEmpty() ? m_zoneSlug : m_zoneName);

            emit pcRegistrationChanged();
            emit authRequired();
            startPowerHeartbeat();
            // Post-boot cool-down: climate runs without session to blow room if hot.
            m_postBootCooldown = true;
            startClimateControl();

            const int overlayTerminalId = m_computerId > 0 ? m_computerId : 1;
            fetchOverlays(overlayTerminalId);
            // fetchQuickApps — после логина (Main.onLoginSucceeded). exists() по exe
            // на UI-потоке рядом с экраном телефона давал заметные подвисания.
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

                const QJsonObject fanObj = responseJson.value(QStringLiteral("fan")).toObject();
                applyFanStateFromJson(fanObj);
                const QJsonObject facts = fanObj.value(QStringLiteral("facts")).toObject();
                const int sessionsLeft = facts.value(QStringLiteral("sessions_in_space")).toInt(
                    facts.value(QStringLiteral("session")).toBool(false) ? 1 : 0);
                // Last session in room → always OFF (no orphan ON after empty room).
                if (sessionsLeft <= 0) {
                    m_postBootCooldown = false;
                    ensureFanOffBeforeExit();
                }

                const QString powerAction = responseJson.value(QStringLiteral("power_action")).toString();
                if (powerAction == QLatin1String("reboot")
                    || powerAction == QLatin1String("shutdown")) {
                    if (powerAction == QLatin1String("shutdown"))
                        m_idleShutdownRequested = true;
                    m_sawActiveSession = false;
                    qWarning() << "[POWER] logout →" << powerAction;
                    emit powerActionRequested(powerAction);
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

    QString fileName = remotePath.split('/').last().split('?').first();
    if (fileName.isEmpty()) fileName = "overlay_video.mp4";
    QString localFilePath = m_cachePath + fileName;

    if (QFile::exists(localFilePath) && QFileInfo(localFilePath).size() > 0) {
        return QUrl::fromLocalFile(localFilePath).toString();
    }

    if (m_activeDownloads.contains(target)) {
        return "";
    }

    m_activeDownloads.append(target);

    // Overlay URLs in DB often keep an old absolute host (e.g. LAN IP).
    // Always download /storage/... from the configured shell server.
    QString fullUrl = remotePath;
    const QUrl remoteUrl(remotePath);
    if (remoteUrl.isValid() && !remoteUrl.scheme().isEmpty()) {
        const QString path = remoteUrl.path();
        if (path.contains(QStringLiteral("/storage/"), Qt::CaseInsensitive)
            && !m_serverUrl.isEmpty()) {
            fullUrl = m_serverUrl + path;
            if (remoteUrl.hasQuery())
                fullUrl += QLatin1Char('?') + remoteUrl.query();
        }
    } else if (!remotePath.startsWith("http")) {
        QString cleanRemote = remotePath;
        if (cleanRemote.startsWith("/")) cleanRemote.remove(0, 1);
        fullUrl = m_serverUrl + "/" + cleanRemote;
    }

    qDebug() << "[CACHE-OPTIMIZED] Запуск одиночного скачивания файла для зоны:" << target
             << "URL:" << fullUrl
             << "(raw:" << remotePath << ")";

    QNetworkRequest request((QUrl(fullUrl)));
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 ReactorShell/1.0");

    const QString tmpPath = localFilePath + QStringLiteral(".part");
    QFile::remove(tmpPath);
    auto *outFile = new QFile(tmpPath);
    if (!outFile->open(QIODevice::WriteOnly)) {
        qWarning() << "[CACHE] cannot open temp file:" << tmpPath;
        delete outFile;
        m_activeDownloads.removeAll(target);
        return "";
    }

    QNetworkReply *reply = m_networkManager->get(request);

    // Пишем по мере прихода — не копим 100–160 МБ в RAM и не блочим UI на finished.
    connect(reply, &QNetworkReply::readyRead, this, [reply, outFile]() {
        if (outFile && outFile->isOpen())
            outFile->write(reply->readAll());
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, outFile, localFilePath, tmpPath, remotePath, target]() {
        reply->deleteLater();
        this->m_activeDownloads.removeAll(target);

        if (outFile->isOpen()) {
            // Дочищаем хвост буфера
            if (reply->bytesAvailable() > 0)
                outFile->write(reply->readAll());
            outFile->close();
        }
        outFile->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QFile::remove(localFilePath);
            if (!QFile::rename(tmpPath, localFilePath)) {
                QFile::remove(localFilePath);
                QFile::copy(tmpPath, localFilePath);
                QFile::remove(tmpPath);
            }
            qDebug() << "[CACHE] Фоновое скачивание успешно завершено для зоны:" << target;
            emit fileDownloaded(remotePath, QUrl::fromLocalFile(localFilePath).toString(), target);
        } else {
            QFile::remove(tmpPath);
            qWarning() << "[CACHE] Ошибка скачивания оверлея для" << target << ":" << reply->errorString();
        }
    });

    return "";
}

bool NetworkManager::isLocalMediaLight(const QString &qmlOrLocalPath, qint64 maxBytes) const
{
    if (qmlOrLocalPath.isEmpty() || maxBytes <= 0)
        return false;

    QString path = qmlOrLocalPath;
    const QUrl url(qmlOrLocalPath);
    if (url.isValid() && url.isLocalFile())
        path = url.toLocalFile();
    else if (path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        path = QUrl(path).toLocalFile();

    if (path.isEmpty())
        return false;

    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return false;
    const qint64 sz = fi.size();
    return sz > 0 && sz <= maxBytes;
}

int NetworkManager::getLatency(const QString &host) {
    Q_UNUSED(host);
    return 24 + (rand() % 4);
}

QStringList NetworkManager::getAvailableZones() {
    return QStringList()
            << QStringLiteral("singl")
            << QStringLiteral("duo")
            << QStringLiteral("trio")
            << QStringLiteral("kvatro")
            << QStringLiteral("bootcamp")
            << QStringLiteral("tv");
}

void NetworkManager::fetchQuickApps()
{
    if (m_serverUrl.isEmpty())
        return;

    QNetworkRequest request(QUrl(m_serverUrl + QStringLiteral("/api/shell/quick-apps")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[QUICK] API error:" << reply->errorString();
            if (!m_quickApps.isEmpty()) {
                m_quickApps.clear();
                emit quickAppsChanged();
            }
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        const QJsonObject root = doc.object();
        if (root.value(QStringLiteral("status")).toString() != QLatin1String("success")
                || !root.value(QStringLiteral("apps")).isArray()) {
            qWarning() << "[QUICK] invalid API payload";
            return;
        }

        QVariantList apps;
        const QJsonArray rows = root.value(QStringLiteral("apps")).toArray();
        apps.reserve(rows.size());
        for (const QJsonValue &value : rows) {
            if (!value.isObject())
                continue;
            const QJsonObject row = value.toObject();
            const QString title = row.value(QStringLiteral("title")).toString().trimmed();
            const QString path = row.value(QStringLiteral("exe_path")).toString().trimmed();
            if (title.isEmpty() || path.isEmpty())
                continue;

            QVariantMap app;
            app.insert(QStringLiteral("id"), row.value(QStringLiteral("id")).toInt());
            app.insert(QStringLiteral("title"), title);
            app.insert(QStringLiteral("path"), path);
            app.insert(QStringLiteral("args"), row.value(QStringLiteral("args")).toString());
            app.insert(QStringLiteral("available"), QFileInfo::exists(path));
            apps.append(app);
        }

        if (m_quickApps == apps)
            return;
        m_quickApps = apps;
        qDebug() << "[QUICK] loaded from admin:" << m_quickApps.size();
        emit quickAppsChanged();
    });
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
    m_lastKnownBalance = -1.0;
    m_balanceRefreshInFlight = false;
    stopClimateControl();
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
            m_lastKnownBalance = balance;
            qDebug() << "[NET] Login OK user=" << user.value("id").toInt(0)
                     << "balance=" << balance
                     << "rawKeys=" << user.keys();
            m_postBootCooldown = false;
            m_sawActiveSession = true;
            const QJsonObject fanObj = response.value(QStringLiteral("fan")).toObject();
            if (!fanObj.isEmpty())
                applyFanStateFromJson(fanObj);
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

void NetworkManager::refreshBalance()
{
    if (m_serverUrl.isEmpty() || m_balanceRefreshInFlight)
        return;

    // No session → do not spam the API.
    if (m_userId <= 0 && m_lastBookingId <= 0) {
        if (m_rootQml) {
            const QString sessionUser = m_rootQml->property("sessionUser").toString();
            if (sessionUser.isEmpty()
                || sessionUser == QLatin1String("GUEST")
                || sessionUser == QLatin1String("PAUSE")) {
                return;
            }
        } else {
            return;
        }
    }

    const int tid = resolveTerminalId(0);
    if (tid <= 0 && m_userId <= 0 && m_lastBookingId <= 0)
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/balance"));
    QUrlQuery query;
    if (tid > 0)
        query.addQueryItem(QStringLiteral("terminal_id"), QString::number(tid));
    if (m_lastBookingId > 0)
        query.addQueryItem(QStringLiteral("booking_id"), QString::number(m_lastBookingId));
    if (m_userId > 0)
        query.addQueryItem(QStringLiteral("user_id"), QString::number(m_userId));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    m_balanceRefreshInFlight = true;
    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_balanceRefreshInFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[NET] balance refresh error:" << reply->errorString();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject())
            return;

        const QJsonObject response = doc.object();
        if (response.value(QStringLiteral("status")).toString() != QLatin1String("success"))
            return;

        double balance = -1.0;
        if (response.contains(QStringLiteral("balance")))
            balance = jsonToDouble(response.value(QStringLiteral("balance")), -1.0);
        else if (response.contains(QStringLiteral("deposit_balance")))
            balance = jsonToDouble(response.value(QStringLiteral("deposit_balance")), -1.0);
        else if (response.value(QStringLiteral("user")).isObject())
            balance = userBalanceFromJson(response.value(QStringLiteral("user")).toObject());

        if (balance < 0.0)
            return;

        // Avoid QML churn when unchanged (kopeck tolerance).
        if (m_lastKnownBalance >= 0.0 && qAbs(balance - m_lastKnownBalance) < 0.005)
            return;

        m_lastKnownBalance = balance;
        qDebug() << "[NET] balance updated =" << balance;
        emit balanceUpdated(balance);

        if (m_rootQml) {
            const double current = m_rootQml->property("sessionBalance").toDouble();
            if (qAbs(current - balance) >= 0.005)
                m_rootQml->setProperty("sessionBalance", balance);
        }
    });
}

void NetworkManager::syncTopUpPayment(const QString &paymentId)
{
    if (m_serverUrl.isEmpty() || paymentId.isEmpty())
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/billing/yookassa/sync/") + paymentId);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");

    qDebug() << "[NET] syncTopUpPayment" << paymentId;
    QNetworkReply *reply = m_networkManager->post(request, QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, paymentId]() {
        reply->deleteLater();

        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[NET] syncTopUpPayment error:" << reply->errorString() << body;
            refreshBalance();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        const QJsonObject response = doc.isObject() ? doc.object() : QJsonObject();
        const bool paid = response.value(QStringLiteral("paid")).toBool();
        const QString status = response.value(QStringLiteral("payment_status")).toString();
        qDebug() << "[NET] syncTopUpPayment OK" << paymentId
                 << "status=" << status << "paid=" << paid;

        // Always refresh: webhook may have credited already, or sync just did.
        refreshBalance();
    });
}

void NetworkManager::createTopUp(double amount)
{
    if (m_serverUrl.isEmpty()) {
        emit topUpFailed(tr("Сервер не настроен"));
        return;
    }

    const int tid = resolveTerminalId(0);
    if (tid <= 0) {
        emit topUpFailed(tr("Терминал не привязан"));
        return;
    }

    if (amount < 100.0) {
        emit topUpFailed(tr("Минимальная сумма пополнения — 100 ₽"));
        return;
    }

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/billing/topup"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QJsonObject json;
    json.insert(QStringLiteral("terminal_id"), tid);
    json.insert(QStringLiteral("amount"), amount);
    if (m_lastBookingId > 0)
        json.insert(QStringLiteral("booking_id"), m_lastBookingId);
    if (m_userId > 0)
        json.insert(QStringLiteral("user_id"), m_userId);

    qDebug() << "[NET] createTopUp amount=" << amount << "terminal=" << tid;
    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        const QJsonObject response = doc.isObject() ? doc.object() : QJsonObject();

        if (reply->error() != QNetworkReply::NoError) {
            const QString msg = response.value(QStringLiteral("message")).toString(reply->errorString());
            qWarning() << "[NET] createTopUp error:" << msg << body;
            emit topUpFailed(msg.isEmpty() ? tr("Не удалось создать платёж") : msg);
            return;
        }

        if (response.value(QStringLiteral("status")).toString() != QLatin1String("success")) {
            const QString msg = response.value(QStringLiteral("message")).toString(tr("Не удалось создать платёж"));
            emit topUpFailed(msg);
            return;
        }

        const QString confirmationToken =
            response.value(QStringLiteral("confirmation_token")).toString();
        const QString widgetUrl = response.value(QStringLiteral("widget_url")).toString();
        const QString confirmationUrl = response.value(QStringLiteral("confirmation_url")).toString();
        QString paymentUrl;

        if (!widgetUrl.isEmpty()) {
            paymentUrl = widgetUrl;
        } else if (!confirmationUrl.isEmpty()) {
            paymentUrl = confirmationUrl;
        } else if (!confirmationToken.isEmpty()) {
            // Запасной путь, если бэкенд отдал только токен: checkout-ui верхним
            // уровнем. Страница-обёртка предпочтительнее — там результат оплаты
            // приходит колбэком виджета и баланс синхронизируется.
            QUrl checkoutUrl(QStringLiteral("https://yoomoney.ru/checkout/checkout-ui"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("token"), confirmationToken);
            checkoutUrl.setQuery(query);
            paymentUrl = checkoutUrl.toString(QUrl::FullyEncoded);
        }
        const QString paymentId = response.value(QStringLiteral("payment_id")).toString();
        const double paidAmount = jsonToDouble(response.value(QStringLiteral("amount")), 0.0);

        if (paymentUrl.isEmpty()) {
            emit topUpFailed(tr("ЮKassa не вернула ссылку на виджет оплаты"));
            return;
        }

        qDebug() << "[NET] createTopUp OK payment=" << paymentId << "url=" << paymentUrl;
        emit topUpReady(paymentUrl, paymentId, paidAmount);
    });
}

void NetworkManager::fetchOverlays(int terminalId)
{
    if (m_serverUrl.isEmpty()) {
        return;
    }

    const int targetId = terminalId > 0 ? terminalId : (m_computerId > 0 ? m_computerId : 1);

    // Single-flight: authRequired + terminalId + Loader.Ready иначе бьют 3–4 раза подряд
    // и каждый overlaysReady пересобирает 6 OverlayBlock на UI-потоке.
    if (m_overlaysFetchInFlight) {
        m_overlaysQueuedTerminalId = targetId;
        return;
    }
    m_overlaysFetchInFlight = true;
    m_overlaysQueuedTerminalId = -1;

    QUrl url(m_serverUrl + "/api/shell/overlays?terminal_id=" + QString::number(targetId)
             + "&t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));

    qDebug() << "[NET] Запрос оверлеев:" << url.toString();

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "ReactorShell/1.0");

    QNetworkReply *reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, targetId]() {
        reply->deleteLater();
        m_overlaysFetchInFlight = false;

        const int queued = m_overlaysQueuedTerminalId;
        m_overlaysQueuedTerminalId = -1;

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[NET] Ошибка загрузки оверлеев:" << reply->errorString();
            if (queued > 0 && queued != targetId)
                fetchOverlays(queued);
            return;
        }

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus != 200) {
            qWarning() << "[NET] Бэкенд вернул HTTP" << httpStatus << "для оверлеев";
            if (queued > 0 && queued != targetId)
                fetchOverlays(queued);
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

        // Если за время запроса пришёл другой terminal_id — один догон.
        if (queued > 0 && queued != targetId)
            fetchOverlays(queued);
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

void NetworkManager::startPowerHeartbeat()
{
    if (!m_powerHeartbeatTimer) {
        m_powerHeartbeatTimer = new QTimer(this);
        m_powerHeartbeatTimer->setInterval(30000);
        connect(m_powerHeartbeatTimer, &QTimer::timeout, this, &NetworkManager::sendPowerHeartbeat);
    }
    if (!m_powerHeartbeatTimer->isActive()) {
        m_powerHeartbeatTimer->start();
        qWarning() << "[POWER] heartbeat started, terminal" << m_computerId;
    }
    sendPowerHeartbeat();
}

void NetworkManager::stopPowerHeartbeat()
{
    if (m_powerHeartbeatTimer)
        m_powerHeartbeatTimer->stop();
}

QString NetworkManager::primaryMacAddress() const
{
    if (!m_cachedMac.isEmpty())
        return m_cachedMac;

    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        const auto flags = iface.flags();
        if (flags & QNetworkInterface::IsLoopBack)
            continue;
        if (!(flags & QNetworkInterface::IsUp))
            continue;
        if (!(flags & QNetworkInterface::IsRunning))
            continue;
        const QString mac = iface.hardwareAddress().trimmed().toUpper();
        if (mac.isEmpty() || mac == QLatin1String("00:00:00:00:00:00"))
            continue;
        return mac;
    }
    return {};
}

bool NetworkManager::isLocalSessionActive() const
{
    if (m_userId > 0)
        return true;
    if (!m_rootQml)
        return false;
    const QString sessionUser = m_rootQml->property("sessionUser").toString();
    return !sessionUser.isEmpty()
            && sessionUser != QLatin1String("GUEST")
            && sessionUser != QLatin1String("");
}

void NetworkManager::handlePowerPolicy(const QString &desired, const QString &action, bool sessionActive)
{
    if (sessionActive) {
        m_sawActiveSession = true;
        m_idleShutdownRequested = false;
        return;
    }

    // Сервер закрыл бронь, а шелл ещё показывает игрока → сброс UI + reboot/shutdown.
    if (isLocalSessionActive() && m_sawActiveSession) {
        m_sawActiveSession = false;
        clearSessionUser();
        emit sessionForceEnded();
        if (action == QLatin1String("reboot") || action == QLatin1String("shutdown")) {
            qWarning() << "[POWER] session ended by server →" << action;
            emit powerActionRequested(action);
        }
        return;
    }

    // Гостевой экран и ПК больше не нужен → выключение (заглушка).
    if (!isLocalSessionActive() && desired == QLatin1String("off")) {
        if (!m_idleShutdownRequested) {
            m_idleShutdownRequested = true;
            qWarning() << "[POWER] idle + desired=off → shutdown";
            emit powerActionRequested(QStringLiteral("shutdown"));
        }
        return;
    }

    if (desired == QLatin1String("on"))
        m_idleShutdownRequested = false;
}

void NetworkManager::sendPowerHeartbeat()
{
    if (m_serverUrl.isEmpty() || m_powerHeartbeatInFlight)
        return;

    const int termId = resolveTerminalId(0);
    if (termId <= 0 && m_hwid.isEmpty())
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/power/heartbeat"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QJsonObject json;
    if (termId > 0)
        json.insert(QStringLiteral("terminal_id"), termId);
    if (!m_hwid.isEmpty())
        json.insert(QStringLiteral("hwid"), m_hwid);

    const QString mac = primaryMacAddress();
    if (!mac.isEmpty()) {
        m_cachedMac = mac;
        json.insert(QStringLiteral("mac_address"), mac);
    }

    m_powerHeartbeatInFlight = true;
    QNetworkReply *reply = m_networkManager->post(request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_powerHeartbeatInFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[POWER] heartbeat failed:" << reply->errorString();
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        if (root.value(QStringLiteral("status")).toString() != QLatin1String("success"))
            return;

        handlePowerPolicy(
            root.value(QStringLiteral("power_desired")).toString(),
            root.value(QStringLiteral("power_action")).toString(),
            root.value(QStringLiteral("session_active")).toBool());
    });
}

void NetworkManager::notifyPowerOffline()
{
    ensureFanOffBeforeExit();
    stopPowerHeartbeat();

    if (m_serverUrl.isEmpty())
        return;

    const int termId = resolveTerminalId(0);
    if (termId <= 0 && m_hwid.isEmpty())
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/power/offline"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    // Не зависать надолго при выходе
    request.setTransferTimeout(2000);

    QJsonObject json;
    if (termId > 0)
        json.insert(QStringLiteral("terminal_id"), termId);
    if (!m_hwid.isEmpty())
        json.insert(QStringLiteral("hwid"), m_hwid);

    qWarning() << "[POWER] notifyPowerOffline terminal" << termId;

    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));

    QEventLoop loop;
    QTimer killer;
    killer.setSingleShot(true);
    QObject::connect(&killer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    killer.start(2000);
    loop.exec();

    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        qWarning() << "[POWER] offline ack:" << reply->readAll();
    } else {
        qWarning() << "[POWER] offline notify failed:"
                   << (reply->isFinished() ? reply->errorString() : QStringLiteral("timeout"));
    }
    reply->deleteLater();
}

bool NetworkManager::hasRelayConfig() const
{
    return m_fanAvailable
        && !m_fanRelayHost.isEmpty()
        && m_fanRelayPort > 0
        && m_fanRelayChannel >= 1
        && m_fanRelayChannel <= 16
        && m_fanRelayChannel2 >= 1
        && m_fanRelayChannel2 <= 16
        && m_fanRelayChannel != m_fanRelayChannel2;
}

void NetworkManager::setFanDebug(const QString &line)
{
    qWarning().noquote() << "[FAN]" << line;
    if (m_fanDebug == line)
        return;
    m_fanDebug = line;
    emit fanDebugChanged();
}

int NetworkManager::computeLocalDesiredPower(const QJsonObject &fanObj) const
{
    const QString mode = fanObj.value(QStringLiteral("manual_mode")).toString(m_fanMode);

    if (mode == QLatin1String("force_off"))
        return 1; // night / duty 120V ≈ 50%
    if (mode == QLatin1String("force_on")) {
        int s = fanObj.value(QStringLiteral("desired_power")).toInt(0);
        if (s <= 0)
            s = fanObj.value(QStringLiteral("default_on_power")).toInt(3);
        if (s <= 0)
            s = 3;
        if (s > 3)
            s = 3;
        return s;
    }

    const QJsonObject facts = fanObj.value(QStringLiteral("facts")).toObject();
    const bool session = facts.value(QStringLiteral("session")).toBool(false);
    const bool thermal = facts.value(QStringLiteral("thermal")).toBool(false);

    if (session)
        return 3;

    if ((m_postBootCooldown || thermal) && thermal)
        return 2;

    return 1;
}

void NetworkManager::applyDesiredToRelay(int desiredPower, const QString &source)
{
    setFanDebug(QStringLiteral("apply want=%1 source=%2 http://%3/%4/ K1=%5 K2=%6 avail=%7")
                    .arg(desiredPower)
                    .arg(source)
                    .arg(m_fanRelayHost)
                    .arg(m_fanRelayPort)
                    .arg(m_fanRelayChannel)
                    .arg(m_fanRelayChannel2)
                    .arg(hasRelayConfig() ? QStringLiteral("yes") : QStringLiteral("NO")));

    if (!hasRelayConfig()) {
        setFanDebug(QStringLiteral("SKIP: no relay config (available=%1 host='%2' ch=%3/%4)")
                        .arg(m_fanAvailable ? 1 : 0)
                        .arg(m_fanRelayHost)
                        .arg(m_fanRelayChannel)
                        .arg(m_fanRelayChannel2));
        return;
    }
    if (m_fanApplyInFlight) {
        setFanDebug(QStringLiteral("SKIP: apply already in flight"));
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_fanRelayUnreachableUntilMs > now) {
        const int leftSec = int((m_fanRelayUnreachableUntilMs - now + 999) / 1000);
        setFanDebug(QStringLiteral("LAN unreachable → http://%1/%2/… (retry %3s)")
                        .arg(m_fanRelayHost)
                        .arg(m_fanRelayPort)
                        .arg(leftSec));
        return;
    }

    int want = desiredPower;
    if (want <= 0)
        want = 1;
    if (want > 3)
        want = 3;

    m_fanApplyInFlight = true;
    QString error;
    int applied = m_fanAppliedPower;

    setFanDebug(QStringLiteral("GET status http://%1/%2/99 …")
                    .arg(m_fanRelayHost)
                    .arg(m_fanRelayPort));
    auto status = FanRelayController::readStatus(m_fanRelayHost, m_fanRelayPort);
    int current = -1;
    if (status.ok) {
        m_fanRelayUnreachableUntilMs = 0;
        current = FanRelayController::speedFromStatus(
            status.body, m_fanRelayChannel, m_fanRelayChannel2);
        setFanDebug(QStringLiteral("status ok body='%1' decodedSpeed=%2 want=%3")
                        .arg(status.body.left(32))
                        .arg(current)
                        .arg(want));
    } else {
        // /99 уже не отвечает — setSpeed по тому же IP бессмысленен, только удлиняет зависание UI.
        m_fanRelayUnreachableUntilMs = now + 45000;
        setFanDebug(QStringLiteral("status FAIL: %1 → http://%2/%3/99. Backoff 45с.")
                        .arg(status.error, m_fanRelayHost)
                        .arg(m_fanRelayPort));
        acknowledgeFanApplied(applied, status.error, source);
        m_fanApplyInFlight = false;
        return;
    }

    if (current == want) {
        applied = want;
        setFanDebug(QStringLiteral("already at speed %1 — ack only").arg(want));
    } else {
        setFanDebug(QStringLiteral("setSpeed %1 → http://%2/%3/…")
                        .arg(want)
                        .arg(m_fanRelayHost)
                        .arg(m_fanRelayPort));
        const int got = FanRelayController::setSpeed(
            m_fanRelayHost, m_fanRelayPort,
            m_fanRelayChannel, m_fanRelayChannel2,
            want, &error);
        if (got > 0) {
            applied = got;
            m_fanRelayUnreachableUntilMs = 0;
            setFanDebug(QStringLiteral("setSpeed OK → speed %1").arg(got));
        } else {
            m_fanRelayUnreachableUntilMs = now + 45000;
            setFanDebug(QStringLiteral("setSpeed FAIL: %1").arg(error));
        }
    }

    m_fanAppliedPower = applied;
    const bool on = applied >= 2;
    if (m_fanOn != on) {
        m_fanOn = on;
        emit fanStateChanged();
    } else {
        emit fanStateChanged(); // refresh fanSpeed
    }

    acknowledgeFanApplied(applied, error, source);
    m_fanApplyInFlight = false;
}

void NetworkManager::ensureFanOffBeforeExit()
{
    if (m_fanRelayHost.isEmpty()
        || m_fanRelayChannel < 1 || m_fanRelayChannel > 16
        || m_fanRelayChannel2 < 1 || m_fanRelayChannel2 > 16)
        return;

    QString error;
    int applied = FanRelayController::setSpeed(
        m_fanRelayHost, m_fanRelayPort,
        m_fanRelayChannel, m_fanRelayChannel2,
        1, &error);

    if (applied < 0)
        applied = 1;

    m_fanAppliedPower = applied;
    if (m_fanOn) {
        m_fanOn = false;
        emit fanStateChanged();
    }
    acknowledgeFanApplied(applied, error, QStringLiteral("status_read"));
}

void NetworkManager::acknowledgeFanApplied(int appliedPower, const QString &error, const QString &source)
{
    const int termId = resolveTerminalId(0);
    if (m_serverUrl.isEmpty() || termId <= 0)
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/fan/applied"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    request.setTransferTimeout(2000);

    QJsonObject json;
    json.insert(QStringLiteral("terminal_id"), termId);
    json.insert(QStringLiteral("applied_power"), appliedPower);
    json.insert(QStringLiteral("source"), source);
    if (!error.isEmpty())
        json.insert(QStringLiteral("last_error"), error);

    // Sync on exit paths; async otherwise.
    const bool sync = source == QLatin1String("status_read") && !m_climateActive;
    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));

    if (sync) {
        QEventLoop loop;
        QTimer killer;
        killer.setSingleShot(true);
        QObject::connect(&killer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        killer.start(2000);
        loop.exec();
        if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
            const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
            m_skipRelayApply = true;
            applyFanStateFromJson(root.value(QStringLiteral("fan")).toObject());
            m_skipRelayApply = false;
        }
        reply->deleteLater();
        return;
    }

    if (m_fanAckInFlight) {
        reply->abort();
        reply->deleteLater();
        return;
    }
    m_fanAckInFlight = true;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_fanAckInFlight = false;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[FAN] ack failed:" << reply->errorString();
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        if (root.value(QStringLiteral("status")).toString() == QLatin1String("success")
            || root.value(QStringLiteral("status")).toString() == QLatin1String("locked")) {
            m_skipRelayApply = true;
            applyFanStateFromJson(root.value(QStringLiteral("fan")).toObject());
            m_skipRelayApply = false;
        }
    });
}

void NetworkManager::applyFanStateFromJson(const QJsonObject &fanObj)
{
    if (fanObj.isEmpty())
        return;

    const bool available = fanObj.value(QStringLiteral("available")).toBool(false);
    const QString mode = fanObj.value(QStringLiteral("manual_mode")).toString();
    const int lockSec = fanObj.value(QStringLiteral("manual_lock")).toObject()
                            .value(QStringLiteral("remaining_sec")).toInt(0);

    const QJsonObject relay = fanObj.value(QStringLiteral("relay")).toObject();
    if (!relay.isEmpty()) {
        m_fanRelayHost = relay.value(QStringLiteral("host")).toString();
        m_fanRelayPort = relay.value(QStringLiteral("port")).toInt(30000);
        m_fanRelayChannel = relay.value(QStringLiteral("channel")).toInt(0);
        m_fanRelayChannel2 = relay.value(QStringLiteral("channel2")).toInt(0);
    }

    m_fanDefaultOnPower = 3;
    m_fanAppliedPower = fanObj.value(QStringLiteral("applied_power")).toInt(m_fanAppliedPower);
    if (m_fanAppliedPower <= 0)
        m_fanAppliedPower = 1;
    if (m_fanAppliedPower > 3)
        m_fanAppliedPower = 3;

    const QJsonObject facts = fanObj.value(QStringLiteral("facts")).toObject();
    if (m_postBootCooldown && !facts.value(QStringLiteral("thermal")).toBool(false)
        && !facts.value(QStringLiteral("session")).toBool(false)) {
        m_postBootCooldown = false;
    }
    if (facts.value(QStringLiteral("session")).toBool(false))
        m_postBootCooldown = false;

    const int desired = computeLocalDesiredPower(fanObj);
    // Не затирать детальный лог apply/setSpeed после ack (skip path).
    if (!m_skipRelayApply) {
        setFanDebug(QStringLiteral("state mode=%1 desired=%2 applied=%3 http://%4/%5/ K1=%6 K2=%7")
                        .arg(mode)
                        .arg(desired)
                        .arg(m_fanAppliedPower)
                        .arg(m_fanRelayHost)
                        .arg(m_fanRelayPort)
                        .arg(m_fanRelayChannel)
                        .arg(m_fanRelayChannel2));
    }
    const bool uiOn = desired >= 2;

    const bool changed = (available != m_fanAvailable) || (uiOn != m_fanOn)
        || (mode != m_fanMode) || (lockSec != m_fanManualLockSec);
    m_fanAvailable = available;
    m_fanOn = uiOn;
    m_fanMode = mode;
    m_fanManualLockSec = lockSec;
    if (changed)
        emit fanStateChanged();

    if (m_skipRelayApply)
        return;

    const QJsonObject autoLock = fanObj.value(QStringLiteral("auto_lock")).toObject();
    const bool autoLocked = autoLock.value(QStringLiteral("locked")).toBool(false);
    // auto_lock — только для фонового poll; ручной setFan всегда жмёт реле сразу.
    if (hasRelayConfig() && (!autoLocked || m_forceRelayApply)) {
        applyDesiredToRelay(desired, QStringLiteral("command"));
    } else if (!hasRelayConfig()) {
        setFanDebug(QStringLiteral("no relay yet mode=%1 desired=%2 avail=%3 host='%4'")
                        .arg(mode)
                        .arg(desired)
                        .arg(m_fanAvailable ? 1 : 0)
                        .arg(m_fanRelayHost));
    } else if (autoLocked) {
        setFanDebug(QStringLiteral("auto_lock %1s — skip background apply (want=%2)")
                        .arg(autoLock.value(QStringLiteral("remaining_sec")).toInt())
                        .arg(desired));
    }
}

void NetworkManager::startClimateControl()
{
    if (m_climateActive)
        return;

    m_climateActive = true;
    if (!m_climateTimer) {
        m_climateTimer = new QTimer(this);
        m_climateTimer->setInterval(10000);
        connect(m_climateTimer, &QTimer::timeout, this, [this]() {
            reportThermalNow();
            fetchFanState();
        });
    }

    fetchFanState();
    reportThermalNow();
    m_climateTimer->start();
}

void NetworkManager::stopClimateControl()
{
    m_climateActive = false;
    if (m_climateTimer)
        m_climateTimer->stop();
}

void NetworkManager::fetchFanState()
{
    const int termId = resolveTerminalId(0);
    if (m_serverUrl.isEmpty() || termId <= 0)
        return;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/fan"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("terminal_id"), QString::number(termId));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[CLIMATE] getFanState failed:" << reply->errorString();
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        if (root.value(QStringLiteral("status")).toString() != QLatin1String("success"))
            return;
        applyFanStateFromJson(root.value(QStringLiteral("fan")).toObject());
    });
}

void NetworkManager::setFan(const QString &action)
{
    const int termId = resolveTerminalId(0);
    if (m_serverUrl.isEmpty() || termId <= 0) {
        setFanDebug(QStringLiteral("setFan: no server/terminal"));
        return;
    }

    const QString normalized = action.trimmed().toLower();
    if (normalized != QLatin1String("on")
        && normalized != QLatin1String("off")
        && normalized != QLatin1String("auto")
        && normalized != QLatin1String("50")
        && normalized != QLatin1String("75")
        && normalized != QLatin1String("100")) {
        setFanDebug(QStringLiteral("setFan: bad action %1").arg(action));
        return;
    }

    if (m_fanRequestInFlight) {
        setFanDebug(QStringLiteral("setFan: request in flight"));
        return;
    }
    // manual_lock на чужих ПК проверяет бэкенд; свой терминал может менять скорость сразу
    m_fanRequestInFlight = true;
    setFanDebug(QStringLiteral("setFan POST action=%1 term=%2").arg(normalized).arg(termId));

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/fan"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QJsonObject json;
    json.insert(QStringLiteral("terminal_id"), termId);
    json.insert(QStringLiteral("action"), normalized);

    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, normalized]() {
        reply->deleteLater();
        m_fanRequestInFlight = false;

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        const QJsonObject root = QJsonDocument::fromJson(raw).object();

        if (reply->error() != QNetworkReply::NoError && httpStatus != 423) {
            setFanDebug(QStringLiteral("setFan HTTP fail %1 %2").arg(httpStatus).arg(reply->errorString()));
            fetchFanState();
            return;
        }

        setFanDebug(QStringLiteral("setFan OK action=%1 mode=%2 desired=%3")
                        .arg(normalized)
                        .arg(root.value(QStringLiteral("fan")).toObject()
                                 .value(QStringLiteral("manual_mode")).toString())
                        .arg(root.value(QStringLiteral("fan")).toObject()
                                 .value(QStringLiteral("desired_power")).toInt()));
        m_fanRelayUnreachableUntilMs = 0; // ручной клик — сразу пробуем LAN снова
        m_forceRelayApply = true;
        applyFanStateFromJson(root.value(QStringLiteral("fan")).toObject());
        m_forceRelayApply = false;
        if (root.value(QStringLiteral("status")).toString() == QLatin1String("locked")
            || httpStatus == 423) {
            setFanDebug(QStringLiteral("setFan cooldown %1s")
                            .arg(root.value(QStringLiteral("remaining_sec")).toInt()));
        }
    });
}

void NetworkManager::postThermal(double cpuC)
{
    const int termId = resolveTerminalId(0);
    if (m_serverUrl.isEmpty() || termId <= 0 || cpuC < 0.0)
        return;
    if (m_thermalRequestInFlight)
        return;
    m_thermalRequestInFlight = true;

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/thermal"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));

    QJsonObject json;
    json.insert(QStringLiteral("terminal_id"), termId);
    json.insert(QStringLiteral("cpu_c"), cpuC);

    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cpuC]() {
        reply->deleteLater();
        m_thermalRequestInFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[CLIMATE] reportThermal failed:" << reply->errorString();
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        if (root.value(QStringLiteral("status")).toString() != QLatin1String("success"))
            return;

        if (m_cpuTempC != cpuC) {
            m_cpuTempC = cpuC;
            emit cpuTempChanged();
        }
        applyFanStateFromJson(root.value(QStringLiteral("fan")).toObject());
    });
}

void NetworkManager::reportThermalNow()
{
    const double cpuC = ThermalMonitor::readCpuCelsius();
    if (cpuC < 0.0)
        return;

    if (m_cpuTempC != cpuC) {
        m_cpuTempC = cpuC;
        emit cpuTempChanged();
    }
    postThermal(cpuC);
}

void NetworkManager::abortAiAssistant()
{
    if (!m_aiAssistantReply)
        return;
    QNetworkReply *reply = m_aiAssistantReply;
    m_aiAssistantReply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void NetworkManager::askAiAssistant(int terminalId, const QString &audioPath,
                                    int gameId, const QString &gameTitle)
{
    abortAiAssistant();

    if (m_serverUrl.isEmpty() || terminalId <= 0) {
        emit aiAssistantFailed(QStringLiteral("Терминал не готов"));
        return;
    }
    if (!QFile::exists(audioPath)) {
        emit aiAssistantFailed(QStringLiteral("Файл записи не найден"));
        return;
    }

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/ai-assistant"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    request.setTransferTimeout(90000);

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart terminalPart;
    terminalPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QStringLiteral("form-data; name=\"terminal_id\"")));
    terminalPart.setBody(QByteArray::number(terminalId));
    multiPart->append(terminalPart);

    if (gameId > 0) {
        QHttpPart gameIdPart;
        gameIdPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                             QVariant(QStringLiteral("form-data; name=\"game_id\"")));
        gameIdPart.setBody(QByteArray::number(gameId));
        multiPart->append(gameIdPart);
    }
    if (!gameTitle.trimmed().isEmpty()) {
        QHttpPart titlePart;
        titlePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                            QVariant(QStringLiteral("form-data; name=\"game_title\"")));
        titlePart.setBody(gameTitle.trimmed().toUtf8());
        multiPart->append(titlePart);
    }

    auto *file = new QFile(audioPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        delete multiPart;
        emit aiAssistantFailed(QStringLiteral("Не удалось открыть запись"));
        return;
    }

    QHttpPart audioPart;
    audioPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QVariant(QStringLiteral("form-data; name=\"audio\"; filename=\"ask.wav\"")));
    audioPart.setHeader(QNetworkRequest::ContentTypeHeader,
                        QVariant(QStringLiteral("audio/wav")));
    audioPart.setBodyDevice(file);
    file->setParent(multiPart);
    multiPart->append(audioPart);

    qWarning() << "[VOICE-NET] POST" << url.toString()
               << "terminal=" << terminalId
               << "game_id=" << gameId
               << "bytes=" << file->size();

    QNetworkReply *reply = m_networkManager->post(request, multiPart);
    multiPart->setParent(reply);
    m_aiAssistantReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_aiAssistantReply != reply) {
            // Aborted / superseded.
            return;
        }
        m_aiAssistantReply = nullptr;

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (reply->error() == QNetworkReply::OperationCanceledError) {
            qWarning() << "[VOICE-NET] aborted";
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = QStringLiteral("Сеть: %1").arg(reply->errorString());
            const QJsonDocument errDoc = QJsonDocument::fromJson(body);
            if (errDoc.isObject()) {
                const QString m = errDoc.object().value(QStringLiteral("message")).toString();
                if (!m.isEmpty())
                    msg = m;
            }
            qWarning() << "[VOICE-NET] HTTP error" << httpStatus << msg;
            emit aiAssistantFailed(msg);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit aiAssistantFailed(QStringLiteral("Некорректный ответ сервера"));
            return;
        }

        const QJsonObject root = doc.object();
        if (root.value(QStringLiteral("status")).toString() != QLatin1String("success")) {
            const QString msg = root.value(QStringLiteral("message")).toString(
                QStringLiteral("Ошибка ассистента"));
            emit aiAssistantFailed(msg);
            return;
        }

        const QString b64 = root.value(QStringLiteral("audio_base64")).toString();
        const QByteArray audioBytes = QByteArray::fromBase64(b64.toUtf8());
        const QString mime = root.value(QStringLiteral("audio_mime")).toString(
            QStringLiteral("audio/mpeg"));
        const QString transcript = root.value(QStringLiteral("transcript")).toString();
        const QString replyText = root.value(QStringLiteral("reply_text")).toString();

        if (audioBytes.isEmpty()) {
            emit aiAssistantFailed(QStringLiteral("Пустой audio в ответе"));
            return;
        }

        emit aiAssistantSucceeded(audioBytes, mime, transcript, replyText);
    });
}

void NetworkManager::abortVoiceGreeting()
{
    if (!m_voiceGreetingReply)
        return;
    QNetworkReply *reply = m_voiceGreetingReply;
    m_voiceGreetingReply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void NetworkManager::requestVoiceGreeting(int terminalId, int bookingId)
{
    abortVoiceGreeting();

    if (m_serverUrl.isEmpty() || terminalId <= 0) {
        emit voiceGreetingFailed(QStringLiteral("Терминал не готов"));
        return;
    }

    QUrl url(m_serverUrl + QStringLiteral("/api/shell/voice-greeting"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReactorShell/1.0"));
    request.setTransferTimeout(90000);

    QJsonObject json;
    json.insert(QStringLiteral("terminal_id"), terminalId);
    if (bookingId > 0)
        json.insert(QStringLiteral("booking_id"), bookingId);

    qWarning() << "[VOICE-NET] POST voice-greeting terminal=" << terminalId
               << "booking=" << bookingId;

    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    m_voiceGreetingReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_voiceGreetingReply != reply)
            return;
        m_voiceGreetingReply = nullptr;

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (reply->error() == QNetworkReply::OperationCanceledError) {
            qWarning() << "[VOICE-NET] voice-greeting aborted";
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = QStringLiteral("Сеть: %1").arg(reply->errorString());
            const QJsonDocument errDoc = QJsonDocument::fromJson(body);
            if (errDoc.isObject()) {
                const QString m = errDoc.object().value(QStringLiteral("message")).toString();
                if (!m.isEmpty())
                    msg = m;
            }
            qWarning() << "[VOICE-NET] voice-greeting HTTP" << httpStatus << msg;
            emit voiceGreetingFailed(msg);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit voiceGreetingFailed(QStringLiteral("Некорректный ответ сервера"));
            return;
        }

        const QJsonObject root = doc.object();
        if (root.value(QStringLiteral("status")).toString() != QLatin1String("success")) {
            emit voiceGreetingFailed(root.value(QStringLiteral("message")).toString(
                QStringLiteral("Ошибка приветствия")));
            return;
        }

        const QByteArray audioBytes = QByteArray::fromBase64(
            root.value(QStringLiteral("audio_base64")).toString().toUtf8());
        if (audioBytes.isEmpty()) {
            emit voiceGreetingFailed(QStringLiteral("Пустой audio в приветствии"));
            return;
        }

        emit voiceGreetingSucceeded(
            audioBytes,
            root.value(QStringLiteral("audio_mime")).toString(QStringLiteral("audio/mpeg")),
            root.value(QStringLiteral("reply_text")).toString(),
            root.value(QStringLiteral("is_first_visit")).toBool(false));
    });
}