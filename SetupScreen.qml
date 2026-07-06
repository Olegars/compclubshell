import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: setupRoot
    anchors.fill: parent
    color: "#050505"

    ColumnLayout {
        anchors.centerIn: parent
        width: 420
        spacing: 25

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: "⚙️ ИНИЦИАЛИЗАЦИЯ ТЕРМИНАЛА"
            color: "#00FF66"
            font.pixelSize: 22
            font.bold: true
            font.letterSpacing: 1
        }

        // Выпадающий список зон (Сингл, Дуо, Трио...)
        ColumnLayout {
            spacing: 6
            Layout.fillWidth: true
            Text { text: "ТИП ИГРОВОЙ ЗОНЫ:"; color: "#a3a3a3"; font.pixelSize: 11; font.bold: true }
            ComboBox {
                id: zoneComboBox
                Layout.fillWidth: true
                height: 48
                model: NetworkManager.getAvailableZones()

                delegate: ItemDelegate {
                    id: delegateItem
                    width: zoneComboBox.width
                    text: modelData
                    contentItem: Text {
                        text: delegateItem.text
                        color: delegateItem.highlighted ? "black" : "white"
                        font.bold: true
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 15
                    }
                    background: Rectangle { color: delegateItem.highlighted ? "#00FF66" : "#151515" }
                }

                contentItem: Text {
                    text: zoneComboBox.currentText
                    color: "white"
                    font.pixelSize: 14
                    font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 15
                }

                background: Rectangle { color: "#111"; border.color: "#333"; radius: 4 }
            }
        }

        Item { height: 10; Layout.fillHeight: true }

        // Кнопка привязки
        Rectangle {
            Layout.fillWidth: true
            height: 50
            color: "#00FF66"
            radius: 4

            Text {
                anchors.centerIn: parent
                text: "ЗАРЕГИСТРИРОВАТЬ ПК В БАЗЕ"
                color: "black"
                font.bold: true
                font.pixelSize: 13
                font.letterSpacing: 0.5
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    console.log("[QML-CLICK] Отправка запроса регистрации для зоны:", zoneComboBox.currentText);
                    NetworkManager.registerStation(zoneComboBox.currentText);
                }
            }
        }
    }
}