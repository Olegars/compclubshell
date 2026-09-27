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
    property string scope: "hall"
    property string kind: "duel"
    property int targetId: 0
    property int battleMax: 8
    readonly property var arena: (typeof NetworkManager !== "undefined") ? NetworkManager.arena : ({})
    readonly property var modes: (arena && arena.modes) ? arena.modes : [
        { id: "cs2:1v1_aim", game: "cs2", mode: "1v1_aim", label: "CS2 1v1 AIM" },
        { id: "cs2:2v2_wingman", game: "cs2", mode: "2v2_wingman", label: "CS2 2v2 Wingman" },
        { id: "dota:1v1_mid", game: "dota", mode: "1v1_mid", label: "Dota 2 1v1 Mid Only" }
    ]
    readonly property var targets: (typeof NetworkManager !== "undefined") ? NetworkManager.bountyTargets : []
    readonly property var currentMode: modes[Math.min(modeIndex, Math.max(0, modes.length - 1))] || modes[0]
    readonly property var mine: (arena && arena.mine) ? arena.mine : null
    readonly property var board: (arena && arena.board) ? arena.board : ((arena && arena.open) ? arena.open : [])
    readonly property var ladder: (arena && arena.ladder) ? arena.ladder : []
    readonly property var boss: (arena && arena.boss) ? arena.boss : null
    readonly property var koth: (arena && arena.koth) ? arena.koth : null
    readonly property var me: (arena && arena.me) ? arena.me : null

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
                  : "Дуэль без ставок: кто лучше на этом ПК. Рейтинг клуба, серия побед, царь горы."
            color: "#9ca3af"
            font.pixelSize: 12
        }

        Text {
            visible: !!(arenaPopup.me)
            Layout.fillWidth: true
            text: arenaPopup.me
                  ? ("Ты · Elo " + Number(arenaPopup.me.rating || 1000)
                     + " · серия " + Number(arenaPopup.me.streak || 0)
                     + (arenaPopup.me.title ? (" · " + arenaPopup.me.title) : ""))
                  : ""
            color: "#fdba74"
            font.pixelSize: 12
            font.bold: true
        }

        Rectangle {
            visible: !!(arenaPopup.koth && arenaPopup.koth.name)
            Layout.fillWidth: true
            implicitHeight: 44
            radius: 8
            color: "#1a1408"
            border.color: "#fbbf24"
            Text {
                anchors.fill: parent
                anchors.margins: 10
                text: arenaPopup.koth
                      ? ("Царь горы: " + String(arenaPopup.koth.name || "")
                         + " · серия " + Number(arenaPopup.koth.streak || 0)
                         + (arenaPopup.koth.perk ? (" · " + arenaPopup.koth.perk) : ""))
                      : ""
                color: "#fde68a"
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
            }
        }

        Rectangle {
            visible: !!(arenaPopup.boss && arenaPopup.boss.name)
            Layout.fillWidth: true
            implicitHeight: 48
            radius: 8
            color: "#1a1208"
            border.color: "#fb923c"
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                Text {
                    Layout.fillWidth: true
                    text: arenaPopup.boss
                          ? ("Босс клуба: " + String(arenaPopup.boss.name || "")
                             + " · Elo " + Number(arenaPopup.boss.rating || 1000)
                             + (arenaPopup.boss.in_club ? (" · " + String(arenaPopup.boss.pc || "в зале")) : " · не в клубе"))
                          : ""
                    color: "#fdba74"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    visible: !!(arenaPopup.boss && arenaPopup.boss.in_club && arenaPopup.boss.computer_id)
                    text: "ВЫЗВАТЬ"
                    color: "#fb923c"
                    font.bold: true
                    font.pixelSize: 12
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        onClicked: {
                            var m = arenaPopup.currentMode || {}
                            if (typeof NetworkManager !== "undefined") {
                                NetworkManager.createArenaChallenge(
                                    String(m.game || "cs2"),
                                    String(m.mode || "1v1_aim"),
                                    0,
                                    "computer",
                                    Number(arenaPopup.boss.computer_id || 0),
                                    "duel",
                                    0
                                )
                            }
                            arenaPopup.close()
                        }
                    }
                }
            }
        }

        Repeater {
            model: arenaPopup.ladder
            delegate: Text {
                visible: index < 5
                Layout.fillWidth: true
                text: Number(modelData.rank || (index + 1)) + ". " + String(modelData.name || "Игрок")
                      + (modelData.title ? (" · " + modelData.title) : "")
                      + " · " + Number(modelData.rating || 0)
                color: "#9ca3af"
                font.pixelSize: 11
                font.family: "Monospace"
            }
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
                          + " · " + Number((arenaPopup.mine && arenaPopup.mine.players_count) ? arenaPopup.mine.players_count : 1)
                          + "/" + Number((arenaPopup.mine && arenaPopup.mine.max_players) ? arenaPopup.mine.max_players : 2)
                    color: "#fdba74"
                    font.bold: true
                    font.pixelSize: 13
                }
                RowLayout {
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
                    Text {
                        visible: !!(arenaPopup.mine && arenaPopup.mine.can_cancel)
                        text: "Снять"
                        color: "#f87171"
                        font.bold: true
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.cancelArena(String(arenaPopup.mine.uuid || ""))
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
                            text: "Elo " + Number(modelData.creator_rating || 1000) + " · "
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
            text: "БРОСИТЬ ВЫЗОВ"
            onClicked: {
                var m = arenaPopup.currentMode || {}
                if (typeof NetworkManager !== "undefined") {
                    NetworkManager.createArenaChallenge(
                        String(m.game || "cs2"),
                        String(m.mode || "1v1_aim"),
                        0,
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
