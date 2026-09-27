import QtQuick
import QtQuick.Controls
import sector0451

Item {
    id: overlay
    anchors.fill: parent
    visible: incomingOn || victoryOn
    z: 230

    property bool incomingOn: false
    property bool victoryOn: false
    property var challenge: ({})
    property var result: ({})
    property int remain: 0

    function presentIncoming(payload) {
        challenge = payload || {}
        remain = Number(challenge.seconds_left || 60)
        incomingOn = true
        victoryOn = false
        tick.restart()
    }

    function presentVictory(payload) {
        result = payload || {}
        incomingOn = false
        victoryOn = true
        victoryHide.restart()
    }

    function dismiss() {
        incomingOn = false
        victoryOn = false
        tick.stop()
        victoryHide.stop()
    }

    Timer {
        id: tick
        interval: 1000
        repeat: true
        onTriggered: {
            overlay.remain = Math.max(0, overlay.remain - 1)
            if (overlay.remain <= 0) {
                overlay.incomingOn = false
                tick.stop()
            }
        }
    }
    Timer {
        id: victoryHide
        interval: 5200
        onTriggered: overlay.victoryOn = false
    }

    Rectangle {
        anchors.fill: parent
        color: overlay.victoryOn ? "#cc1a1200" : "#cc020202"
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (overlay.victoryOn)
                    overlay.dismiss()
            }
        }
    }

    Column {
        visible: overlay.incomingOn
        anchors.centerIn: parent
        spacing: 16
        width: Math.min(parent.width - 48, 520)
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "⚔"
            font.pixelSize: 42
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "ВЫЗОВ НА ДУЭЛЬ"
            color: "#fb923c"
            font.pixelSize: 26
            font.bold: true
            font.italic: true
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: overlay.challenge.line ? String(overlay.challenge.line)
                                         : "1 на 1 или зассал"
            color: "white"
            font.pixelSize: 16
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.remain + "с"
            color: "#fdba74"
            font.pixelSize: 22
            font.family: "Monospace"
            font.bold: true
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 12
            Rectangle {
                width: 160
                height: 48
                radius: 8
                color: "#111"
                border.color: "#444"
                Text {
                    anchors.centerIn: parent
                    text: "ОТКЛОНИТЬ"
                    color: "#9ca3af"
                    font.bold: true
                    font.pixelSize: 13
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof NetworkManager !== "undefined")
                            NetworkManager.declineArena(String(overlay.challenge.uuid || ""))
                        overlay.dismiss()
                    }
                }
            }
            Rectangle {
                width: 240
                height: 48
                radius: 8
                color: "#fb923c"
                Text {
                    anchors.centerIn: parent
                    text: "ПРИНЯТЬ ВЫЗОВ"
                    color: "#111"
                    font.bold: true
                    font.pixelSize: 13
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof NetworkManager !== "undefined")
                            NetworkManager.acceptArena(String(overlay.challenge.uuid || ""))
                        overlay.dismiss()
                    }
                }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: "Без ставок. За рейтинг клуба и царя горы."
            color: "#6b7280"
            font.pixelSize: 11
        }
    }

    Column {
        visible: overlay.victoryOn
        anchors.centerIn: parent
        spacing: 14
        width: Math.min(parent.width - 48, 520)
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "ПОБЕДА"
            color: "#fbbf24"
            font.pixelSize: 42
            font.bold: true
            font.italic: true
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: overlay.result.message ? String(overlay.result.message) : "Победа на арене"
            color: "white"
            font.pixelSize: 18
            font.bold: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.result.perk && overlay.result.perk.label
                  ? String(overlay.result.perk.label)
                  : (overlay.result.koth && overlay.result.koth.streak
                     ? ("Серия " + Number(overlay.result.koth.streak))
                     : "")
            color: "#fde68a"
            font.pixelSize: 16
        }
    }
}
