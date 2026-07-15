import QtQuick
import QtQuick.Window

// Отдельное topmost-окно: вспышка Steam-логина остаётся ПОД заставкой
Window {
    id: overlay
    title: "REACTOR"
    color: "#020202"
    visible: false
    // Не привязываем к главному окну шелла — иначе заставка оказывается под Dashboard
    transientParent: null
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    visibility: Window.Hidden

    property alias running: spinLoop.running
    property string statusText: statusModel[statusIndex]
    property int statusIndex: 0
    readonly property var statusModel: [
        "ПОДГОТОВКА STEAM…",
        "ПРОВЕРКА СЕССИИ…",
        "ЗАПУСК ИГРЫ…"
    ]

    onVisibleChanged: {
        if (visible) {
            width = Screen.width
            height = Screen.height
            x = Screen.virtualX
            y = Screen.virtualY
            visibility = Window.FullScreen
            statusIndex = 0
            appear.restart()
            statusTicker.restart()
            raiseTimer.start()
            Qt.callLater(function() { overlay.raise() })
        } else {
            visibility = Window.Hidden
            statusTicker.stop()
            raiseTimer.stop()
            spinLoop.stop()
            counterSpin.stop()
        }
    }

    onRunningChanged: {
        if (running) {
            spinLoop.start()
            counterSpin.start()
        } else {
            spinLoop.stop()
            counterSpin.stop()
        }
    }

    // Держим поверх Steam, даже если он пытается вылезти
    Timer {
        id: raiseTimer
        interval: 150
        repeat: true
        onTriggered: overlay.raise()
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#06140a" }
            GradientStop { position: 0.55; color: "#020202" }
            GradientStop { position: 1.0; color: "#010101" }
        }
    }

    Rectangle {
        id: glowCore
        width: 520
        height: 520
        radius: 260
        anchors.centerIn: parent
        color: "#22c55e"
        opacity: 0.06
        scale: 0.92

        SequentialAnimation on opacity {
            running: overlay.visible
            loops: Animation.Infinite
            NumberAnimation { to: 0.11; duration: 1600; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0.05; duration: 1600; easing.type: Easing.InOutSine }
        }
        SequentialAnimation on scale {
            running: overlay.visible
            loops: Animation.Infinite
            NumberAnimation { to: 1.04; duration: 2200; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0.92; duration: 2200; easing.type: Easing.InOutSine }
        }
    }

    Item {
        id: stage
        anchors.centerIn: parent
        width: 220
        height: 220
        opacity: 0
        scale: 0.86

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.color: "#143322"
            border.width: 2
        }

        Rectangle {
            anchors.centerIn: parent
            width: 160
            height: 160
            radius: 80
            color: "transparent"
            border.color: "#1a4d29"
            border.width: 1
            opacity: 0.7
        }

        Canvas {
            id: arcPrimary
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                var cx = width / 2
                var cy = height / 2
                var r = Math.min(cx, cy) - 8
                ctx.reset()
                ctx.lineWidth = 4
                ctx.lineCap = "round"
                ctx.strokeStyle = "#22c55e"
                ctx.beginPath()
                ctx.arc(cx, cy, r, -Math.PI * 0.55, Math.PI * 0.85)
                ctx.stroke()
                ctx.strokeStyle = "#4ade80"
                ctx.lineWidth = 2
                ctx.beginPath()
                ctx.arc(cx, cy, r, -Math.PI * 0.55, -Math.PI * 0.15)
                ctx.stroke()
            }
            Component.onCompleted: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            NumberAnimation on rotation {
                id: spinLoop
                from: 0; to: 360
                duration: 1400
                loops: Animation.Infinite
                running: false
                easing.type: Easing.Linear
            }
        }

        Canvas {
            id: arcCounter
            anchors.centerIn: parent
            width: 150
            height: 150
            onPaint: {
                var ctx = getContext("2d")
                var cx = width / 2
                var cy = height / 2
                var r = Math.min(cx, cy) - 6
                ctx.reset()
                ctx.lineWidth = 2.5
                ctx.lineCap = "round"
                ctx.strokeStyle = "#86efac"
                ctx.globalAlpha = 0.55
                ctx.beginPath()
                ctx.arc(cx, cy, r, Math.PI * 0.2, Math.PI * 1.1)
                ctx.stroke()
            }
            Component.onCompleted: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            NumberAnimation on rotation {
                id: counterSpin
                from: 360; to: 0
                duration: 2200
                loops: Animation.Infinite
                running: false
                easing.type: Easing.Linear
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 18
            height: 18
            radius: 9
            color: "#22c55e"
            opacity: 0.95
            Rectangle {
                anchors.centerIn: parent
                width: 34
                height: 34
                radius: 17
                color: "transparent"
                border.color: "#22c55e"
                border.width: 1
                opacity: 0.35
            }
        }

        Repeater {
            model: 6
            Rectangle {
                required property int index
                width: 6
                height: 6
                radius: 3
                color: "#4ade80"
                opacity: 0.35 + (index % 3) * 0.2
                property real angle: index * 60
                x: stage.width / 2 + Math.cos(angle * Math.PI / 180) * 98 - width / 2
                y: stage.height / 2 + Math.sin(angle * Math.PI / 180) * 98 - height / 2
                NumberAnimation on angle {
                    from: index * 60
                    to: index * 60 + 360
                    duration: 3200 + index * 120
                    loops: Animation.Infinite
                    running: overlay.visible
                    easing.type: Easing.Linear
                }
            }
        }
    }

    Column {
        id: titleBlock
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: stage.bottom
        anchors.topMargin: 36
        spacing: 10
        opacity: 0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "REACTOR"
            color: "#e7ffe9"
            font.pixelSize: 28
            font.bold: true
            font.letterSpacing: 8
            style: Text.Outline
            styleColor: "#14532d"
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.statusText
            color: "#22c55e"
            font.pixelSize: 14
            font.bold: true
            font.letterSpacing: 3
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Repeater {
                model: 3
                Rectangle {
                    required property int index
                    width: 8
                    height: 8
                    radius: 4
                    color: index === overlay.statusIndex ? "#22c55e" : "#1a4d29"
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }
    }

    Timer {
        id: statusTicker
        interval: 2200
        repeat: true
        onTriggered: overlay.statusIndex = (overlay.statusIndex + 1) % overlay.statusModel.length
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: stage; property: "opacity"; from: 0; to: 1; duration: 420; easing.type: Easing.OutCubic }
        NumberAnimation { target: stage; property: "scale"; from: 0.86; to: 1; duration: 520; easing.type: Easing.OutBack }
        NumberAnimation { target: titleBlock; property: "opacity"; from: 0; to: 1; duration: 500; easing.type: Easing.OutCubic }
    }
}
