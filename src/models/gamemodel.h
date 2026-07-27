#pragma once

#include <QAbstractListModel>
#include <vector>
#include <QString>
#include <QVariantMap>

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
    Q_PROPERTY(int gridSkip READ gridSkip WRITE setGridSkip NOTIFY gridSkipChanged)
    Q_PROPERTY(int featuredCount READ featuredCount NOTIFY featuredCountChanged)

public:
    static constexpr int kMaxFeatured = 6;

    // Роли нужны для связи полей C++ с QML (gameId — not "id", QML reserved)
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

    /**
     * Featured IDs shown as the first N entries of the display list
     * («ВСЕ ИГРЫ», no search). Title/poster always come from the catalog.
     */
    void setFeaturedGames(const std::vector<GameItem> &games);

    // Метод для вызова из QML (фильтрация по категориям/платформам)
    Q_INVOKABLE void setFilter(const QString &filter);

    // Поиск по названию игры (case-insensitive contains); пустая строка = без поиска
    Q_INVOKABLE void setSearchQuery(const QString &query);

    /** Absolute display entry (ignores gridSkip) — for first-row cards. */
    Q_INVOKABLE QVariantMap get(int index) const;

    /** Hide first N display items from GridView (they sit in the first-row Row). */
    int gridSkip() const { return m_gridSkip; }
    void setGridSkip(int skip);

    /** Full filtered list size (first row + grid), ignoring gridSkip. */
    int count() const { return static_cast<int>(m_displayGames.size()); }

    /** How many leading display entries are featured (0 when strip hidden). */
    int featuredCount() const { return m_featuredCount; }

signals:
    void countChanged();
    void gridSkipChanged();
    void featuredCountChanged();

private:
    void applyFilters();

    std::vector<GameItem> m_allGames;      // Кэш всех скачанных игр
    std::vector<GameItem> m_featuredGames; // Featured IDs (fields overwritten from catalog)
    std::vector<GameItem> m_displayGames;  // Игры, которые сейчас отображаются на экране
    QString m_currentFilter = QStringLiteral("ВСЕ ИГРЫ");
    QString m_searchQuery;
    int m_gridSkip = 0;
    int m_featuredCount = 0;
};
