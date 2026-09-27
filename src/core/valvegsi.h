#ifndef VALVEGSI_H
#define VALVEGSI_H

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "localhttpserver.h"
#include "reactivelighting.h"

/** CS2 + Dota 2 Game State Integration on 127.0.0.1:59898. */
class ValveGsi : public QObject
{
    Q_OBJECT
public:
    static const quint16 kPort = 59898;

    explicit ValveGsi(ReactiveLighting *hub, QObject *parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

signals:
    /** kill / death / round_win / round_loss / match_win / bomb / freezetime / heartbeat */
    void gameEvent(const QJsonObject &payload);
    void matchStateChanged(bool inMatch);

private:
    LocalHttpResponse onHttp(const LocalHttpRequest &req);
    void handlePayload(const QJsonObject &root);
    void handleCs2(const QJsonObject &root);
    void handleDota(const QJsonObject &root);
    bool writeConfigs();
    void removeConfigs();
    QStringList cfgDirs(bool dota) const;
    QString cfgBody(bool dota) const;
    void playLight(const QString &id, ReactiveLighting::Priority pri, const QString &hint);
    void releaseLight(const QString &id);
    void emitSnapshot(const QString &event, const QJsonObject &extra);
    static QString activeWeapon(const QJsonObject &player);
    static QJsonObject dotaUlt(const QJsonObject &root);
    void updateCs2Ambient(const QJsonObject &player, const QString &mapName, int burning,
                          bool inRound);
    void updateCs2Flash(int flashed, int prevFlashed);
    void clearCs2Ambient();

    ReactiveLighting *m_hub = nullptr;
    LocalHttpServer m_server;
    bool m_enabled = false;
    bool m_cs2Bomb = false;
    bool m_cs2Dead = false;
    bool m_cs2Flashed = false;
    bool m_dotaDead = false;
    bool m_inMatch = false;
    QString m_game;
    QString m_cs2Ambient;
    int m_cs2Kills = -1;
    int m_cs2RoundKills = -1;
    qint64 m_lastHeartbeatMs = 0;

    QString m_steamId;
    QString m_map;
    QString m_mapMode;
    QString m_mapPhase;
    QString m_customGame;
    QString m_matchId;
    bool m_hasBots = false;
    QString m_team;
    QString m_weapon;
    QString m_phase;
    QString m_hero;
    QString m_ultName;
    QString m_bomb;
    QString m_playerName;
    int m_round = -1;
    int m_money = -1;
    int m_gameTime = -1;
    bool m_ultReady = false;
    bool m_alive = true;
};

#endif
