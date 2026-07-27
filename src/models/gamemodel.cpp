#include "gamemodel.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <algorithm>

GameModel::GameModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GameModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    // GridView sees display list after first-row cards (gridSkip)
    return std::max(0, static_cast<int>(m_displayGames.size()) - m_gridSkip);
}

QVariant GameModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    const int absolute = index.row() + m_gridSkip;
    if (absolute < 0 || absolute >= static_cast<int>(m_displayGames.size()))
        return QVariant();

    const GameItem &item = m_displayGames.at(static_cast<size_t>(absolute));

    switch (role) {
    case IdRole:       return item.id;
    case TitleRole:    return item.title;
    case PosterRole:   return item.poster;
    case ExePathRole:  return item.exePath;
    case ArgsRole:     return item.args;
    case CategoryRole: return item.category;
    case PlatformRole: return item.platform;
    default:           return QVariant();
    }
}

QHash<int, QByteArray> GameModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]       = "gameId";
    roles[TitleRole]    = "title";
    roles[PosterRole]   = "poster";
    roles[ExePathRole]  = "exePath";
    roles[ArgsRole]     = "args";
    roles[CategoryRole] = "category";
    roles[PlatformRole] = "platform";
    return roles;
}

void GameModel::setGames(const std::vector<GameItem> &games)
{
    m_allGames = games;
    applyFilters();
}

void GameModel::setFeaturedGames(const std::vector<GameItem> &games)
{
    m_featuredGames = games;
    if (static_cast<int>(m_featuredGames.size()) > kMaxFeatured)
        m_featuredGames.resize(static_cast<size_t>(kMaxFeatured));
    applyFilters();
}

void GameModel::setFilter(const QString &filter)
{
    m_currentFilter = filter.isEmpty() ? QStringLiteral("ВСЕ ИГРЫ") : filter;
    applyFilters();
}

void GameModel::setSearchQuery(const QString &query)
{
    m_searchQuery = query.trimmed();
    applyFilters();
}

QVariantMap GameModel::get(int index) const
{
    QVariantMap map;
    if (index < 0 || index >= static_cast<int>(m_displayGames.size()))
        return map;

    const GameItem &item = m_displayGames.at(static_cast<size_t>(index));
    map.insert(QStringLiteral("gameId"), item.id);
    map.insert(QStringLiteral("title"), item.title);
    map.insert(QStringLiteral("poster"), item.poster);
    map.insert(QStringLiteral("exePath"), item.exePath);
    map.insert(QStringLiteral("args"), item.args);
    map.insert(QStringLiteral("category"), item.category);
    map.insert(QStringLiteral("platform"), item.platform);
    return map;
}

void GameModel::setGridSkip(int skip)
{
    const int clamped = std::max(0, std::min(skip, static_cast<int>(m_displayGames.size())));
    if (m_gridSkip == clamped)
        return;

    beginResetModel();
    m_gridSkip = clamped;
    endResetModel();
    emit gridSkipChanged();
}

void GameModel::applyFilters()
{
    beginResetModel();

    m_displayGames.clear();

    const QString &filter = m_currentFilter;
    const bool showAll = filter == QStringLiteral("ВСЕ ИГРЫ") || filter.isEmpty();
    const bool riotTab = filter.compare(QStringLiteral("RIOT"), Qt::CaseInsensitive) == 0;
    const bool hasSearch = !m_searchQuery.isEmpty();

    // Catalog lookup — featured title/poster always from main games list by id.
    QHash<int, GameItem> byId;
    byId.reserve(static_cast<int>(m_allGames.size()));
    for (const auto &g : m_allGames) {
        if (g.id > 0)
            byId.insert(g.id, g);
    }

    const bool showFeatured = showAll && !hasSearch && !m_featuredGames.empty();
    QSet<int> featuredIds;
    int newFeaturedCount = 0;

    if (showFeatured) {
        for (const auto &fg : m_featuredGames) {
            if (fg.id <= 0 || featuredIds.contains(fg.id))
                continue;
            const auto it = byId.constFind(fg.id);
            if (it == byId.cend())
                continue;
            m_displayGames.push_back(it.value()); // canonical catalog fields
            featuredIds.insert(fg.id);
            ++newFeaturedCount;
            if (newFeaturedCount >= kMaxFeatured)
                break;
        }
    }

    for (const auto &game : m_allGames) {
        if (featuredIds.contains(game.id))
            continue;

        bool match = showAll;

        if (!showAll) {
            match = game.platform.contains(filter, Qt::CaseInsensitive)
                || game.category.contains(filter, Qt::CaseInsensitive);

            if (!match && riotTab) {
                const QString hay = (game.platform + QLatin1Char(' ')
                                     + game.title + QLatin1Char(' ')
                                     + game.exePath + QLatin1Char(' ')
                                     + game.args).toLower();
                match = hay.contains(QLatin1String("riot"))
                    || hay.contains(QLatin1String("valorant"))
                    || hay.contains(QLatin1String("league of legends"))
                    || hay.contains(QLatin1String("league_of_legends"));
            }
        }

        if (match && hasSearch
            && !game.title.contains(m_searchQuery, Qt::CaseInsensitive)) {
            match = false;
        }

        if (match)
            m_displayGames.push_back(game);
    }

    m_gridSkip = std::max(0, std::min(m_gridSkip, static_cast<int>(m_displayGames.size())));

    const bool featuredCountDirty = (m_featuredCount != newFeaturedCount);
    m_featuredCount = newFeaturedCount;

    endResetModel();
    emit countChanged();
    if (featuredCountDirty)
        emit featuredCountChanged();
}
