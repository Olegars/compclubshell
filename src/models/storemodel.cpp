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

    // ТРАССИРОВКА: Логируем, что C++ отдает в QML при обращении к цене
    if (role == PriceRole) {
        qDebug() << "[CPP-MODEL-TRACE] Товар:" << item.name << "| Запрос цены. Значение double:" << item.price;
    }

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
    QString target = filter.trimmed();
    qDebug() << "[STORE] Фильтр запрошен:" << target;

    beginResetModel();
    if (target == "Все" || target.isEmpty()) {
        m_displayProducts = m_allProducts;
        qDebug() << "[STORE] Сброс. Всего товаров:" << m_displayProducts.size();
    } else {
        m_displayProducts.clear();
        QString lowerTarget = target.toLower();

        for (const auto &item : m_allProducts) {
            QString lowerCategory = item.category.trimmed().toLower();
            if (lowerCategory.contains(lowerTarget) || lowerTarget.contains(lowerCategory)) {
                m_displayProducts.push_back(item);
            }
        }
        qDebug() << "[STORE] Найдено после фильтрации:" << m_displayProducts.size();
    }
    endResetModel();
}