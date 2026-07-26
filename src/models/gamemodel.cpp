#include "gamemodel.h"

GameModel::GameModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GameModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    // QML должен знать только о тех играх, которые прошли фильтр
    return static_cast<int>(m_displayGames.size());
}

QVariant GameModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_displayGames.size())
        return QVariant();

    const GameItem &item = m_displayGames.at(index.row());

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
    roles[IdRole]       = "id";
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

void GameModel::applyFilters()
{
    beginResetModel();

    m_displayGames.clear();

    const QString &filter = m_currentFilter;
    const bool showAll = filter == QStringLiteral("ВСЕ ИГРЫ") || filter.isEmpty();
    const bool riotTab = filter.compare(QStringLiteral("RIOT"), Qt::CaseInsensitive) == 0;
    const bool hasSearch = !m_searchQuery.isEmpty();

    for (const auto &game : m_allGames) {
        bool match = showAll;

        if (!showAll) {
            // Совпадение по платформе (Steam) или категории (Утилиты)
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

    endResetModel();
    emit countChanged();
}
