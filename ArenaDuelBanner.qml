import QtQuick
import sector0451

Rectangle {
    id: banner
    property var duel: null
    readonly property var ticker: {
        if (duel && duel.line)
            return duel
        if (typeof NetworkManager !== "undefined" && NetworkManager.arena && NetworkManager.arena.ticker)
            return NetworkManager.arena.ticker
        return null
    }
    visible: !!(ticker && ticker.line)
             && !(typeof NetworkManager !== "undefined" && NetworkManager.clanWar && NetworkManager.clanWar.id)
             && !(typeof NetworkManager !== "undefined" && NetworkManager.faceitMatch && NetworkManager.faceitMatch.id)
    color: "#cc120800"
    border.color: "#fb923c"
    border.width: 1
    radius: 6
    height: visible ? 72 : 0

    Column {
        anchors.centerIn: parent
        spacing: 4
        width: parent.width - 24
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "ARENA"
            color: "#fb923c"
            font.pixelSize: Theme.fontCaption
            font.bold: true
            font.letterSpacing: 3
            opacity: 0.85
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: banner.ticker && banner.ticker.line ? String(banner.ticker.line) : ""
            color: "white"
            font.pixelSize: 16
            font.bold: true
            font.italic: true
        }
    }
}
