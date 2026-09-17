import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import sector0451

Popup {
    id: arenaPopup
    width: Math.min(720, parent.width * 0.92)
    height: Math.min(820, parent.height * 0.94)
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int modeIndex: 0
    property int presetIndex: 0
    property string scope: "hall"
    property string kind: "duel"
    property int targetId: 0
    property int battleMax: 8
    property double customFee: 250
    property double raiseTo: 0
    readonly property var arena: (typeof NetworkManager !== "undefined") ? NetworkManager.arena : ({})
    readonly property var modes: (arena && arena.modes) ? arena.modes : [
        { id: "cs2:1v1_aim", game: "cs2", mode: "1v1_aim", label: "CS2 1v1 AIM" },
        { id: "cs2:2v2_wingman", game: "cs2", mode: "2v2_wingman", label: "CS2 2v2 Wingman" },
        { id: "dota:1v1_mid", game: "dota", mode: "1v1_mid", label: "Dota 2 1v1 Mid Only" }
    ]
    readonly property var presets: (arena && arena.presets) ? arena.presets : [100, 250, 500]
    readonly property var targets: (typeof NetworkManager !== "undefined") ? NetworkManager.bountyTargets : []
    readonly property var currentMode: modes[Math.min(modeIndex, Math.max(0, modes.length - 1))] || modes[0]
    readonly property var mine: (arena && arena.mine) ? arena.mine : null
    readonly property var board: (arena && arena.board) ? arena.board : ((arena && arena.open) ? arena.open : [])
    readonly property double fee: {
        if (presetIndex >= 0 && presetIndex < presets.length)
            return Number(presets[presetIndex] || 250)
        return Number(customFee || 250)
    }

    onOpened: {
        if (typeof NetworkManager !== "undefined")
            NetworkManager.fetchLanLive()
        if (targetBox.count > 0) {
            targetBox.currentIndex = 0
            arenaPopup.targetId = Number(targetBox.currentValue || 0)
        }
        if (arena && arena.max_battle_players)
            arenaPopup.battleMax = Number(arena.max_battle_players || 8)
    }

    background: Rectangle {
        color: "#050505"
        border.color: "#fb923c"
        border.width: 2
        radius: Theme.radiusSm
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "АРЕНА / ДУЭЛИ"
                color: "#fb923c"
                font.pixelSize: 22
                font.bold: true
                font.italic: true
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "✕"
                color: "white"
                font.pixelSize: 18
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    cursorShape: Qt.PointingHandCursor
                    onClicked: arenaPopup.close()
                }
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: (arena && arena.legal && arena.legal.notice)
                  ? String(arena.legal.notice)
                  : "Взнос за участие в соревновании мастерства. Приз на депозит клуба, без вывода на карту."
            color: "#9ca3af"
            font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [
                    { id: "duel", label: "Дуэль" },
                    { id: "battle", label: "Битва" }
                ]
                delegate: Rectangle {
                    Layout.fillWidth: true
                    height: 36
                    radius: 8
                    color: arenaPopup.kind === modelData.id ? "#fb923c" : "#111"
                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: arenaPopup.kind === modelData.id ? "#111" : "#d1d5db"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: arenaPopup.kind = modelData.id
                    }
                }
            }
        }

        ComboBox {
            Layout.fillWidth: true
            model: arenaPopup.modes
            textRole: "label"
            onActivated: arenaPopup.modeIndex = currentIndex
        }

        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: arenaPopup.presets
                delegate: Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    radius: 8
                    color: arenaPopup.presetIndex === index ? "#fb923c" : "#111"
                    border.color: "#fb923c"
                    Text {
                        anchors.centerIn: parent
                        text: Number(modelData) + " ₽"
                        color: arenaPopup.presetIndex === index ? "#111" : "#fdba74"
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: arenaPopup.presetIndex = index
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: arenaPopup.kind === "battle"
            Text {
                text: "Игроков"
                color: "#9ca3af"
                font.pixelSize: 12
            }
            Slider {
                Layout.fillWidth: true
                from: 3
                to: Math.max(3, Number((arena && arena.max_battle_players) ? arena.max_battle_players : 8))
                stepSize: 1
                value: arenaPopup.battleMax
                onMoved: arenaPopup.battleMax = Math.round(value)
            }
            Text {
                text: String(arenaPopup.battleMax)
                color: "#fb923c"
                font.bold: true
                font.pixelSize: 14
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [
                    { id: "hall", label: "Весь клуб" },
                    { id: "computer", label: "ПК" },
                    { id: "zone", label: "Межзонный" }
                ]
                delegate: Rectangle {
                    Layout.fillWidth: true
                    height: 36
                    radius: 8
                    color: arenaPopup.scope === modelData.id ? "#fb923c" : "#111"
                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: arenaPopup.scope === modelData.id ? "#111" : "#d1d5db"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: arenaPopup.scope = modelData.id
                    }
                }
            }
        }

        ComboBox {
            id: targetBox
            Layout.fillWidth: true
            visible: arenaPopup.scope === "computer"
            model: arenaPopup.targets
            textRole: "name"
            valueRole: "id"
            onActivated: arenaPopup.targetId = Number(currentValue || 0)
        }

        Rectangle {
            visible: !!(arenaPopup.mine && arenaPopup.mine.uuid)
            Layout.fillWidth: true
            implicitHeight: raiseCol.implicitHeight + 16
            radius: 8
            color: "#1a1208"
            border.color: "#fb923c"
            ColumnLayout {
                id: raiseCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                Text {
                    text: (arenaPopup.mine && arenaPopup.mine.kind_label ? arenaPopup.mine.kind_label : "Лобби")
                          + " · " + Number((arenaPopup.mine && arenaPopup.mine.entry_fee) ? arenaPopup.mine.entry_fee : 0) + " ₽"
                    color: "#fdba74"
                    font.bold: true
                    font.pixelSize: 13
                }
                Text {
                    visible: !!(arenaPopup.mine && arenaPopup.mine.raise_to)
                    text: "Повысить до " + Number(arenaPopup.mine.raise_to || 0) + " ₽"
                    color: "#fde68a"
                    font.pixelSize: 12
                }
                RowLayout {
                    TextField {
                        Layout.fillWidth: true
                        placeholderText: "Новая ставка"
                        color: "white"
                        onTextChanged: arenaPopup.raiseTo = Number(text || 0)
                    }
                    Text {
                        visible: !!(arenaPopup.mine && arenaPopup.mine.can_raise)
                        text: "Повысить"
                        color: "#fb923c"
                        font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.proposeArenaRaise(String(arenaPopup.mine.uuid || ""), arenaPopup.raiseTo)
                            }
                        }
                    }
                    Text {
                        visible: !!(arenaPopup.mine && arenaPopup.mine.can_vote_raise)
                        text: "Согласен"
                        color: "#4ade80"
                        font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.voteArenaRaise(String(arenaPopup.mine.uuid || ""), true)
                            }
                        }
                    }
                    Text {
                        visible: !!(arenaPopup.mine && arenaPopup.mine.can_start)
                        text: "Старт"
                        color: "#4ade80"
                        font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.startArena(String(arenaPopup.mine.uuid || ""))
                            }
                        }
                    }
                }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: arenaPopup.board
            delegate: Rectangle {
                width: ListView.view.width
                height: 64
                radius: 8
                color: "#111"
                border.color: "#fb923c"
                border.width: 1
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: (modelData.kind_label || "Дуэль") + " · " + (modelData.mode_label || "")
                            color: "white"
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            font.bold: true
                        }
                        Text {
                            text: Number(modelData.entry_fee || 0) + " ₽ · "
                                  + Number(modelData.players_count || 1) + "/" + Number(modelData.max_players || 2)
                                  + (modelData.scheduled_at ? " · " + Qt.formatDateTime(new Date(modelData.scheduled_at), "dd.MM HH:mm") : "")
                            color: "#9ca3af"
                            font.pixelSize: 11
                        }
                    }
                    Text {
                        visible: !!modelData.can_cancel
                        text: "Снять"
                        color: "#f87171"
                        font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.cancelArena(String(modelData.uuid || ""))
                            }
                        }
                    }
                    Text {
                        visible: !!modelData.can_accept && !modelData.can_cancel
                        text: "Принять"
                        color: "#fb923c"
                        font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.acceptArena(String(modelData.uuid || ""))
                            }
                        }
                    }
                }
            }
        }

        Button {
            Layout.fillWidth: true
            text: "БРОСИТЬ ВЫЗОВ · " + arenaPopup.fee + " ₽"
            onClicked: {
                var m = arenaPopup.currentMode || {}
                if (typeof NetworkManager !== "undefined") {
                    NetworkManager.createArenaChallenge(
                        String(m.game || "cs2"),
                        String(m.mode || "1v1_aim"),
                        arenaPopup.fee,
                        arenaPopup.scope,
                        arenaPopup.scope === "computer" ? arenaPopup.targetId : 0,
                        arenaPopup.kind,
                        arenaPopup.kind === "battle" ? arenaPopup.battleMax : 0
                    )
                }
                arenaPopup.close()
            }
        }
    }
}
