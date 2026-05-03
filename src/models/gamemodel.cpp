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
    // beginResetModel заставляет QML полностью перерисовать GridView
    beginResetModel();
    m_allGames = games;
    m_displayGames = games; // По умолчанию показываем всё
    endResetModel();
}

void GameModel::setFilter(const QString &filter)
{
    beginResetModel();

    if (filter == "ВСЕ ИГРЫ" || filter.isEmpty()) {
        m_displayGames = m_allGames;
    } else {
        m_displayGames.clear();

        // Пробегаемся по полному списку и ищем совпадения
        for (const auto &game : m_allGames) {
            // Ищем совпадение либо по платформе (напр. "Steam"), либо по категории (напр. "Утилиты")
            // Используем Qt::CaseInsensitive, чтобы не было проблем с регистром (Steam == STEAM)
            if (game.platform.contains(filter, Qt::CaseInsensitive) ||
                game.category.contains(filter, Qt::CaseInsensitive)) {

                m_displayGames.push_back(game);
            }
        }
    }

    endResetModel();
}