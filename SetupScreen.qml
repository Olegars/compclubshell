import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import sector0451

Rectangle {
    id: setupRoot
    anchors.fill: parent
    color: Theme.bgRoot

    readonly property var zonesList: (typeof NetworkManager !== "undefined")
                                     ? NetworkManager.getAvailableZones()
                                     : ["singl", "duo", "trio", "kvatro", "bootcamp", "tv"]

    readonly property bool pcRegistered: typeof NetworkManager !== "undefined"
                                         && NetworkManager.computerId > 0

    property string fanMsg: ""
    property bool fanMsgOk: true
    property int selectedBoardId: 0
    property int selectedK1: 0
    property int selectedK2: 0
    property string selectedHost: ""
    property int selectedPort: 30000
    property string selectedLabel: ""

    readonly property var occupiedFans: {
        var seen = ({})
        var out = []
        var boards = (typeof NetworkManager !== "undefined") ? NetworkManager.fanDiscoverBoards : []
        for (var i = 0; i < boards.length; ++i) {
            var board = boards[i]
            var pairs = board.pairs || []
            for (var j = 0; j < pairs.length; ++j) {
                var p = pairs[j]
                var id = Number(p.fan_id || 0)
                if (id <= 0 || seen[id])
                    continue
                seen[id] = true
                out.push({
                    fan_id: id,
                    label: p.label || "",
                    host: board.host || "",
                    status: p.status || "",
                    space_name: p.space_name || ""
                })
            }
        }
        return out
    }

    AvatarWatermarkBg {
        anchors.fill: parent
    }

    Component.onCompleted: {
        if (typeof Ccboot !== "undefined")
            Ccboot.refresh()
        if (pcRegistered)
            NetworkManager.fetchFanDiscover()
    }

    Connections {
        target: typeof NetworkManager !== "undefined" ? NetworkManager : null
        function onComputerIdChanged() {
            if (setupRoot.pcRegistered)
                NetworkManager.fetchFanDiscover()
        }
        function onFanBindFinished(ok, message) {
            setupRoot.fanMsgOk = ok
            setupRoot.fanMsg = message
        }
        function onFanTestFinished(ok, message) {
            setupRoot.fanMsgOk = ok
            setupRoot.fanMsg = message
        }
    }

    signal requestClose()

    function closeSetup() {
        requestClose()
        var win = Window.window
        if (win)
            win.closeSetupScreen()
    }

    function selectPair(board, pair) {
        if (!board || !pair || pair.status === "taken")
            return
        selectedBoardId = board.id
        selectedHost = board.host
        selectedPort = board.port
        selectedK1 = pair.channel
        selectedK2 = pair.channel2
        selectedLabel = (board.name || "") + " · " + pair.label
        fanMsg = "Выбрано: " + selectedLabel
        fanMsgOk = true
    }

    function runTest() {
        if (selectedBoardId <= 0 || selectedK1 <= 0)
            return
        fanMsg = "Тест 100% ~2.5с…"
        fanMsgOk = true
        NetworkManager.testFanPair(selectedHost, selectedPort, selectedK1, selectedK2)
    }

    function runBind() {
        if (selectedBoardId <= 0 || selectedK1 <= 0)
            return
        NetworkManager.bindFanPair(selectedBoardId, selectedK1, selectedK2)
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 36
        contentWidth: width
        contentHeight: mainCol.implicitHeight + 20
        clip: true

        Column {
            id: mainCol
            width: Math.min(760, flick.width)
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 22

            RowLayout {
                width: parent.width
                Text {
                    Layout.fillWidth: true
                    text: ((typeof NetworkManager !== "undefined" && NetworkManager.clubName)
                           ? NetworkManager.clubName : "Клуб") + " CONTROL"
                    color: Theme.accent
                    font.pixelSize: 28
                    font.bold: true
                    font.letterSpacing: 2
                }
                Button {
                    text: "ВЫХОД"
                    implicitWidth: 120
                    implicitHeight: 40
                    onClicked: setupRoot.closeSetup()
                    contentItem: Text {
                        text: parent.text
                        color: Theme.accent
                        font.pixelSize: 13
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: Theme.accentPanel
                        border.color: Theme.accent
                        border.width: 1
                        radius: 4
                    }
                }
            }

            Text {
                text: setupRoot.pcRegistered
                      ? "НАСТРОЙКА · terminal #" + NetworkManager.computerId
                      : "ПЕРВИЧНАЯ РЕГИСТРАЦИЯ ТЕРМИНАЛА"
                color: Theme.textMuted
                font.pixelSize: 12
                font.bold: true
                font.letterSpacing: 1
            }

            // Registration (only if not registered)
            Column {
                width: parent.width
                spacing: 10
                visible: !setupRoot.pcRegistered

                Text {
                    text: "НАЗВАНИЕ КОМПЬЮТЕРА"
                    color: Theme.accent
                    font.pixelSize: 11
                    font.bold: true
                }
                TextField {
                    id: pcNameInputField
                    width: parent.width
                    height: 52
                    font.pixelSize: 18
                    font.bold: true
                    color: "white"
                    placeholderText: "PC-01, DUO-05…"
                    placeholderTextColor: "#334155"
                    leftPadding: 14
                    background: Rectangle {
                        color: pcNameInputField.activeFocus ? Theme.accentSurface : Theme.accentSurfaceIdle
                        border.color: pcNameInputField.activeFocus ? Theme.accent : Theme.accentBorder
                        border.width: 1
                        radius: Theme.radiusSm
                    }
                }
                Text {
                    id: errorValidationText
                    text: "Введите имя перед выбором зоны"
                    color: Theme.danger
                    font.pixelSize: 12
                    visible: false
                }
                Text {
                    text: "ЗОНА"
                    color: Theme.textMuted
                    font.pixelSize: 11
                    font.bold: true
                    topPadding: 6
                }
                Repeater {
                    model: setupRoot.zonesList
                    delegate: Button {
                        id: zoneBtn
                        width: mainCol.width
                        height: 46
                        contentItem: Text {
                            text: modelData
                            color: zoneBtn.hovered ? "black" : Theme.accent
                            font.pixelSize: 14
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: zoneBtn.hovered ? Theme.accent : Theme.accentPanel
                            border.color: Theme.accentBorder
                            radius: Theme.radiusSm
                        }
                        onClicked: {
                            var cleanName = pcNameInputField.text.trim()
                            if (!cleanName) {
                                errorValidationText.visible = true
                                return
                            }
                            errorValidationText.visible = false
                            NetworkManager.registerStation(modelData, cleanName)
                        }
                    }
                }
            }

            // Super Client — правка образа с этого ПК
            Rectangle {
                width: parent.width
                radius: 8
                color: "#0a0f0b"
                border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.35)
                border.width: 1
                implicitHeight: scInner.implicitHeight + 32

                Column {
                    id: scInner
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 10

                    Text {
                        text: "ОБРАЗ · SUPER CLIENT"
                        color: Theme.accent
                        font.pixelSize: 13
                        font.bold: true
                        font.letterSpacing: 1.4
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Вход в setup: Win+ПКМ (или Ctrl+клик по имени ПК). Пароль — Admin Password CCBoot, не PIN брони. После Enable ПК уйдёт в reboot; киоск снимется. Сохранение образа — Disable Super Client, не кнопка в шелле."
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        visible: typeof Ccboot !== "undefined"
                        text: typeof Ccboot !== "undefined" ? Ccboot.lastMessage : ""
                        color: typeof Ccboot !== "undefined" && Ccboot.lastOk ? Theme.success : Theme.danger
                        font.pixelSize: 12
                        font.bold: true
                    }
                    Text {
                        visible: typeof Ccboot !== "undefined" && Ccboot.superClientActive
                        text: "СЕЙЧАС: SUPER CLIENT ВКЛЮЧЁН"
                        color: Theme.warning
                        font.pixelSize: 12
                        font.bold: true
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        visible: typeof Ccboot !== "undefined"
                        text: typeof Ccboot !== "undefined" && Ccboot.clientFound
                              ? ("Клиент: " + Ccboot.clientPath)
                              : "CCBootClient.exe не найден — укажите Diskless/client_exe в config.ini"
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        font.family: "Monospace"
                    }

                    Text {
                        text: "ПАРОЛЬ CCBOOT"
                        color: Theme.accent
                        font.pixelSize: 11
                        font.bold: true
                    }
                    TextField {
                        id: scPassword
                        width: parent.width
                        height: 44
                        echoMode: TextInput.Password
                        font.pixelSize: 16
                        color: "white"
                        placeholderText: "Admin Password"
                        placeholderTextColor: "#334155"
                        leftPadding: 14
                        background: Rectangle {
                            color: scPassword.activeFocus ? Theme.accentSurface : Theme.accentSurfaceIdle
                            border.color: scPassword.activeFocus ? Theme.accent : Theme.accentBorder
                            border.width: 1
                            radius: Theme.radiusSm
                        }
                    }

                    Text {
                        text: "ДИСК"
                        color: Theme.accent
                        font.pixelSize: 11
                        font.bold: true
                    }
                    ComboBox {
                        id: scDisk
                        width: parent.width
                        height: 40
                        model: ["image", "disk", "both"]
                        currentIndex: 0
                    }

                    Row {
                        spacing: 8
                        Button {
                            text: "ВКЛЮЧИТЬ SUPER CLIENT"
                            height: 36
                            enabled: typeof Ccboot !== "undefined" && !Ccboot.busy
                            onClicked: {
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.setMaintenance(true)
                                Ccboot.enableSuperClient(scPassword.text, scDisk.currentText)
                            }
                            contentItem: Text {
                                text: parent.text
                                color: parent.enabled ? "#111" : Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: parent.enabled ? Theme.accent : "#222"
                                radius: 4
                            }
                        }
                        Button {
                            text: "ВЫКЛЮЧИТЬ И СОХРАНИТЬ"
                            height: 36
                            enabled: typeof Ccboot !== "undefined" && !Ccboot.busy
                            onClicked: Ccboot.disableSuperClient(scPassword.text, true)
                            contentItem: Text {
                                text: parent.text
                                color: parent.enabled ? "#111" : Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: parent.enabled ? Theme.warning : "#222"
                                radius: 4
                            }
                        }
                    }
                    Row {
                        spacing: 8
                        Button {
                            text: "ТОЛЬКО CCBOOT CLIENT"
                            height: 34
                            enabled: typeof Ccboot !== "undefined" && !Ccboot.busy
                            onClicked: Ccboot.openCcbootClient()
                            contentItem: Text {
                                text: parent.text
                                color: Theme.accent
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle { color: "#111"; border.color: "#333"; radius: 4 }
                        }
                        Button {
                            text: "СНЯТЬ КИОСК"
                            height: 34
                            onClicked: {
                                if (typeof Ccboot !== "undefined")
                                    Ccboot.unlockForMaintenance()
                                if (typeof NetworkManager !== "undefined")
                                    NetworkManager.setMaintenance(true)
                            }
                            contentItem: Text {
                                text: parent.text
                                color: Theme.accent
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle { color: "#111"; border.color: "#333"; radius: 4 }
                        }
                        Button {
                            text: "ВЕРНУТЬ КИОСК"
                            height: 34
                            onClicked: {
                                if (typeof Launcher !== "undefined" && typeof Launcher.exitMaintenance === "function") {
                                    Launcher.exitMaintenance()
                                } else {
                                    if (typeof NetworkManager !== "undefined")
                                        NetworkManager.setMaintenance(false)
                                    if (typeof Ccboot !== "undefined")
                                        Ccboot.lockKiosk()
                                }
                            }
                            contentItem: Text {
                                text: parent.text
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle { color: "#111"; border.color: "#333"; radius: 4 }
                        }
                    }
                }
            }

            // Fan discovery
            Rectangle {
                width: parent.width
                visible: setupRoot.pcRegistered
                radius: 8
                color: "#0a0f0b"
                border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.35)
                border.width: 1
                implicitHeight: fanInner.implicitHeight + 32

                Column {
                    id: fanInner
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 10

                    Text {
                        text: "ВЕНТИЛЯТОР · DISCOVERY"
                        color: Theme.accent
                        font.pixelSize: 13
                        font.bold: true
                        font.letterSpacing: 1.4
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "ТЕСТ пульсирует 100% ~2.5с. Услышали свой — ПРИВЯЗАТЬ. Комната берётся из зоны регистрации. В комнате до "
                              + NetworkManager.fanDiscoverSlotsMax + " вентиляторов."
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: NetworkManager.fanDiscoverStatus
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        font.family: "Monospace"
                    }
                    Text {
                        visible: setupRoot.fanMsg.length > 0
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: setupRoot.fanMsg
                        color: setupRoot.fanMsgOk ? Theme.success : Theme.danger
                        font.pixelSize: 12
                        font.bold: true
                    }

                    Row {
                        spacing: 8
                        Button {
                            text: "ОБНОВИТЬ СПИСОК"
                            height: 34
                            enabled: !NetworkManager.fanDiscoverBusy
                            onClicked: NetworkManager.fetchFanDiscover()
                            contentItem: Text {
                                text: parent.text
                                color: Theme.accent
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle { color: "#111"; border.color: "#333"; radius: 4 }
                        }
                        Button {
                            text: "ТЕСТ"
                            height: 34
                            enabled: setupRoot.selectedBoardId > 0 && !NetworkManager.fanDiscoverBusy
                            onClicked: setupRoot.runTest()
                            contentItem: Text {
                                text: parent.text
                                color: parent.enabled ? "#111" : Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: parent.enabled ? Theme.accent : "#222"
                                radius: 4
                            }
                        }
                        Button {
                            text: "ПРИВЯЗАТЬ"
                            height: 34
                            enabled: setupRoot.selectedBoardId > 0
                                     && !NetworkManager.fanDiscoverBusy
                                     && NetworkManager.fanDiscoverSlotsUsed < NetworkManager.fanDiscoverSlotsMax
                            onClicked: setupRoot.runBind()
                            contentItem: Text {
                                text: parent.text
                                color: parent.enabled ? "#111" : Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: parent.enabled ? Theme.success : "#222"
                                radius: 4
                            }
                        }
                    }

                    Text {
                        visible: setupRoot.selectedLabel.length > 0
                        text: "Выбрано: " + setupRoot.selectedLabel
                        color: Theme.textPrimary
                        font.pixelSize: 12
                        font.bold: true
                    }

                    Column {
                        width: parent.width
                        spacing: 6
                        visible: setupRoot.occupiedFans.length > 0
                        Text {
                            text: "ПРИВЯЗАНО (" + setupRoot.occupiedFans.length
                                  + "/" + NetworkManager.fanDiscoverSlotsMax + ")"
                            color: Theme.success
                            font.pixelSize: 11
                            font.bold: true
                        }
                        Repeater {
                            model: setupRoot.occupiedFans
                            delegate: RowLayout {
                                width: parent.width
                                required property var modelData
                                Text {
                                    Layout.fillWidth: true
                                    text: "#" + modelData.fan_id + " · " + modelData.label
                                          + " · " + modelData.host
                                          + (modelData.status === "taken" && modelData.space_name
                                             ? (" · " + modelData.space_name) : "")
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    font.family: "Monospace"
                                    elide: Text.ElideRight
                                }
                                Button {
                                    text: "ОТВЯЗАТЬ"
                                    height: 28
                                    enabled: !NetworkManager.fanDiscoverBusy
                                    onClicked: NetworkManager.unbindFan(modelData.fan_id)
                                    contentItem: Text {
                                        text: parent.text
                                        color: Theme.danger
                                        font.pixelSize: 10
                                        font.bold: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: "#1a0808"
                                        border.color: "#662222"
                                        radius: 4
                                    }
                                }
                            }
                        }
                    }

                    Repeater {
                        model: NetworkManager.fanDiscoverBoards

                        delegate: Column {
                            id: boardCol
                            width: fanInner.width
                            spacing: 8
                            required property var modelData

                            Text {
                                text: (boardCol.modelData.name || "BOARD")
                                      + "  http://" + boardCol.modelData.host + "/" + boardCol.modelData.port + "/"
                                color: Theme.textPrimary
                                font.pixelSize: 12
                                font.bold: true
                                font.family: "Monospace"
                            }

                            Flow {
                                width: parent.width
                                spacing: 8

                                Repeater {
                                    model: boardCol.modelData.pairs

                                    delegate: Rectangle {
                                        id: pairCard
                                        required property var modelData
                                        readonly property bool isMine: modelData.status === "mine"
                                        readonly property bool isTaken: modelData.status === "taken"
                                        readonly property bool isSel: setupRoot.selectedBoardId === boardCol.modelData.id
                                                                     && setupRoot.selectedK1 === modelData.channel
                                                                     && setupRoot.selectedK2 === modelData.channel2

                                        width: 158
                                        height: 56
                                        radius: 6
                                        opacity: isTaken ? 0.4 : 1
                                        color: isSel ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.22)
                                                     : (isMine ? Qt.rgba(0.13, 0.77, 0.36, 0.14) : "#101010")
                                        border.width: 1
                                        border.color: isSel ? Theme.accent
                                                            : (isMine ? Theme.success : "#333")

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 2
                                            Text {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: pairCard.modelData.label
                                                      + (pairCard.isMine ? " ✓" : "")
                                                color: pairCard.isMine ? Theme.success : Theme.textPrimary
                                                font.pixelSize: 13
                                                font.bold: true
                                            }
                                            Text {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                visible: pairCard.isTaken
                                                text: pairCard.modelData.space_name || "занят"
                                                color: Theme.textMuted
                                                font.pixelSize: 10
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: !pairCard.isTaken
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: setupRoot.selectPair(boardCol.modelData, pairCard.modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        visible: NetworkManager.fanDiscoverBoards.length === 0
                                 && !NetworkManager.fanDiscoverBusy
                        text: "Нет активных плат W5100 в клубе — добавьте плату в /admin/fans"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        width: parent.width
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Button {
                width: parent.width
                height: 52
                text: "ВЫХОД ИЗ SETUP"
                onClicked: setupRoot.closeSetup()
                contentItem: Text {
                    text: parent.text
                    color: Theme.accent
                    font.pixelSize: 15
                    font.bold: true
                    font.letterSpacing: 1.2
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: Theme.accentPanel
                    border.color: Theme.accent
                    border.width: 1
                    radius: 6
                }
            }

            Item { width: 1; height: 24 }
        }
    }
}
