#include <utility>
#include "thymiodevicesmodel.h"
#include "thymiodevicemanagerclient.h"
#include "thymionode.h"

namespace mobsya {

ThymioDevicesModel::ThymioDevicesModel(const ThymioDeviceManagerClient& manager, QObject* parent)
    : QAbstractListModel(parent), m_manager(manager) {

    connect(&manager, &ThymioDeviceManagerClient::nodeAdded, this, &ThymioDevicesModel::updateModel);
    connect(&manager, &ThymioDeviceManagerClient::nodeRemoved, this, &ThymioDevicesModel::updateModel);
}
int ThymioDevicesModel::rowCount(const QModelIndex&) const {
    return m_manager.m_nodes.size();
}

QVariant ThymioDevicesModel::data(const QModelIndex& index, int role) const {
    auto idx = index.row();
    if(idx >= m_manager.m_nodes.size())
        return {};
    auto item = (m_manager.m_nodes.begin() + idx).value();
    switch(role) {
        case Qt::DisplayRole: return item->name();
        case StatusRole: return QVariant::fromValue(item->status());
        case NodeIdRole: return item->uuid().toString();
    }
    return {};
}

QHash<int, QByteArray> ThymioDevicesModel::roleNames() const {
    auto roles = QAbstractListModel::roleNames();
    roles.insert(StatusRole, "status");
    roles.insert(Qt::DisplayRole, "name");
    roles.insert(NodeIdRole, "id");
    return roles;
}

bool ThymioDevicesModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    auto idx = index.row();
    if(idx >= m_manager.m_nodes.size())
        return false;
    auto item = (m_manager.m_nodes.begin() + idx).value();
    switch(role) {
        case Qt::DisplayRole:
            if(!value.canConvert<QString>())
                return false;
            item->setName(value.toString());
            return true;
    }
    return false;
}

Qt::ItemFlags ThymioDevicesModel::flags(const QModelIndex& index) const {
    return QAbstractItemModel::flags(index) | Qt::ItemIsEditable;
}

void ThymioDevicesModel::updateModel() {
    layoutAboutToBeChanged();
    layoutChanged();
}

}  // namespace mobsya