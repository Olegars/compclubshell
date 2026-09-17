import QtQuick
import QtQuick.Controls
import sector0451

Item {
    id: calm
    anchors.fill: parent
    visible: overlayOn
    z: 230

    property bool overlayOn: false
    property var payload: ({})
    property bool ordering: false
    property string orderHint: ""

    readonly property var drinks: {
        var list = (payload && payload.drinks) ? payload.drinks : []
        return (list && list.length !== undefined) ? list : []
    }

    function present(data) {
        payload = data || {}
        overlayOn = true
        ordering = false
        orderHint = ""
    }

    function dismiss() {
        overlayOn = false
        ordering = false
        orderHint = ""
    }

    function orderDrink(productId) {
        if (ordering)
            return
        var id = Number(productId || 0)
        if (id < 1)
            return
        var baseUrl = (typeof NetworkManager !== "undefined") ? String(NetworkManager.serverUrl || "") : ""
        var termId = (typeof NetworkManager !== "undefined") ? Number(NetworkManager.computerId || 0) : 0
        if (!baseUrl || termId < 1) {
            orderHint = "Терминал не готов"
            return
        }
        ordering = true
        orderHint = ""
        var xhr = new XMLHttpRequest()
        xhr.open("POST", baseUrl + "/api/shell/store/checkout")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.setRequestHeader("Accept", "application/json")
        xhr.onreadystatechange = function () {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return
            ordering = false
            try {
                var res = xhr.responseText ? JSON.parse(xhr.responseText) : {}
                if (xhr.status >= 200 && xhr.status < 300 && res.status === "success") {
                    orderHint = "Заказ принят — сейчас принесут"
                    if (typeof NetworkManager !== "undefined")
                        NetworkManager.checkOrderStatus(termId, Number(res.order_id || 0))
                    Qt.callLater(function () { calm.dismiss() })
                } else {
                    orderHint = (res && res.message) ? String(res.message) : "Не удалось оформить"
                }
            } catch (e) {
                orderHint = "Не удалось оформить заказ"
            }
        }
        xhr.send(JSON.stringify({
            "terminal_id": termId,
            "items": [{ "product_id": id, "qty": 1 }]
        }))
    }

    Rectangle {
        anchors.fill: parent
        color: "#cc020202"
        MouseArea {
            anchors.fill: parent
            onClicked: calm.dismiss()
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        height: innerCol.implicitHeight + 56
        radius: Theme.radiusLg
        color: "#120e08"
        border.width: 1
        border.color: Theme.shop

        Column {
            id: innerCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 28
            spacing: 14

            Text {
                width: parent.width
                text: String((calm.payload && calm.payload.title) ? calm.payload.title : "Всё в порядке")
                color: Theme.shop
                font.pixelSize: Theme.fontHeading
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: String((calm.payload && calm.payload.message)
                             ? calm.payload.message
                             : "Похоже, партия идёт тяжело. Стол тут ни при чём — сделайте паузу, можем принести напиток.")
                color: Theme.textBody
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: calm.drinks
                delegate: Button {
                    width: parent.width
                    height: 44
                    enabled: !calm.ordering
                    text: {
                        var n = String(modelData.name || "Напиток")
                        var p = Number(modelData.price || 0)
                        return n + (p > 0 ? ("  ·  " + p + " ₽") : "")
                    }
                    contentItem: Text {
                        text: parent.text
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontLabel
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: Theme.radiusSm
                        color: parent.down ? Theme.accentPressed : Theme.accentDeep
                        border.color: Theme.accent
                        border.width: 1
                    }
                    onClicked: calm.orderDrink(modelData.id)
                }
            }

            Text {
                width: parent.width
                visible: orderHint.length > 0
                text: orderHint
                color: Theme.warning
                font.pixelSize: Theme.fontCaption
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Button {
                width: parent.width
                height: 40
                text: "Не сейчас"
                contentItem: Text {
                    text: parent.text
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radiusSm
                    color: "#1a1a1a"
                    border.color: "#333"
                    border.width: 1
                }
                onClicked: calm.dismiss()
            }
        }
    }
}
