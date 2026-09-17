import QtQuick
import sector0451

Rectangle {
    id: banner
    property var war: null
    visible: war && war.id
    color: "#cc050510"
    border.color: Theme.accentBorder
    border.width: 1
    radius: 6

    readonly property string leftLabel: (war && war.side_a && war.side_a.label) ? String(war.side_a.label) : "A"
    readonly property string rightLabel: (war && war.side_b && war.side_b.label) ? String(war.side_b.label) : "B"
    readonly property int leftScore: (war && war.side_a) ? Number(war.side_a.score || 0) : 0
    readonly property int rightScore: (war && war.side_b) ? Number(war.side_b.score || 0) : 0
    readonly property int remain: {
        if (!war) return 0
        return Math.max(0, Number(war.remaining_sec || 0) - tick)
    }
    property int tick: 0

    Timer {
        interval: 1000
        running: banner.visible
        repeat: true
        onTriggered: banner.tick += 1
    }

    onWarChanged: banner.tick = 0

    function clock(sec) {
        var s = Math.max(0, parseInt(sec, 10) || 0)
        var m = Math.floor(s / 60)
        var r = s % 60
        return (m < 10 ? "0" : "") + m + ":" + (r < 10 ? "0" : "") + r
    }

    Column {
        anchors.centerIn: parent
        spacing: 4
        width: parent.width - 24

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "CLAN WAR" + (banner.remain > 0 ? ("  " + banner.clock(banner.remain)) : "")
            color: Theme.accent
            font.pixelSize: Theme.fontCaption
            font.bold: true
            font.letterSpacing: 3
            opacity: 0.85
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 16
            Text {
                text: banner.leftLabel
                color: "#c084fc"
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideRight
                width: Math.min(220, banner.width * 0.28)
                horizontalAlignment: Text.AlignRight
            }
            Text {
                text: banner.leftScore + "  :  " + banner.rightScore
                color: "white"
                font.pixelSize: 28
                font.bold: true
                font.family: "Monospace"
            }
            Text {
                text: banner.rightLabel
                color: "#22c55e"
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideRight
                width: Math.min(220, banner.width * 0.28)
            }
        }
    }
}
