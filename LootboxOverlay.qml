import QtQuick
import QtQuick.Controls
import sector0451

Item {
    id: loot
    anchors.fill: parent
    visible: overlayOn
    z: 220

    property bool overlayOn: false
    property var box: ({})
    property bool opening: false
    property bool revealed: false
    property int spinIndex: 0
    readonly property var icons: ["₽", "🥤", "🏷️"]
    readonly property var iconLabels: ["Бонус", "Напиток", "Store"]

    function present(payload) {
        box = payload || {}
        overlayOn = true
        opening = false
        revealed = false
        spinIndex = 0
        idleShake.restart()
    }

    function dismiss() {
        overlayOn = false
        opening = false
        idleShake.stop()
        spinTimer.stop()
    }

    function openCase() {
        if (opening || revealed)
            return
        var id = Number((box && box.id) ? box.id : 0)
        if (id < 1)
            return
        opening = true
        idleShake.stop()
        spinTimer.start()
        if (typeof NetworkManager !== "undefined")
            NetworkManager.openLootbox(id)
    }

    function applyOpened(payload) {
        if (payload && payload.id)
            box = payload
        spinTimer.stop()
        opening = false
        revealed = true
        var t = String((box.reward_type || (box.reward && box.reward.type) || ""))
        if (t === "drink")
            spinIndex = 1
        else if (t === "store_promo")
            spinIndex = 2
        else
            spinIndex = 0
    }

    Rectangle {
        anchors.fill: parent
        color: "#cc020202"
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (loot.revealed)
                    loot.dismiss()
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 18
        width: Math.min(parent.width - 48, 460)

        Text {
            width: parent.width
            text: "LUCKY SEAT"
            color: Theme.shop
            font.pixelSize: Theme.fontHeading
            font.bold: true
            font.letterSpacing: 4
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            width: parent.width
            text: String((loot.box && loot.box.subtitle) ? loot.box.subtitle : "Кейс выпал на этом месте")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
        }

        Item {
            id: crateWrap
            width: 220
            height: 180
            anchors.horizontalCenter: parent.horizontalCenter
            transform: Rotation {
                id: crateTilt
                origin.x: crateWrap.width / 2
                origin.y: crateWrap.height / 2
                angle: 0
            }

            Rectangle {
                anchors.fill: parent
                radius: 18
                color: "#1a1204"
                border.width: 2
                border.color: Theme.shop
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#3a2a08" }
                    GradientStop { position: 1.0; color: "#120e04" }
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 18
                    width: parent.width - 36
                    height: 8
                    radius: 4
                    color: Theme.shop
                    opacity: 0.85
                }

                Text {
                    anchors.centerIn: parent
                    text: loot.opening || loot.revealed ? loot.icons[loot.spinIndex] : "✦"
                    color: "white"
                    font.pixelSize: loot.revealed ? 64 : 48
                }
            }
        }

        Text {
            width: parent.width
            visible: loot.opening && !loot.revealed
            text: loot.iconLabels[loot.spinIndex]
            color: Theme.shop
            font.pixelSize: Theme.fontTitle
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            width: parent.width
            visible: loot.revealed
            text: String((loot.box && loot.box.reward && loot.box.reward.label)
                         ? loot.box.reward.label
                         : "Награда внутри")
            color: "white"
            font.pixelSize: Theme.fontTitle
            font.bold: true
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            width: parent.width
            visible: loot.revealed && loot.box && loot.box.reward && loot.box.reward.code
            text: loot.box && loot.box.reward ? String(loot.box.reward.code || "") : ""
            color: Theme.shop
            font.pixelSize: Theme.fontHeading
            font.bold: true
            font.letterSpacing: 3
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            width: parent.width
            visible: loot.revealed && loot.box && loot.box.reward && loot.box.reward.code
            text: "Покажи код в REACTOR Store — 10% на мышь, клавиатуру, гарнитуру"
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        Rectangle {
            visible: !loot.revealed
            width: parent.width
            height: 52
            radius: 8
            color: loot.opening ? "#333" : Theme.shop
            Text {
                anchors.centerIn: parent
                text: loot.opening ? "ОТКРЫВАЕМ…" : "ОТКРЫТЬ КЕЙС"
                color: loot.opening ? "#aaa" : "#111"
                font.pixelSize: 16
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                enabled: !loot.opening
                cursorShape: Qt.PointingHandCursor
                onClicked: loot.openCase()
            }
        }

        Text {
            width: parent.width
            visible: loot.revealed
            text: "Нажми, чтобы закрыть"
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            width: parent.width
            visible: !loot.revealed && !loot.opening
            text: "Можно закрыть и открыть позже — кнопка КЕЙС"
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            MouseArea {
                anchors.fill: parent
                onClicked: loot.dismiss()
            }
        }
    }

    Timer {
        id: idleShake
        interval: 900
        repeat: true
        running: loot.overlayOn && !loot.opening && !loot.revealed
        onTriggered: crateShake.start()
    }

    SequentialAnimation {
        id: crateShake
        NumberAnimation { target: crateTilt; property: "angle"; to: -6; duration: 70 }
        NumberAnimation { target: crateTilt; property: "angle"; to: 6; duration: 90 }
        NumberAnimation { target: crateTilt; property: "angle"; to: 0; duration: 70 }
    }

    Timer {
        id: spinTimer
        interval: 90
        repeat: true
        onTriggered: loot.spinIndex = (loot.spinIndex + 1) % loot.icons.length
    }
}
