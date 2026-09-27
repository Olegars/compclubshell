#include "storemodel.h"

StoreModel::StoreModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int StoreModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_displayProducts.size());
}

QVariant StoreModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_displayProducts.size()))
        return QVariant();

    const StoreItem &item = m_displayProducts.at(index.row());

    switch (role) {
    case IdRole:       return item.id;
    case NameRole:     return item.name;
    case PriceRole:    return item.price;
    case CategoryRole: return item.category;
    case ImageRole:    return item.image;
    case StockRole:    return item.stock;
    default:           return QVariant();
    }
}

QHash<int, QByteArray> StoreModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]       = "id";
    roles[NameRole]     = "name";
    roles[PriceRole]    = "price";
    roles[CategoryRole] = "category";
    roles[ImageRole]    = "image";
    roles[StockRole]    = "stock";
    return roles;
}

void StoreModel::setProducts(const std::vector<StoreItem> &products)
{
    beginResetModel();
    m_allProducts = products;
    m_displayProducts = products;
    endResetModel();
}

void StoreModel::setFilter(const QString &filter)
{
    const QString target = filter.trimmed().toLower();
    qDebug() << "[STORE] Фильтр запрошен:" << target;

    auto matches = [](const QString &category, const QString &needle) {
        const QString cat = category.trimmed().toLower();
        if (cat.isEmpty() || needle.isEmpty())
            return false;
        if (cat.contains(needle) || needle.contains(cat))
            return true;
        const bool drinkNeedle = needle.contains(QStringLiteral("drink"))
                || needle.contains(QStringLiteral("напит"));
        const bool snackNeedle = needle.contains(QStringLiteral("food"))
                || needle.contains(QStringLiteral("snack"))
                || needle.contains(QStringLiteral("снэк"))
                || needle.contains(QStringLiteral("снек"));
        const bool mealNeedle = needle.contains(QStringLiteral("еда"))
                || needle.contains(QStringLiteral("meal"));
        if (drinkNeedle && (cat.contains(QStringLiteral("напит")) || cat.contains(QStringLiteral("drink"))))
            return true;
        if (snackNeedle && (cat.contains(QStringLiteral("снэк")) || cat.contains(QStringLiteral("снек"))
                            || cat.contains(QStringLiteral("snack"))))
            return true;
        if (mealNeedle && (cat.contains(QStringLiteral("еда")) || cat.contains(QStringLiteral("meal"))))
            return true;
        return false;
    };

    beginResetModel();
    if (target.isEmpty() || target == QStringLiteral("все")) {
        m_displayProducts = m_allProducts;
        qDebug() << "[STORE] Сброс. Всего товаров:" << m_displayProducts.size();
    } else {
        m_displayProducts.clear();
        for (const auto &item : m_allProducts) {
            if (matches(item.category, target))
                m_displayProducts.push_back(item);
        }
        qDebug() << "[STORE] Найдено после фильтрации:" << m_displayProducts.size();
    }
    endResetModel();
}