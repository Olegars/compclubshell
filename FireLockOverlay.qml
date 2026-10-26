import QtQuick

Item {
    id: lock
    anchors.fill: parent
    z: 4000
    visible: locked

    property bool locked: false

    Rectangle {
        anchors.fill: parent
        color: "#c40000"
    }

    MouseArea {
        anchors.fill: parent
        onPressed: mouse.accepted = true
    }

    Column {
        anchors.centerIn: parent
        spacing: 28
        width: parent.width * 0.8

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "ПОЖАР"
            color: "white"
            font.pixelSize: 120
            font.bold: true
            font.letterSpacing: 8
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Срочно покиньте комнату"
            color: "white"
            font.pixelSize: 48
            font.bold: true
        }
    }
}
