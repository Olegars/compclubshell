import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import sector0451

Rectangle {
    id: setupRoot
    anchors.fill: parent
    color: Theme.bgRoot

    // Подтягиваем зоны из C++ модели
    readonly property var zonesList: (typeof NetworkManager !== "undefined")
                                     ? NetworkManager.getAvailableZones()
                                     : ["singl", "duo", "trio", "kvatro", "bootcamp", "tv"]

    Image {
        anchors.fill: parent
        source: Qt.resolvedUrl("images/hex_bg.png")
        fillMode: Image.Tile
        opacity: 0.3
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
                color: Theme.accent
                font.pixelSize: 32
                font.bold: true
                font.letterSpacing: 2
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                text: "ПЕРВИЧНАЯ РЕГИСТРАЦИЯ ТЕРМИНАЛА"
                color: Theme.textMuted
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
                color: Theme.accent
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
                selectionColor: Theme.accent
                selectedTextColor: "black"
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: 20
                focus: true

                background: Rectangle {
                    color: pcNameInputField.activeFocus ? Theme.accentSurface : Theme.accentSurfaceIdle
                    border.color: pcNameInputField.activeFocus ? Theme.accent : Theme.accentBorder
                    border.width: pcNameInputField.activeFocus ? 2 : 1
                    radius: Theme.radiusSm

                    Behavior on color { ColorAnimation { duration: 150 } }
                    Behavior on border.color { ColorAnimation { duration: 150 } }
                }
            }

            Text {
                id: errorValidationText
                text: "Пожалуйста, введите имя компьютера перед выбором зоны"
                color: Theme.danger
                font.pixelSize: 12
                visible: false
            }
        }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.accent
            opacity: 0.2
        }

        // БЛОК ВЫБОРА ИГРОВОЙ ЗОНЫ
        Column {
            width: parent.width
            spacing: 15

            Text {
                text: "ВЫБЕРИТЕ ИГРОВУЮ ЗОНУ ДЛЯ ЭТОГО ХАРДА"
                color: Theme.textMuted
                font.pixelSize: 11
                font.bold: true
                font.letterSpacing: 1
            }

            Repeater {
                model: setupRoot.zonesList
                delegate: Button {
                    id: zoneBtn
                    width: parent.width
                    height: 55
                    scale: zoneBtn.down ? 0.98 : 1.0

                    Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }

                    HoverHandler { cursorShape: Qt.PointingHandCursor }

                    contentItem: Text {
                        text: modelData
                        color: zoneBtn.hovered ? "black" : Theme.accent
                        font.pixelSize: 16
                        font.bold: true
                        font.letterSpacing: 1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        color: zoneBtn.down ? Theme.accentDeep
                                            : (zoneBtn.hovered ? Theme.accent : Theme.accentPanel)
                        border.color: zoneBtn.hovered ? Theme.accent : Theme.accentBorder
                        border.width: 1
                        radius: Theme.radiusSm
                        Behavior on color { ColorAnimation { duration: 100 } }
                        Behavior on border.color { ColorAnimation { duration: 100 } }
                    }

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