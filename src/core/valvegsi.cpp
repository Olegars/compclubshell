#include "valvegsi.h"

#include "pathresolver.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QtMath>

namespace {
const char kToken[] = "reactor-club";
const char kCfgName[] = "gamestate_integration_reactor.cfg";
}

ValveGsi::ValveGsi(ReactiveLighting *hub, QObject *parent)
    : QObject(parent)
    , m_hub(hub)
{
    m_server.setHandler([this](const LocalHttpRequest &req) { return onHttp(req); });
}

void ValveGsi::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (on) {
        if (!m_server.listen(kPort)) {
            qWarning() << "[GSI] listen failed" << kPort;
        } else {
            qWarning() << "[GSI] listening 127.0.0.1:" << kPort;
            writeConfigs();
        }
    } else {
        m_cs2Bomb = false;
        m_cs2Dead = false;
        m_cs2Flashed = false;
        m_dotaDead = false;
        m_cs2Kills = -1;
        m_cs2RoundKills = -1;
        m_mapMode.clear();
        m_mapPhase.clear();
        m_customGame.clear();
        m_hasBots = false;
        if (m_inMatch) {
            m_inMatch = false;
            emit matchStateChanged(false);
        }
        releaseLight(QStringLiteral("cs2.bomb"));
        releaseLight(QStringLiteral("cs2.win"));
        releaseLight(QStringLiteral("cs2.death"));
        releaseLight(QStringLiteral("cs2.flash"));
        releaseLight(QStringLiteral("dota.win"));
        releaseLight(QStringLiteral("dota.death"));
        clearCs2Ambient();
        m_server.close();
        removeConfigs();
    }
}

LocalHttpResponse ValveGsi::onHttp(const LocalHttpRequest &req)
{
    LocalHttpResponse res;
    res.body.clear();
    res.contentType = "text/plain";
    if (!m_enabled || req.method != QLatin1String("POST"))
        return res;
    const QJsonObject root = QJsonDocument::fromJson(req.body).object();
    if (root.isEmpty())
        return res;
    const QString token = root.value(QStringLiteral("auth")).toObject()
                              .value(QStringLiteral("token")).toString();
    if (!token.isEmpty() && token != QLatin1String(kToken))
        return res;
    handlePayload(root);
    return res;
}

void ValveGsi::handlePayload(const QJsonObject &root)
{
    const QJsonObject provider = root.value(QStringLiteral("provider")).toObject();
    const int appid = provider.value(QStringLiteral("appid")).toInt(0);
    const QString name = provider.value(QStringLiteral("name")).toString().toLower();
    if (appid == 570 || name.contains(QLatin1String("dota")))
        handleDota(root);
    else
        handleCs2(root);
}

void ValveGsi::playLight(const QString &id, ReactiveLighting::Priority pri, const QString &hint)
{
    if (m_hub && m_hub->isEnabled())
        m_hub->playPreset(id, pri, hint);
}

void ValveGsi::releaseLight(const QString &id)
{
    if (m_hub)
        m_hub->release(id);
}

namespace {
struct GsiVec3 {
    bool ok = false;
    float x = 0;
    float y = 0;
    float z = 0;
};

GsiVec3 parseGsiPosition(const QJsonObject &player)
{
    GsiVec3 out;
    const QJsonValue v = player.value(QStringLiteral("position"));
    if (v.isString()) {
        const QStringList parts = v.toString().split(QLatin1Char(','));
        if (parts.size() >= 2) {
            bool okX = false;
            bool okY = false;
            out.x = parts.at(0).trimmed().toFloat(&okX);
            out.y = parts.at(1).trimmed().toFloat(&okY);
            out.ok = okX && okY;
            if (parts.size() >= 3) {
                bool okZ = false;
                out.z = parts.at(2).trimmed().toFloat(&okZ);
                Q_UNUSED(okZ);
            }
        }
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        out.x = float(o.value(QStringLiteral("x")).toDouble());
        out.y = float(o.value(QStringLiteral("y")).toDouble());
        out.z = float(o.value(QStringLiteral("z")).toDouble());
        out.ok = o.contains(QStringLiteral("x")) && o.contains(QStringLiteral("y"));
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() >= 2) {
            out.x = float(a.at(0).toDouble());
            out.y = float(a.at(1).toDouble());
            if (a.size() >= 3)
                out.z = float(a.at(2).toDouble());
            out.ok = true;
        }
    }
    if (out.ok && qFuzzyIsNull(out.x) && qFuzzyIsNull(out.y) && qFuzzyIsNull(out.z))
        out.ok = false;
    return out;
}

bool mapNameHas(const QString &map, const QString &token)
{
    return map.contains(token, Qt::CaseInsensitive);
}

bool isSteamId64(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^7656\\d{13}$"));
    return re.match(id.trimmed()).hasMatch();
}

/** CS2 allplayers: боты приходят как steamid BOT / 0, без SteamID64. */
bool rosterHasBots(const QJsonObject &allplayers)
{
    for (auto it = allplayers.begin(); it != allplayers.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject player = it.value().toObject();
        const QString team = player.value(QStringLiteral("team")).toString().trimmed().toUpper();
        if (!team.isEmpty() && team != QLatin1String("T") && team != QLatin1String("CT"))
            continue;
        QString steam = player.value(QStringLiteral("steamid")).toString().trimmed();
        if (steam.isEmpty())
            steam = it.key().trimmed();
        const QString low = steam.toLower();
        if (low == QLatin1String("bot") || low == QLatin1String("0") || !isSteamId64(steam))
            return true;
    }
    return false;
}

bool ancientInsideCaves(const GsiVec3 &p)
{
    if (!p.ok)
        return false;
    const bool caveB = p.x >= -400.f && p.x <= 1650.f && p.y >= -2200.f && p.y <= -80.f;
    const bool caveMid = p.x >= -650.f && p.x <= 250.f && p.y >= -450.f && p.y <= 180.f
        && p.z < 140.f;
    return caveB || caveMid;
}
}

void ValveGsi::clearCs2Ambient()
{
    if (m_cs2Ambient.isEmpty())
        return;
    releaseLight(m_cs2Ambient);
    m_cs2Ambient.clear();
}

void ValveGsi::updateCs2Flash(int flashed, int prevFlashed)
{
    if (flashed < 40)
        m_cs2Flashed = false;
    if (flashed < 80)
        return;
    if (m_cs2Flashed && flashed < prevFlashed + 40)
        return;
    m_cs2Flashed = true;
    playLight(QStringLiteral("cs2.flash"), ReactiveLighting::Alert, QStringLiteral("слепота"));
}

void ValveGsi::updateCs2Ambient(const QJsonObject &player, const QString &mapName, int burning,
                               bool inRound)
{
    if (!inRound) {
        clearCs2Ambient();
        return;
    }

    QString want;
    if (burning >= 20 || mapNameHas(mapName, QStringLiteral("inferno"))) {
        want = QStringLiteral("cs2.ambient.inferno");
    } else if (mapNameHas(mapName, QStringLiteral("nuke"))) {
        want = QStringLiteral("cs2.ambient.winter");
    } else if (mapNameHas(mapName, QStringLiteral("ancient"))) {
        if (!ancientInsideCaves(parseGsiPosition(player)))
            want = QStringLiteral("cs2.ambient.winter");
    }

    if (want == m_cs2Ambient)
        return;
    if (!m_hub || !m_hub->isEnabled()) {
        clearCs2Ambient();
        return;
    }
    clearCs2Ambient();
    m_cs2Ambient = want;
    if (want.isEmpty())
        return;
    const QString hint = want.endsWith(QLatin1String("inferno"))
        ? QStringLiteral("огонь")
        : QStringLiteral("зима");
    playLight(want, ReactiveLighting::Ambient, hint);
}

void ValveGsi::emitSnapshot(const QString &event, const QJsonObject &extra)
{
    QJsonObject payload = extra;
    payload.insert(QStringLiteral("event"), event);
    payload.insert(QStringLiteral("game"), m_game);
    payload.insert(QStringLiteral("steam_id"), m_steamId);
    payload.insert(QStringLiteral("map"), m_map.left(120));
    payload.insert(QStringLiteral("match_id"), m_matchId);
    payload.insert(QStringLiteral("team"), m_team);
    payload.insert(QStringLiteral("weapon"), m_weapon);
    payload.insert(QStringLiteral("phase"), m_phase);
    payload.insert(QStringLiteral("hero"), m_hero);
    payload.insert(QStringLiteral("ult_name"), m_ultName);
    payload.insert(QStringLiteral("bomb"), m_bomb);
    if (!m_playerName.isEmpty())
        payload.insert(QStringLiteral("player_name"), m_playerName);
    payload.insert(QStringLiteral("in_match"), m_inMatch);
    payload.insert(QStringLiteral("ult_ready"), m_ultReady);
    payload.insert(QStringLiteral("alive"), m_alive);
    if (!m_mapMode.isEmpty())
        payload.insert(QStringLiteral("map_mode"), m_mapMode.left(32));
    if (!m_mapPhase.isEmpty())
        payload.insert(QStringLiteral("map_phase"), m_mapPhase.left(24));
    if (!m_customGame.isEmpty())
        payload.insert(QStringLiteral("custom_game"), m_customGame.left(64));
    payload.insert(QStringLiteral("has_bots"), m_hasBots);
    if (m_round >= 0)
        payload.insert(QStringLiteral("round"), m_round);
    if (m_money >= 0)
        payload.insert(QStringLiteral("money"), m_money);
    if (m_gameTime >= 0)
        payload.insert(QStringLiteral("game_time"), m_gameTime);
    emit gameEvent(payload);
}

QString ValveGsi::activeWeapon(const QJsonObject &player)
{
    const QJsonObject weapons = player.value(QStringLiteral("weapons")).toObject();
    for (auto it = weapons.begin(); it != weapons.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject w = it.value().toObject();
        if (w.value(QStringLiteral("state")).toString() == QLatin1String("active"))
            return w.value(QStringLiteral("name")).toString();
    }
    return QString();
}

QJsonObject ValveGsi::dotaUlt(const QJsonObject &root)
{
    QJsonObject out;
    const QJsonObject abilities = root.value(QStringLiteral("abilities")).toObject();
    for (auto it = abilities.begin(); it != abilities.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject a = it.value().toObject();
        const bool ult = a.value(QStringLiteral("ultimate")).toBool(false)
            || it.key().contains(QLatin1String("ability5"))
            || it.key().contains(QLatin1String("ability6"));
        if (!ult)
            continue;
        out.insert(QStringLiteral("name"), a.value(QStringLiteral("name")).toString());
        const double cd = a.value(QStringLiteral("cooldown")).toDouble(1);
        const bool can = a.value(QStringLiteral("can_cast")).toBool(false);
        out.insert(QStringLiteral("ready"), can || cd <= 0.05);
        return out;
    }
    return out;
}

void ValveGsi::handleCs2(const QJsonObject &root)
{
    m_game = QStringLiteral("cs2");
    m_customGame.clear();
    const QJsonObject round = root.value(QStringLiteral("round")).toObject();
    const QJsonObject prev = root.value(QStringLiteral("previously")).toObject();
    const QJsonObject prevRound = prev.value(QStringLiteral("round")).toObject();
    const QJsonObject player = root.value(QStringLiteral("player")).toObject();
    const QJsonObject state = player.value(QStringLiteral("state")).toObject();
    const QJsonObject prevState = prev.value(QStringLiteral("player")).toObject()
                                      .value(QStringLiteral("state")).toObject();
    const QJsonObject map = root.value(QStringLiteral("map")).toObject();
    const QJsonObject stats = player.value(QStringLiteral("match_stats")).toObject();

    m_steamId = player.value(QStringLiteral("steamid")).toString();
    m_playerName = player.value(QStringLiteral("name")).toString();
    if (map.contains(QStringLiteral("name"))) {
        const QString nextMap = map.value(QStringLiteral("name")).toString();
        if (nextMap != m_map)
            m_hasBots = false;
        m_map = nextMap;
    }
    if (map.contains(QStringLiteral("mode")))
        m_mapMode = map.value(QStringLiteral("mode")).toString();
    if (map.contains(QStringLiteral("phase")))
        m_mapPhase = map.value(QStringLiteral("phase")).toString();
    const int mapRound = map.value(QStringLiteral("round")).toInt(-1);
    if (mapRound >= 0 && m_round >= 0 && mapRound < m_round)
        m_hasBots = false;
    if (root.contains(QStringLiteral("allplayers")))
        m_hasBots = rosterHasBots(root.value(QStringLiteral("allplayers")).toObject());
    m_matchId = QString::number(mapRound);
    m_team = player.value(QStringLiteral("team")).toString();
    m_weapon = activeWeapon(player);
    m_phase = round.value(QStringLiteral("phase")).toString();
    m_bomb = round.value(QStringLiteral("bomb")).toString();
    m_round = map.value(QStringLiteral("round")).toInt(-1);
    m_money = state.value(QStringLiteral("money")).toInt(-1);
    const int health = state.value(QStringLiteral("health")).toInt(-1);
    const int prevHealth = prevState.value(QStringLiteral("health")).toInt(-1);
    const int flashed = state.value(QStringLiteral("flashed")).toInt(0);
    const int prevFlashed = prevState.value(QStringLiteral("flashed")).toInt(0);
    const int burning = state.value(QStringLiteral("burning")).toInt(0);
    m_alive = health > 0;
    const QString winTeam = round.value(QStringLiteral("win_team")).toString();
    const QString prevPhase = prevRound.value(QStringLiteral("phase")).toString();
    const bool inRound = m_phase == QLatin1String("live")
        || m_phase == QLatin1String("freezetime")
        || m_phase == QLatin1String("over")
        || m_phase == QLatin1String("warmup");
    if (m_inMatch != inRound) {
        m_inMatch = inRound;
        emit matchStateChanged(m_inMatch);
    }

    updateCs2Flash(flashed, prevFlashed);
    updateCs2Ambient(player, m_map, burning, inRound);

    const int kills = stats.value(QStringLiteral("kills")).toInt(-1);
    const int roundKills = state.value(QStringLiteral("round_kills")).toInt(-1);

    if (m_bomb == QLatin1String("planted") && !m_cs2Bomb) {
        m_cs2Bomb = true;
        playLight(QStringLiteral("cs2.bomb"), ReactiveLighting::Alert, QStringLiteral("бомба"));
        emitSnapshot(QStringLiteral("bomb"), {});
        return;
    }
    if (m_cs2Bomb && m_bomb != QLatin1String("planted") && !m_bomb.isEmpty()) {
        m_cs2Bomb = false;
        releaseLight(QStringLiteral("cs2.bomb"));
    }

    if (m_phase == QLatin1String("over") && prevPhase != QLatin1String("over") && !winTeam.isEmpty()) {
        m_cs2Bomb = false;
        releaseLight(QStringLiteral("cs2.bomb"));
        playLight(QStringLiteral("cs2.win"), ReactiveLighting::Round, QStringLiteral("раунд"));
        const bool won = !m_team.isEmpty() && winTeam.compare(m_team, Qt::CaseInsensitive) == 0;
        emitSnapshot(won ? QStringLiteral("round_win") : QStringLiteral("round_loss"), {});
        return;
    }

    const bool gotKill = (roundKills >= 0 && m_cs2RoundKills >= 0 && roundKills > m_cs2RoundKills)
        || (kills >= 0 && m_cs2Kills >= 0 && kills > m_cs2Kills);
    if (gotKill && inRound) {
        emitSnapshot(QStringLiteral("kill"), {});
    }
    if (roundKills >= 0)
        m_cs2RoundKills = roundKills;
    if (kills >= 0)
        m_cs2Kills = kills;

    if (health == 0 && !m_cs2Dead && inRound && (prevHealth > 0 || prevHealth < 0)) {
        m_cs2Dead = true;
        playLight(QStringLiteral("cs2.death"), ReactiveLighting::Event, QStringLiteral("смерть"));
        emitSnapshot(QStringLiteral("death"), {});
        return;
    }
    if (health > 0 && m_cs2Dead) {
        m_cs2Dead = false;
        releaseLight(QStringLiteral("cs2.death"));
    }

    if (m_phase == QLatin1String("freezetime") && prevPhase != QLatin1String("freezetime")) {
        m_cs2Bomb = false;
        m_cs2Dead = false;
        m_cs2RoundKills = 0;
        releaseLight(QStringLiteral("cs2.bomb"));
        releaseLight(QStringLiteral("cs2.win"));
        releaseLight(QStringLiteral("cs2.death"));
        emitSnapshot(QStringLiteral("freezetime"), {});
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_inMatch && now - m_lastHeartbeatMs >= 4000) {
        m_lastHeartbeatMs = now;
        emitSnapshot(QStringLiteral("heartbeat"), {});
    }
}

void ValveGsi::handleDota(const QJsonObject &root)
{
    m_game = QStringLiteral("dota");
    m_mapMode.clear();
    m_mapPhase.clear();
    m_hasBots = false;
    const QJsonObject hero = root.value(QStringLiteral("hero")).toObject();
    const QJsonObject map = root.value(QStringLiteral("map")).toObject();
    const QJsonObject player = root.value(QStringLiteral("player")).toObject();
    const QJsonObject prev = root.value(QStringLiteral("previously")).toObject();
    const QJsonObject prevHero = prev.value(QStringLiteral("hero")).toObject();
    const QJsonObject prevMap = prev.value(QStringLiteral("map")).toObject();
    const QJsonObject prevPlayer = prev.value(QStringLiteral("player")).toObject();

    m_steamId = player.value(QStringLiteral("steamid")).toString();
    m_playerName = player.value(QStringLiteral("name")).toString();
    m_map = QStringLiteral("dota");
    m_matchId = map.value(QStringLiteral("matchid")).toVariant().toString();
    if (map.contains(QStringLiteral("customgamename")))
        m_customGame = map.value(QStringLiteral("customgamename")).toString();
    m_team = player.value(QStringLiteral("team_name")).toString();
    if (m_team.isEmpty())
        m_team = QString::number(player.value(QStringLiteral("team")).toInt(0));
    m_hero = hero.value(QStringLiteral("name")).toString();
    m_gameTime = map.value(QStringLiteral("clock_time")).toInt(-1);
    if (m_gameTime < 0)
        m_gameTime = map.value(QStringLiteral("game_time")).toInt(-1);
    const QJsonObject ult = dotaUlt(root);
    m_ultName = ult.value(QStringLiteral("name")).toString();
    m_ultReady = ult.value(QStringLiteral("ready")).toBool(false);
    m_money = player.value(QStringLiteral("gold")).toInt(-1);

    const QString win = map.value(QStringLiteral("win_team")).toString().toLower();
    const QString prevWin = prevMap.value(QStringLiteral("win_team")).toString().toLower();
    bool alive = hero.value(QStringLiteral("alive")).toBool(true);
    if (hero.contains(QStringLiteral("health")) && hero.value(QStringLiteral("health")).toInt(-1) == 0)
        alive = false;
    m_alive = alive;
    bool prevAlive = true;
    if (prevHero.contains(QStringLiteral("alive")))
        prevAlive = prevHero.value(QStringLiteral("alive")).toBool(true);
    else if (prevHero.contains(QStringLiteral("health")))
        prevAlive = prevHero.value(QStringLiteral("health")).toInt(1) > 0;

    const QString phase = map.value(QStringLiteral("game_state")).toString();
    m_phase = phase;
    const bool playing = phase.contains(QLatin1String("game"), Qt::CaseInsensitive)
        || map.value(QStringLiteral("paused")).toBool(false)
        || hero.contains(QStringLiteral("id"));
    if (m_inMatch != playing) {
        m_inMatch = playing;
        emit matchStateChanged(m_inMatch);
    }

    const int kills = player.value(QStringLiteral("kills")).toInt(-1);
    const int prevKills = prevPlayer.value(QStringLiteral("kills")).toInt(-1);

    if (!win.isEmpty() && win != QLatin1String("none") && win != prevWin) {
        m_dotaDead = false;
        releaseLight(QStringLiteral("dota.death"));
        playLight(QStringLiteral("dota.win"), ReactiveLighting::Round, QStringLiteral("победа"));
        const bool won = m_team.contains(win, Qt::CaseInsensitive)
            || (win == QLatin1String("radiant") && m_team.contains(QLatin1String("2")))
            || (win == QLatin1String("dire") && m_team.contains(QLatin1String("3")));
        emitSnapshot(won ? QStringLiteral("match_win") : QStringLiteral("match_loss"), {});
        return;
    }

    if (kills >= 0 && prevKills >= 0 && kills > prevKills) {
        emitSnapshot(QStringLiteral("kill"), {});
    }

    if (!alive && prevAlive) {
        m_dotaDead = true;
        playLight(QStringLiteral("dota.death"), ReactiveLighting::Event, QStringLiteral("смерть"));
        emitSnapshot(QStringLiteral("death"), {});
        return;
    }
    if (alive && m_dotaDead) {
        m_dotaDead = false;
        releaseLight(QStringLiteral("dota.death"));
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_inMatch && now - m_lastHeartbeatMs >= 4000) {
        m_lastHeartbeatMs = now;
        emitSnapshot(QStringLiteral("heartbeat"), {});
    }
}

bool ValveGsi::writeConfigs()
{
    bool wrote = false;
    for (bool dota : {false, true}) {
        const QString body = cfgBody(dota);
        for (const QString &dir : cfgDirs(dota)) {
            const QString path = dir + QLatin1Char('/') + QLatin1String(kCfgName);
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                qWarning() << "[GSI] cannot write" << path << f.errorString();
                continue;
            }
            f.write(body.toUtf8());
            wrote = true;
            qWarning() << "[GSI] wrote" << path;
        }
    }
    if (!wrote)
        qWarning() << "[GSI] cfg folder not found (CS2/Dota) — Chroma/GameSense всё равно работают";
    return wrote;
}

void ValveGsi::removeConfigs()
{
    for (bool dota : {false, true}) {
        for (const QString &dir : cfgDirs(dota)) {
            const QString path = dir + QLatin1Char('/') + QLatin1String(kCfgName);
            if (QFile::exists(path) && !QFile::remove(path))
                qWarning() << "[GSI] cannot remove" << path;
        }
    }
}

QStringList ValveGsi::cfgDirs(bool dota) const
{
    QStringList dirs;
    PathResolver *paths = PathResolver::instance();
    const QString steam = paths ? paths->steamPath() : QString();
    const QString games = paths ? paths->gamesPath() : QString();
    const QStringList roots = { steam, games,
        QStringLiteral("D:/Steam"), QStringLiteral("D:/Games") };
    QStringList tails;
    if (dota) {
        tails = {
            QStringLiteral("/steamapps/common/dota 2 beta/game/dota/cfg"),
            QStringLiteral("/steamapps/common/dota 2 beta/dota/cfg"),
            QStringLiteral("/dota 2 beta/game/dota/cfg"),
            QStringLiteral("/dota 2 beta/dota/cfg"),
        };
    } else {
        tails = {
            QStringLiteral("/steamapps/common/Counter-Strike Global Offensive/game/csgo/cfg"),
            QStringLiteral("/steamapps/common/Counter-Strike Global Offensive/csgo/cfg"),
            QStringLiteral("/Counter-Strike Global Offensive/game/csgo/cfg"),
            QStringLiteral("/Counter-Strike Global Offensive/csgo/cfg"),
            QStringLiteral("/csgo/cfg"),
        };
    }
    for (const QString &root : roots) {
        if (root.trimmed().isEmpty())
            continue;
        const QString r = QDir::fromNativeSeparators(root)
                              .replace(QRegularExpression(QStringLiteral("/+$")), QString());
        for (const QString &tail : tails) {
            const QString dir = r + tail;
            if (QDir(dir).exists())
                dirs.append(dir);
        }
    }
    dirs.removeDuplicates();
    return dirs;
}

QString ValveGsi::cfgBody(bool dota) const
{
    const QString data = dota
        ? QStringLiteral(
            "\t\t\"provider\"\t\t\"1\"\n"
            "\t\t\"map\"\t\t\t\"1\"\n"
            "\t\t\"player\"\t\t\"1\"\n"
            "\t\t\"hero\"\t\t\t\"1\"\n"
            "\t\t\"abilities\"\t\t\"1\"\n")
        : QStringLiteral(
            "\t\t\"provider\"\t\t\"1\"\n"
            "\t\t\"map\"\t\t\t\"1\"\n"
            "\t\t\"round\"\t\t\t\"1\"\n"
            "\t\t\"player_id\"\t\t\"1\"\n"
            "\t\t\"player_state\"\t\t\"1\"\n"
            "\t\t\"player_weapons\"\t\"1\"\n"
            "\t\t\"player_match_stats\"\t\"1\"\n"
            "\t\t\"player_position\"\t\"1\"\n"
            "\t\t\"allplayers_id\"\t\"1\"\n");
    return QStringLiteral(
        "\"ReactorClubLights\"\n"
        "{\n"
        "\t\"uri\"\t\t\"http://127.0.0.1:%1/\"\n"
        "\t\"timeout\"\t\"5.0\"\n"
        "\t\"buffer\"\t\"0.05\"\n"
        "\t\"throttle\"\t\"0.1\"\n"
        "\t\"heartbeat\"\t\"5.0\"\n"
        "\t\"auth\"\n"
        "\t{\n"
        "\t\t\"token\"\t\t\"%2\"\n"
        "\t}\n"
        "\t\"data\"\n"
        "\t{\n"
        "%3"
        "\t}\n"
        "}\n"
    ).arg(kPort).arg(QLatin1String(kToken)).arg(data);
}
