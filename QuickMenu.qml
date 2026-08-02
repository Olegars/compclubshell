import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import sector0451

// Отдельный tool-HWND: не child шелла (transientParent=null), иначе при
// hideShellForGame исчезает вместе с главным окном. Topmost удерживаем
// таймером через Launcher.raiseTopmostToolWindow — как у вкладки «SHELL».
Window {
    id: quickMenu
    title: "REACTOR Quick"
    visible: false
    color: "transparent"
    // Без DoesNotAcceptFocus: иначе TapHandler часто не кликается поверх игры.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    transientParent: null

    property bool expanded: false
    property bool tipMode: false
    property var apps: []

    readonly property int cornerSize: 48
    readonly property int panelW: 300
    readonly property int panelH: Math.max(170,
                                           82 + apps.length * 46
                                           + (tipMode ? 74 : 0))

    width: expanded ? panelW : cornerSize
    height: expanded ? panelH : cornerSize

    function placeTopLeft() {
        // Как ShellToggle: primaryScreen.availableGeometry, не virtualX/Y.
        var scr = Screen
        if (!scr || scr.width <= 0)
            return
        x = scr.virtualX
        y = scr.virtualY
        // На мультимониторе Screen у Window — экран, где окно уже стоит;
        // при первом показе прижимаем к primary через C++-подобный расчёт:
        if (typeof Qt !== "undefined" && Qt.application && Qt.application.screens) {
            var screens = Qt.application.screens
            if (screens && screens.length > 0) {
                var primary = screens[0]
                for (var i = 0; i < screens.length; i++) {
                    if (screens[i].manufacturer !== undefined) { /* keep */ }
                }
                // Первый экран в списке обычно primary.
                x = primary.virtualX
                y = primary.virtualY
            }
        }
    }

    function reassertTopmost() {
        if (!visible)
            return
        if (typeof Launcher !== "undefined"
                && typeof Launcher.raiseTopmostToolWindow === "function")
            Launcher.raiseTopmostToolWindow(quickMenu)
        else
            raise()
    }

    function refreshApps() {
        if (typeof NetworkManager !== "undefined")
            apps = NetworkManager.quickApps
        else
            apps = []
    }

    function openExpanded(withTip) {
        tipMode = !!withTip
        refreshApps()
        expanded = true
        placeTopLeft()
        visible = true
        reassertTopmost()
    }

    function collapseToCorner() {
        tipMode = false
        expanded = false
        placeTopLeft()
        visible = true
        reassertTopmost()
    }

    function hideAll() {
        tipMode = false
        expanded = false
        visible = false
        topmostHold.stop()
    }

    function launchApp(app) {
        if (!app || !app.available || !app.path)
            return
        if (typeof Launcher !== "undefined")
            Launcher.launchDetached(app.path, app.args || "")
        collapseToCorner()
    }

    onVisibleChanged: {
        if (visible) {
            placeTopLeft()
            reassertTopmost()
            topmostHold.restart()
        } else {
            topmostHold.stop()
        }
    }

    // Игры в exclusive fullscreen периодически перекрывают TOPMOST — как у SHELL.
    Timer {
        id: topmostHold
        interval: 1500
        repeat: true
        running: false
        onTriggered: quickMenu.reassertTopmost()
    }

    Connections {
        target: typeof NetworkManager !== "undefined" ? NetworkManager : null
        function onQuickAppsChanged() {
            quickMenu.refreshApps()
        }
    }

    // --- Свёрнутый уголок ---
    Rectangle {
        anchors.fill: parent
        visible: !quickMenu.expanded
        color: "#030704"
        opacity: cornerHover.hovered ? 0.95 : 0.72
        border.width: 1
        border.color: Theme.accent

        Text {
            anchors.centerIn: parent
            text: "☰"
            color: Theme.accent
            font.pixelSize: 18
            font.bold: true
        }

        HoverHandler {
            id: cornerHover
            onHoveredChanged: {
                if (hovered)
                    peekOpen.restart()
                else
                    peekOpen.stop()
            }
        }

        Timer {
            id: peekOpen
            interval: 280
            onTriggered: {
                if (cornerHover.hovered && !quickMenu.expanded)
                    quickMenu.openExpanded(false)
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: quickMenu.openExpanded(false)
        }
    }

    // --- Развёрнутая панель ---
    Rectangle {
        anchors.fill: parent
        visible: quickMenu.expanded
        color: Theme.bgPanel
        border.width: 1
        border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.45)
        radius: Theme.radiusSm

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        text: "БЫСТРЫЙ ЗАПУСК"
                        color: Theme.accent
                        font.pixelSize: 11
                        font.bold: true
                        font.letterSpacing: 1.6
                    }
                    Text {
                        visible: quickMenu.tipMode
                        text: "Discord · Telegram · утилиты"
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                    radius: Theme.radiusSm
                    color: closeArea.containsMouse ? "#171717" : "transparent"
                    border.width: 1
                    border.color: closeArea.containsMouse ? "#4d4d4d" : "#232323"

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: closeArea.containsMouse ? "#ffffff" : "#6b6b6b"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: quickMenu.collapseToCorner()
                    }
                }
            }

            Rectangle {
                visible: quickMenu.tipMode
                Layout.fillWidth: true
                Layout.preferredHeight: tipLabel.implicitHeight + 18
                radius: Theme.radiusSm
                color: "#0d1a12"
                border.width: 1
                border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.35)

                Text {
                    id: tipLabel
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Чтобы снова открыть меню — наведите курсор в левый верхний угол"
                    color: Theme.accentBright
                    font.pixelSize: 11
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6

                Text {
                    visible: quickMenu.apps.length === 0
                    Layout.fillWidth: true
                    text: "В админке пока нет приложений\n(/admin/quick-apps)"
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Repeater {
                    model: quickMenu.apps

                    delegate: Rectangle {
                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        radius: Theme.radiusSm
                        opacity: modelData.available ? 1 : 0.45
                        color: modelData.available && appArea.containsMouse ? "#141414" : "#0d0d0d"
                        border.width: 1
                        border.color: modelData.available && appArea.containsMouse
                                      ? Theme.accent : "#1f1f1f"

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.title
                            color: modelData.available ? Theme.textBody : Theme.textMuted
                            font.pixelSize: 13
                            font.bold: true
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.available ? "›" : "нет файла"
                            color: modelData.available ? Theme.accent : Theme.textMuted
                            font.pixelSize: 11
                            font.bold: true
                        }

                        MouseArea {
                            id: appArea
                            anchors.fill: parent
                            enabled: modelData.available
                            hoverEnabled: true
                            cursorShape: modelData.available ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: quickMenu.launchApp(modelData)
                        }
                    }
                }
            }
        }
    }
}
