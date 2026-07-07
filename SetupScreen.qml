import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Rectangle {
    id: setupRoot
    anchors.fill: parent
    color: "#020202"

    // Подтягиваем зоны из C++ модели
    readonly property var zonesList: (typeof NetworkManager !== "undefined") ? NetworkManager.getAvailableZones() : ["STANDARD", "BOOTCAMP", "DUO"]

    Image {
        anchors.fill: parent
        source: "images/hex_bg.png"
        fillMode: Image.Tile
        opacity: 0.1
    }

    Column {
        anchors.centerIn: parent
        spacing: 35
        width: 500

        // Заголовок экрана конфигурации
        Column {
            width: parent.width
            spacing: 10
            Text {
                text: "REACTOR CONTROL"
                color: "#22c55e"
                font.pixelSize: 32
                font.bold: true
                font.letterSpacing: 2
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                text: "ПЕРВИЧНАЯ РЕГИСТРАЦИЯ ТЕРМИНАЛА"
                color: "#666666"
                font.pixelSize: 12
                font.bold: true
                font.letterSpacing: 1
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }

        // БЛОК ВВОДА ИМЕНИ КОМПЬЮТЕРА
        Column {
            width: parent.width
            spacing: 12

            Text {
                text: "НАЗВАНИЕ КОМПЬЮТЕРА (ПРИВЯЗКА К БАЗЕ)"
                color: "#22c55e"
                font.pixelSize: 11
                font.bold: true
                font.letterSpacing: 1
            }

            TextField {
                id: pcNameInputField
                width: parent.width
                height: 60
                font.pixelSize: 22
                font.bold: true
                font.family: "Roboto"
                font.letterSpacing: 1
                color: "white"
                placeholderText: "Например: PC-01, DUO-05..."
                placeholderTextColor: "#334155"
                selectionColor: "#22c55e"
                selectedTextColor: "black"
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: 20
                focus: true

                background: Rectangle {
                    color: pcNameInputField.activeFocus ? "#08120a" : "#0d130e"
                    border.color: pcNameInputField.activeFocus ? "#22c55e" : "#1a4d29"
                    border.width: pcNameInputField.activeFocus ? 2 : 1
                    radius: 4

                    Behavior on color { ColorAnimation { duration: 150 } }
                    Behavior on border.color { ColorAnimation { duration: 150 } }
                }
            }

            Text {
                id: errorValidationText
                text: "Пожалуйста, введите имя компьютера перед выбором зоны"
                color: "#ef4444"
                font.pixelSize: 12
                visible: false
            }
        }

        Rectangle {
            width: parent.width
            height: 1
            color: "#22c55e"
            opacity: 0.2
        }

        // БЛОК ВЫБОРА ИГРОВОЙ ЗОНЫ
        Column {
            width: parent.width
            spacing: 15

            Text {
                text: "ВЫБЕРИТЕ ИГРОВУЮ ЗОНУ ДЛЯ ЭТОГО ХАРДА"
                color: "#666666"
                font.pixelSize: 11
                font.bold: true
                font.letterSpacing: 1
            }

            Repeater {
                model: setupRoot.zonesList
                delegate: Button {
                    width: parent.width
                    height: 55

                    contentItem: Text {
                        text: modelData
                        color: btnMouse.containsMouse ? "black" : "#22c55e"
                        font.pixelSize: 16
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        color: btnMouse.containsMouse ? "#22c55e" : "#050a06"
                        border.color: "#1a4d29"
                        border.width: 1
                        radius: 4
                        Behavior on color { ColorAnimation { duration: 100 } }
                    }

                    MouseArea {
                        id: btnMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor

                        onClicked: {
                            var cleanName = pcNameInputField.text.trim();
                            if (cleanName === "") {
                                errorValidationText.visible = true;
                                pcNameInputField.forceActiveFocus();
                            } else {
                                errorValidationText.visible = false;
                                console.log("[SETUP] Регистрация станции. Имя:", cleanName, "| Зона:", modelData);

                                // Вызов обновлённого C++ метода с двумя параметрами!
                                NetworkManager.registerStation(modelData, cleanName);
                            }
                        }
                    }
                }
            }
        }
    }
}