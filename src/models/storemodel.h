#pragma once

#include <QAbstractListModel>
#include <vector>
#include <QString>

// Структура одного товара из Laravel
struct StoreItem {
    int id;
    QString name;
    double price;
    QString category;
    QString image;
    int stock;
};

class StoreModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum StoreRoles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PriceRole,
        CategoryRole,
        ImageRole,
        StockRole
    };

    explicit StoreModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProducts(const std::vector<StoreItem> &products);
    Q_INVOKABLE void setFilter(const QString &filter); // Для переключения категорий

private:
    std::vector<StoreItem> m_allProducts;
    std::vector<StoreItem> m_displayProducts;
};