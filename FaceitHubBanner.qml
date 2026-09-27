import QtQuick
import sector0451

Rectangle {
    id: banner
    property var match: null
    visible: !!(match && match.id) && !(typeof NetworkManager !== "undefined" && NetworkManager.clanWar && NetworkManager.clanWar.id)
    color: "#cc1a0e00"
    border.color: "#ff9f1c"
    border.width: 1
    radius: 6

    readonly property string mapName: match && match.map ? String(match.map) : "FACEIT"
    readonly property string leftName: (match && match.factions && match.factions.length > 0) ? String(match.factions[0].name || "A") : "A"
    readonly property string rightName: (match && match.factions && match.factions.length > 1) ? String(match.factions[1].name || "B") : "B"
    readonly property int leftElo: (match && match.factions && match.factions.length > 0) ? Number(match.factions[0].elo || 0) : 0
    readonly property int rightElo: (match && match.factions && match.factions.length > 1) ? Number(match.factions[1].elo || 0) : 0

    Column {
        anchors.centerIn: parent
        spacing: 4
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "FACEIT · " + banner.mapName
            color: "#ffb020"
            font.pixelSize: 12
            font.bold: true
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 16
            Text { text: banner.leftName + "  " + banner.leftElo; color: "white"; font.pixelSize: 16; font.bold: true }
            Text { text: "VS"; color: "#ffb020"; font.pixelSize: 12 }
            Text { text: banner.rightElo + "  " + banner.rightName; color: "white"; font.pixelSize: 16; font.bold: true }
        }
    }
}
