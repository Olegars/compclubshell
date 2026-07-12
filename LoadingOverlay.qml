import QtQuick

Rectangle {
    id: overlay
    anchors.fill: parent
    color: "#020202"
    visible: false
    z: 9999

    property alias running: rotationAnimMin.running

    Item {
        anchors.centerIn: parent
        width: 150; height: 150

        // Контур часов
        Rectangle {
            anchors.fill: parent
            radius: 75
            color: "transparent"
            border.color: "#1a4d2c"
            border.width: 4
        }

        // Минутная стрелка (быстрая)
        Rectangle {
            id: handMin
            width: 4; height: 55
            color: "#22c55e"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 20
            transformOrigin: Item.Bottom
        }

        // Часовая стрелка (медленная)
        Rectangle {
            id: handHour
            width: 4; height: 35 // Короче и толще
            color: "#22c55e"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 40
            transformOrigin: Item.Bottom
        }

        // Анимация минутной (базовая)
        RotationAnimation {
            id: rotationAnimMin
            target: handMin
            from: 0; to: 360
            duration: 1500
            loops: Animation.Infinite
            running: false
        }

        // Анимация часовой (в 3 раза медленнее)
        RotationAnimation {
            id: rotationAnimHour
            target: handHour
            from: 0; to: 360
            duration: 4500 // 1500 * 3
            loops: Animation.Infinite
            running: rotationAnimMin.running // Синхронизируем старт
        }
    }

    Text {
        anchors.top: parent.verticalCenter
        anchors.topMargin: 100
        anchors.horizontalCenter: parent.horizontalCenter
        text: "ЗАГРУЗКА ИГРЫ..."
        color: "#22c55e"
        font.pixelSize: 18
        font.bold: true
        font.letterSpacing: 3
    }
}