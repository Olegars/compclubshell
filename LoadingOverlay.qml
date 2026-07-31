import QtQuick
import sector0451

// Заставка внутри главного окна (z поверх Dashboard).
// RotationAnimator крутится на render thread — не замирает при занятости UI.
Item {
    id: overlay
    anchors.fill: parent
    visible: false
    z: 1000000

    property bool running: false
    property string platformName: "LAUNCHER"
    property string gameTitle: "ИГРЫ"
    property int statusIndex: 0

    readonly property string platformLabel: {
        var p = (platformName || "").toString().trim().toUpperCase()
        if (p === "EPIC" || p.indexOf("EPIC") >= 0)
            return "EPIC"
        if (p === "STEAM" || p.indexOf("STEAM") >= 0)
            return "STEAM"
        if (p === "EA" || p === "ORIGIN" || p.indexOf("EAAPP") >= 0 || p.indexOf("EA APP") >= 0)
            return "EA"
        if (p === "DIRECT" || p === "PC")
            return "LAUNCHER"
        if (p.length > 0)
            return p
        return "LAUNCHER"
    }

    readonly property string gameLabel: {
        var t = (gameTitle || "").toString().trim()
        if (t.length > 0)
            return t.toUpperCase()
        return "ИГРЫ"
    }

    readonly property var statusModel: [
        "ПОДГОТОВКА " + platformLabel + "…",
        "ПРОВЕРКА СЕССИИ…",
        "НАСТРАИВАЕМ " + gameLabel + "…"
    ]

    property string statusText: statusModel[statusIndex]

    onVisibleChanged: {
        if (visible) {
            statusIndex = 0
            appear.restart()
            statusTicker.restart()
        } else {
            statusTicker.stop()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
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
            border.color: Theme.accentBorder
            border.width: 2
        }

        Rectangle {
            anchors.centerIn: parent
            width: 160
            height: 160
            radius: 80
            color: "transparent"
            border.color: Theme.accentBorder
            border.width: 1
            opacity: 0.7
        }

        Item {
            id: outerArc
            anchors.fill: parent

            Canvas {
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    var cx = width / 2
                    var cy = height / 2
                    var r = Math.min(cx, cy) - 8
                    ctx.reset()
                    ctx.lineWidth = 4
                    ctx.lineCap = "round"
                    ctx.strokeStyle = Theme.accent
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, -Math.PI * 0.55, Math.PI * 0.85)
                    ctx.stroke()
                    ctx.strokeStyle = Theme.accentBright
                    ctx.lineWidth = 2
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, -Math.PI * 0.55, -Math.PI * 0.15)
                    ctx.stroke()
                }
                readonly property color arcColor: Theme.accent
                onArcColorChanged: requestPaint()
                Component.onCompleted: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
            }

            RotationAnimator {
                target: outerArc
                from: 0
                to: 360
                duration: 1400
                loops: Animation.Infinite
                running: overlay.visible && overlay.running
            }
        }

        Item {
            id: innerArc
            anchors.centerIn: parent
            width: 150
            height: 150

            Canvas {
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    var cx = width / 2
                    var cy = height / 2
                    var r = Math.min(cx, cy) - 6
                    ctx.reset()
                    ctx.lineWidth = 2.5
                    ctx.lineCap = "round"
                    ctx.strokeStyle = Theme.accentBright
                    ctx.globalAlpha = 0.55
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, Math.PI * 0.2, Math.PI * 1.1)
                    ctx.stroke()
                }
                readonly property color arcColor: Theme.accentBright
                onArcColorChanged: requestPaint()
                Component.onCompleted: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
            }

            RotationAnimator {
                target: innerArc
                from: 360
                to: 0
                duration: 2200
                loops: Animation.Infinite
                running: overlay.visible && overlay.running
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 18
            height: 18
            radius: 9
            color: Theme.accent
            Rectangle {
                anchors.centerIn: parent
                width: 34
                height: 34
                radius: 17
                color: "transparent"
                border.color: Theme.accent
                border.width: 1
                opacity: 0.35
            }
        }

        Item {
            id: orbit
            anchors.fill: parent

            Repeater {
                model: 6
                Rectangle {
                    required property int index
                    width: 6
                    height: 6
                    radius: 3
                    color: Theme.accentBright
                    opacity: 0.35 + (index % 3) * 0.2
                    property real ang: index * 60
                    x: stage.width / 2 + Math.cos(ang * Math.PI / 180) * 98 - width / 2
                    y: stage.height / 2 + Math.sin(ang * Math.PI / 180) * 98 - height / 2
                }
            }

            RotationAnimator {
                target: orbit
                from: 0
                to: 360
                duration: 3200
                loops: Animation.Infinite
                running: overlay.visible && overlay.running
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
            color: Theme.textBody
            font.pixelSize: Theme.fontHeading
            font.bold: true
            font.letterSpacing: 8
            style: Text.Outline
            styleColor: Theme.accentBorder
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.statusText
            color: Theme.accent
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
                    color: index === overlay.statusIndex ? Theme.accent : Theme.accentBorder
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

    // Modal (last sibling = on top): swallow mouse so clicks cannot fall through to Dashboard.
    MouseArea {
        anchors.fill: parent
        enabled: overlay.visible
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => { mouse.accepted = true }
        onReleased: (mouse) => { mouse.accepted = true }
        onClicked: (mouse) => { mouse.accepted = true }
        onDoubleClicked: (mouse) => { mouse.accepted = true }
        onWheel: (wheel) => { wheel.accepted = true }
    }
}
