import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import sector0451

// Отдельный tool-HWND: не child шелла (transientParent=null), иначе при
// hideShellForGame исчезает вместе с главным окном. Topmost удерживаем
// таймером через Launcher.raiseTopmostToolWindow — как раньше у вкладки «SHELL».
// WindowDoesNotAcceptFocus + NOACTIVATE: клик/наведение не сворачивают игру.
Window {
    id: quickMenu
    title: "REACTOR Quick"
    visible: false
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus
    transientParent: null

    property bool expanded: false
    property bool tipMode: false
    property var apps: []
    // После сворачивания по leave — не открывать сразу снова с того же hover.
    property bool suppressPeek: false

    readonly property bool shellAvailable: typeof Launcher !== "undefined"
                                          && Launcher.hasActiveGame
                                          && Launcher.shellHiddenForGame

    // Тонкая видимая полоска + чуть шире hit-area окна (прозрачный запас).
    readonly property int stripVisibleW: 4
    readonly property int stripW: 12
    readonly property int stripH: 56
    readonly property int panelW: 300
    readonly property int panelH: Math.max(170,
                                           82
                                           + (shellAvailable ? 46 : 0)
                                           + apps.length * 46
                                           + (tipMode ? 74 : 0))

    width: expanded ? panelW : stripW
    height: expanded ? panelH : stripH

    function placeTopLeft() {
        var scr = Screen
        if (!scr || scr.width <= 0)
            return
        x = scr.virtualX
        y = scr.virtualY
        if (typeof Qt !== "undefined" && Qt.application && Qt.application.screens) {
            var screens = Qt.application.screens
            if (screens && screens.length > 0) {
                var primary = screens[0]
                x = primary.virtualX
                // Чуть ниже самого края — меньше конфликтов с системными жестами.
                y = primary.virtualY + Math.round(primary.height * 0.18)
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

    function refocusGame() {
        if (typeof Launcher !== "undefined"
                && typeof Launcher.focusGameWindow === "function")
            Launcher.focusGameWindow()
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
        leaveClose.stop()
    }

    function collapseToCorner() {
        tipMode = false
        expanded = false
        suppressPeek = true
        peekCooldown.restart()
        leaveClose.stop()
        placeTopLeft()
        visible = true
        reassertTopmost()
        refocusGame()
    }

    function hideAll() {
        tipMode = false
        expanded = false
        visible = false
        leaveClose.stop()
        peekOpen.stop()
        topmostHold.stop()
    }

    function launchApp(app) {
        if (!app || !app.available || !app.path)
            return
        if (typeof Launcher !== "undefined")
            Launcher.launchDetached(app.path, app.args || "")
        collapseToCorner()
    }

    function goToShell() {
        if (typeof Launcher === "undefined")
            return
        Launcher.switchToShell()
        // syncQuickMenu спрячет панель, когда shellHiddenForGame станет false.
    }

    onVisibleChanged: {
        if (visible) {
            placeTopLeft()
            reassertTopmost()
            topmostHold.restart()
        } else {
            topmostHold.stop()
            leaveClose.stop()
        }
    }

    Timer {
        id: topmostHold
        interval: 1500
        repeat: true
        running: false
        onTriggered: quickMenu.reassertTopmost()
    }

    Timer {
        id: peekCooldown
        interval: 450
        onTriggered: quickMenu.suppressPeek = false
    }

    Timer {
        id: leaveClose
        interval: 220
        onTriggered: {
            if (quickMenu.expanded && !panelHover.hovered)
                quickMenu.collapseToCorner()
        }
    }

    Connections {
        target: typeof NetworkManager !== "undefined" ? NetworkManager : null
        function onQuickAppsChanged() {
            quickMenu.refreshApps()
        }
    }

    // --- Свёрнутая полоска у левого края ---
    Item {
        anchors.fill: parent
        visible: !quickMenu.expanded

        Rectangle {
            id: stripBar
            width: quickMenu.stripVisibleW
            height: parent.height
            anchors.left: parent.left
            // Кислотный лайм — максимально ядовитый неон.
            color: stripHover.hovered ? "#f5ff4a" : "#cfff00"
            opacity: stripHover.hovered ? 1 : stripPulse.value
            radius: 1
            border.width: stripHover.hovered ? 1 : 0
            border.color: "#ffffff"

            SequentialAnimation {
                id: stripPulse
                property real value: 1
                running: !stripHover.hovered && quickMenu.visible && !quickMenu.expanded
                loops: Animation.Infinite
                NumberAnimation {
                    target: stripPulse
                    property: "value"
                    from: 1.0; to: 0.65
                    duration: 650
                    easing.type: Easing.InOutSine
                }
                NumberAnimation {
                    target: stripPulse
                    property: "value"
                    from: 0.65; to: 1.0
                    duration: 650
                    easing.type: Easing.InOutSine
                }
            }
        }

        HoverHandler {
            id: stripHover
            onHoveredChanged: {
                if (hovered) {
                    if (!quickMenu.suppressPeek)
                        peekOpen.restart()
                } else {
                    peekOpen.stop()
                }
            }
        }

        Timer {
            id: peekOpen
            interval: 280
            onTriggered: {
                if (stripHover.hovered && !quickMenu.expanded && !quickMenu.suppressPeek)
                    quickMenu.openExpanded(false)
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (!quickMenu.suppressPeek)
                    quickMenu.openExpanded(false)
            }
        }
    }

    // --- Развёрнутая панель ---
    Rectangle {
        id: panelRoot
        anchors.fill: parent
        visible: quickMenu.expanded
        color: Theme.bgPanel
        border.width: 1
        border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.45)
        radius: Theme.radiusSm

        HoverHandler {
            id: panelHover
            onHoveredChanged: {
                if (hovered)
                    leaveClose.stop()
                else if (quickMenu.expanded)
                    leaveClose.restart()
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

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
                    text: "Discord · Telegram · Shell · утилиты"
                    color: Theme.textMuted
                    font.pixelSize: 10
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
                    text: "Чтобы снова открыть — наведите на тонкую полоску у левого края. Уберите мышь — меню свернётся."
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

                // SHELL — в том же меню, вместо отдельной вкладки справа.
                Rectangle {
                    visible: quickMenu.shellAvailable
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    radius: Theme.radiusSm
                    color: shellArea.containsMouse ? "#141414" : "#0d0d0d"
                    border.width: 1
                    border.color: shellArea.containsMouse ? Theme.accent : "#1f1f1f"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: "☰  SHELL"
                        color: Theme.accent
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: "в клуб"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                    }

                    MouseArea {
                        id: shellArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: quickMenu.goToShell()
                    }
                }

                Text {
                    visible: quickMenu.apps.length === 0 && !quickMenu.shellAvailable
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
