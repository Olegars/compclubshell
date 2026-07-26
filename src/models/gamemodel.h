#pragma once

#include <QAbstractListModel>
#include <vector>
#include <QString>

// Структура, описывающая одну игру (полностью совпадает с ответом Laravel)
struct GameItem {
    int id;
    QString title;
    QString poster;
    QString exePath;
    QString args;
    QString category;
    QString platform;
};

class GameModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // Роли нужны для связи полей C++ с QML
    enum GameRoles {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        PosterRole,
        ExePathRole,
        ArgsRole,
        CategoryRole,
        PlatformRole
    };

    explicit GameModel(QObject *parent = nullptr);

    // Обязательные методы QAbstractListModel
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Метод для загрузки данных из NetworkManager
    void setGames(const std::vector<GameItem> &games);

    // Метод для вызова из QML (фильтрация по категориям/платформам)
    Q_INVOKABLE void setFilter(const QString &filter);

    // Поиск по названию игры (case-insensitive contains); пустая строка = без поиска
    Q_INVOKABLE void setSearchQuery(const QString &query);

    int count() const { return static_cast<int>(m_displayGames.size()); }

signals:
    void countChanged();

private:
    void applyFilters();

    std::vector<GameItem> m_allGames;     // Кэш всех скачанных игр
    std::vector<GameItem> m_displayGames; // Игры, которые сейчас отображаются на экране
    QString m_currentFilter = QStringLiteral("ВСЕ ИГРЫ");
    QString m_searchQuery;
};
