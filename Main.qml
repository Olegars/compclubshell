// Путь: Main.qml
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtWebSockets
import QtMultimedia
import QtQuick.Shapes

Window {
    id: root
    width: 1920
    height: 1080
    visible: true
    title: "REACTOR SHELL Sector 0451"
    color: "#020202"
    flags: Qt.Window | Qt.FramelessWindowHint
    visibility: Window.FullScreen

    property int terminalId: 0
    property string sessionUser: "GUEST"
    property real sessionBalance: 0.0
    property string sessionTime: "00:00:00"
    property alias authScreen: screenSwitcher
    property string temporaryPausePin: "----"
    property string pcTypeFromDatabase: "standard"
    property int currentGameId: 0
    property bool isHardwareAdmin: false

    property bool hasActiveOrder: false
    property string orderStatusText: "ЗАКАЗ В РАБОТЕ"

    property string pcNameString: "PC-UNKNOWN"

    property string authErrorMessage: ""
    property bool authErrorVisible: false

    property string sessionPhone: ""
    property string sessionUserBeforePause: ""
    property bool isLoggingIn: false
    property var pendingOverlaysData: null
    property string loadingPlatform: ""
    property string loadingGameTitle: ""

    function showGameLoading(platform, gameTitle) {
        hideGameLoadingTimer.stop()
        if (typeof platform === "string" && platform.length > 0)
            root.loadingPlatform = platform
        if (typeof gameTitle === "string" && gameTitle.length > 0)
            root.loadingGameTitle = gameTitle
        steamLoadingOverlay.platformName = root.loadingPlatform || "LAUNCHER"
        steamLoadingOverlay.gameTitle = root.loadingGameTitle || "ИГРЫ"
        steamLoadingOverlay.running = true
        steamLoadingOverlay.visible = true
        if (typeof Launcher !== "undefined")
            Launcher.setShellTopmost(true)
        root.raise()
        root.requestActivate()
    }

    function updateGameLoading(platform, gameTitle) {
        if (typeof platform === "string" && platform.length > 0) {
            root.loadingPlatform = platform
            steamLoadingOverlay.platformName = platform
        }
        if (typeof gameTitle === "string" && gameTitle.length > 0) {
            root.loadingGameTitle = gameTitle
            steamLoadingOverlay.gameTitle = gameTitle
        }
    }

    function hideGameLoading() {
        hideGameLoadingTimer.stop()
        steamLoadingOverlay.running = false
        steamLoadingOverlay.visible = false
        root.isLoggingIn = false
        // Только оверлей. Shell hide/show — только из C++ (hideShellForGame / showShellAfterGame).
    }

    function scheduleHideGameLoading() {
        hideGameLoadingTimer.restart()
    }

    // Совместимость со старыми именами
    function showSteamLoading() { showGameLoading() }
    function hideSteamLoading() { hideGameLoading() }
    function scheduleHideSteamLoading() { scheduleHideGameLoading() }

    readonly property string fallbackVideo: "file:///C:/ShellVideo/Cache/fallback_bg.mp4"

    readonly property int blockWidth: 524
    readonly property int blockHeight: 295
    readonly property int sideMargin: 50

    onTerminalIdChanged: {
        if (terminalId > 0)
            NetworkManager.fetchOverlays(terminalId)
    }

    function resetAuthForm() {
        if (typeof authCenter !== 'undefined' && authCenter !== null) {
            authCenter.authStep = 1
        }
        if (typeof phoneInput !== 'undefined' && phoneInput !== null) {
            phoneInput.text = ""
        }
        if (typeof pinInput !== 'undefined' && pinInput !== null) {
            pinInput.text = ""
        }
        root.authErrorVisible = false
        root.authErrorMessage = ""
        root.isLoggingIn = false
        root.sessionPhone = ""

        if (typeof phoneInput !== 'undefined' && phoneInput !== null) {
            phoneInput.forceActiveFocus()
            phoneInput.cursorPosition = 4
        }
    }

    function resumeFromPause(enteredPin) {
        var cleanPin = String(enteredPin || "").trim().replace(/[^0-9]/g, "")
        var cleanTarget = root.temporaryPausePin.trim().replace(/[^0-9]/g, "")
        if (cleanPin.length !== 4 || cleanPin !== cleanTarget)
            return false

        var baseUrl = "http://192.168.222.2:22222"
        if (typeof NetworkManager !== 'undefined' && NetworkManager.serverUrl)
            baseUrl = NetworkManager.serverUrl

        var xhr = new XMLHttpRequest()
        xhr.open("POST", baseUrl + "/api/shell/games/unpause")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return

            if (xhr.status !== 200) {
                console.error("[PAUSE] unpause HTTP", xhr.status, xhr.responseText)
            }

            var restoreName = root.sessionUserBeforePause
            root.temporaryPausePin = "----"
            root.sessionUserBeforePause = ""
            root.sessionUser = restoreName.length > 0 ? restoreName : "PLAYER"
            screenSwitcher.sourceComponent = null
            dashboardLoader.source = "Dashboard.qml"
        }
        xhr.send(JSON.stringify({
            "computer_id": parseInt(root.terminalId) || 0,
            "booking_id": (typeof NetworkManager !== 'undefined') ? NetworkManager.lastBookingId : 0,
            "pin_code": cleanPin
        }))
        return true
    }

    onSessionUserChanged: {
        if (root.sessionUser === "PAUSE" || root.sessionUser === "GUEST" || root.sessionUser === "") {
            NetworkManager.fetchOverlays(root.terminalId > 0 ? root.terminalId : 1)

            if (root.sessionUser === "PAUSE") {
                dashboardLoader.source = ""
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent
                }
            } else if (root.sessionUser === "GUEST" || root.sessionUser === "") {
                // ИСПРАВЛЕНО: Если лаунчер прямо сейчас находится в процессе отправки запроса, НЕ сбрасываем форму!
                if (!root.isLoggingIn) {
                    root.resetAuthForm()
                }
                dashboardLoader.source = ""
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent
                }
            }
        }
    }

    Connections {
        target: NetworkManager

        function onSetupRequired() {
            console.log("[REACTOR-SHELL] Получен сигнал setupRequired. Переключение на SetupScreen.qml")
            screenSwitcher.sourceComponent = null
            setupScreenLoader.source = "SetupScreen.qml"
        }

        function onAuthRequired() {
            console.log("[REACTOR-SHELL] Получен сигнал authRequired. Загрузка AuthScreen.")
            root.pcNameString = NetworkManager.getCurrentPcName()
            setupScreenLoader.source = ""
            screenSwitcher.sourceComponent = loginScreenComponent
            // Используем ID из БД (computer_id), а не цифру из имени PC-1
            root.terminalId = NetworkManager.computerId > 0
                ? NetworkManager.computerId
                : (parseInt(root.pcNameString.replace(/[^0-9]/g, "")) || 0)
            console.log("[DEBUG-MAIN] Конец onAuthRequired. Итоговый terminalId =", root.terminalId)
        }

        function onLoginSucceeded(userName, balance, timeRemaining, phone) {
            if (typeof Launcher !== "undefined") Launcher.applyQosPolicies(true)
            root.sessionPhone = phone
            root.sessionUser = userName
            root.sessionBalance = balance
            root.sessionTime = timeRemaining
            screenSwitcher.sourceComponent = null
            dashboardLoader.source = "Dashboard.qml"
            NetworkManager.fetchGames()
            NetworkManager.fetchProducts()
        }

        function onLoginFailed(message) {
            root.authErrorMessage = message
            root.authErrorVisible = true
        }

        function onLoginRequestFinished() {
            root.isLoggingIn = false
        }

        function onOverlaysReady(data) {
            console.log("[OVERLAYS] overlaysReady получен, контейнер status=", overlaysContainer.status)
            updateOverlaysToScreen(data)
        }
    }

    Connections {
        target: Launcher

        function onGameStarted() {
        }

        function onGameStartedSuccessfully() {
            // Оверлей держим, пока C++ не спрячет shell (~2.5с) — без мигания рабочего стола
            hideGameLoadingTimer.interval = 2500
            hideGameLoadingTimer.restart()
        }

        function onGameFinished() {
            // Не вызывать hideGameLoading→hide shell: C++ уже showShellAfterGame()
            steamLoadingOverlay.running = false
            steamLoadingOverlay.visible = false
            root.isLoggingIn = false
            NetworkManager.freeGameAccount(parseInt(root.terminalId), parseInt(root.currentGameId))
        }
    }

    Timer {
        id: hideGameLoadingTimer
        interval: 3000
        repeat: false
        onTriggered: root.hideGameLoading()
    }

    Timer {
        id: hideDelayTimer
        interval: 30000
        running: false
        repeat: false
        onTriggered: {
            root.isLoggingIn = false
            gameLoadingOverlay.opacity = 0.0
        }
    }

    Component.onCompleted: {
        console.log("[START-TRACE] [STEP QML-A] ...Загрузка корневого окна...")
    }

    Loader {
        id: setupScreenLoader
        anchors.fill: parent
        z: 100
    }

    Loader {
        id: overlaysContainer
        anchors.fill: parent
        z: 50
        active: (root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE") && !setupScreenLoader.item

        onStatusChanged: {
            if (status === Loader.Ready) {
                if (root.pendingOverlaysData)
                    updateOverlaysToScreen(root.pendingOverlaysData)
                else
                    NetworkManager.fetchOverlays(root.terminalId > 0 ? root.terminalId : 1)
            }
        }

        sourceComponent: Component {
            Item {
                id: overlaysInnerItem
                anchors.fill: parent
                property alias b1: blockTopLeft
                property alias b2: blockTopRight
                property alias b3: blockMidLeft
                property alias b4: blockBottomLeft
                property alias b5: blockMidRight
                property alias b6: blockBottomRight

                Column {
                    id: leftColumn
                    anchors.left: parent.left
                    anchors.leftMargin: root.sideMargin
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 20

                    OverlayBlock {
                        id: blockTopLeft
                        blockUniqueId: "b1"
                        title: "CAM_01 / TOP_LEFT"
                        width: root.blockWidth
                        height: root.blockHeight
                        fallbackVideo: root.fallbackVideo
                        playbackAllowed: root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE"
                    }
                    OverlayBlock {
                        id: blockMidLeft
                        blockUniqueId: "b3"
                        title: "DAT_02 / MID_LEFT"
                        width: root.blockWidth
                        height: root.blockHeight
                        fallbackVideo: root.fallbackVideo
                        playbackAllowed: root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE"
                    }
                    OverlayBlock {
                        id: blockBottomLeft
                        width: root.blockWidth
                        height: root.blockHeight
                        blockUniqueId: "b4"
                        title: "INF_03 / BOTTOM_LEFT"
                        fallbackVideo: root.fallbackVideo
                        playbackAllowed: root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE"
                    }
                }

                Column {
                    id: rightColumn
                    anchors.right: parent.right
                    anchors.rightMargin: root.sideMargin
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 20

                    OverlayBlock {
                        id: blockTopRight
                        blockUniqueId: "b2"
                        title: "CAM_04 / TOP_RIGHT"
                        width: root.blockWidth
                        height: root.blockHeight
                        fallbackVideo: root.fallbackVideo
                        playbackAllowed: root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE"
                    }
                    OverlayBlock {
                        id: blockMidRight
                        blockUniqueId: "b5"
                        title: "DAT_05 / MID_RIGHT"
                        width: root.blockWidth
                        height: root.blockHeight
                        fallbackVideo: root.fallbackVideo
                        playbackAllowed: root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE"
                    }
                    OverlayBlock {
                        id: blockBottomRight
                        width: root.blockWidth
                        height: root.blockHeight
                        blockUniqueId: "b6"
                        title: "INF_06 / BOTTOM_RIGHT"
                        fallbackVideo: root.fallbackVideo
                        playbackAllowed: root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE"
                    }
                }
            }
        }
    }

    Loader { id: dashboardLoader; anchors.fill: parent; source: ""; visible: dashboardLoader.status === Loader.Ready; z: 10 }
    Loader { id: screenSwitcher; anchors.fill: parent; z: 20 }

    Component {
        id: loginScreenComponent
        Item {
            id: loginScreen
            anchors.fill: parent

            Rectangle {
                anchors.fill: parent
                color: "#020202"
                Image {
                    anchors.fill: parent
                    source: Qt.resolvedUrl("images/hex_bg.png")
                    fillMode: Image.Tile
                    opacity: 0.35
                    onStatusChanged: if (status === Image.Error)
                        console.warn("[BG] hex_bg load failed:", source)
                }
                Shape {
                    anchors.fill: parent
                    layer.enabled: true
                    layer.effect: MultiEffect { blurEnabled: true; blur: 0.3 }
                    ShapePath {
                        fillGradient: RadialGradient {
                            centerX: 1920 / 2
                            centerY: 1080 / 2
                            centerRadius: 1920 / 1.2
                            GradientStop { position: 0.0; color: "transparent" }
                            GradientStop { position: 1.0; color: "black" }
                        }
                        PathRectangle { x: 0; y: 0; width: 1920; height: 1080 }
                    }
                }
            }

            Item {
                id: logoArea
                width: 800
                height: 120
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: 40
                Row {
                    anchors.centerIn: parent
                    spacing: 20
                    Text { text: "REACTOR"; color: "#000"; font.pixelSize: 80; style: Text.Outline; styleColor: "#22c55e" }
                    Text { text: "0 4 5 1"; color: "#22c55e"; font.pixelSize: 60; font.bold: true; opacity: 0.8 }
                }
            }

            Rectangle {
                id: authCenter
                width: 420
                height: 520
                anchors.centerIn: parent
                color: "#050a06"
                border.color: root.sessionUser === "PAUSE" ? "#1d4ed8" : "#1a4d29"
                border.width: 2
                radius: 4
                opacity: 0.95
                property int authStep: 1

                Column {
                    anchors.fill: parent
                    anchors.margins: 40
                    spacing: 25

                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        Text { text: "TERMINAL_ID"; color: root.sessionUser === "PAUSE" ? "#3b82f6" : "#22c55e"; font.pixelSize: 12; opacity: 0.6; anchors.horizontalCenter: parent.horizontalCenter }
                        Text {
                            text: root.pcNameString
                            color: "white"
                            font.pixelSize: 54
                            font.bold: true
                            anchors.horizontalCenter: parent.horizontalCenter
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton
                                onClicked: { if (mouse.modifiers & Qt.ControlModifier) { screenSwitcher.sourceComponent = null; setupScreenLoader.source = "SetupScreen.qml" } }
                            }
                        }
                    }

                    Rectangle { width: parent.width; height: 1; color: root.sessionUser === "PAUSE" ? "#3b82f6" : "#22c55e"; opacity: 0.3 }

                    Item {
                        width: parent.width
                        height: 230

                        Column {
                            visible: root.sessionUser === "PAUSE"
                            width: parent.width
                            spacing: 15

                            Text { text: "ОЖИДАЮ ВОЗВРАЩЕНИЯ"; color: "#3b82f6"; font.pixelSize: 20; font.bold: true; font.letterSpacing: 1; anchors.horizontalCenter: parent.horizontalCenter }
                            Text { text: "Введите PIN-код для разблокировки"; color: "#a3a3a3"; font.pixelSize: 12; anchors.horizontalCenter: parent.horizontalCenter }

                            TextField {
                                id: pausePinInput
                                width: parent.width
                                height: 55
                                font.pixelSize: 22
                                font.bold: false
                                font.family: "Roboto"
                                font.letterSpacing: 4
                                inputMask: "0000;_"
                                echoMode: TextInput.Normal
                                color: "white"
                                selectionColor: "#3b82f6"
                                selectedTextColor: "black"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: TextInput.AlignVCenter
                                focus: false

                                Timer { id: pauseFocusTimer; interval: 80; running: false; repeat: false; onTriggered: { pausePinInput.forceActiveFocus(); pausePinInput.cursorPosition = 0 } }
                                Component.onCompleted: { if (root.sessionUser === "PAUSE") pauseFocusTimer.start() }
                                onVisibleChanged: { if (visible && root.sessionUser === "PAUSE") pauseFocusTimer.start() }
                                onAccepted: {
                                    if (!root.resumeFromPause(pausePinInput.text))
                                        pauseErrorText.visible = true
                                    else {
                                        pauseErrorText.visible = false
                                        pausePinInput.text = ""
                                    }
                                }
                                background: Rectangle { color: pausePinInput.activeFocus ? "#08162a" : "#0d1117"; border.color: pausePinInput.activeFocus ? "#3b82f6" : "#1d4ed8"; border.width: pausePinInput.activeFocus ? 2 : 1; radius: 4 }
                            }

                            Text { id: pauseErrorText; text: "Неверный PIN-код"; visible: false; color: "#ef4444"; font.pixelSize: 12; anchors.horizontalCenter: parent.horizontalCenter }
                            Button {
                                width: parent.width
                                height: 50
                                text: "Я ВЕРНУЛСЯ"
                                onClicked: {
                                    if (!root.resumeFromPause(pausePinInput.text))
                                        pauseErrorText.visible = true
                                    else {
                                        pauseErrorText.visible = false
                                        pausePinInput.text = ""
                                    }
                                }
                            }
                        }

                        Column {
                            visible: root.sessionUser !== "PAUSE"
                            width: parent.width
                            spacing: 12

                            Column {
                                visible: authCenter.authStep === 1
                                width: parent.width
                                spacing: 12
                                Text { text: "НОМЕР ТЕЛЕФОНА"; color: "#22c55e"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2; anchors.horizontalCenter: parent.horizontalCenter }
                                TextField {
                                    id: phoneInput
                                    width: parent.width
                                    height: 55
                                    font.pixelSize: 22
                                    font.bold: false
                                    font.family: "Roboto"
                                    font.letterSpacing: 1
                                    inputMask: "+7 (999) 999-99-99;_"
                                    focus: authCenter.authStep === 1 && root.sessionUser !== "PAUSE"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: "white"
                                    selectionColor: "#22c55e"
                                    selectedTextColor: "black"
                                    Timer { id: focusTimer; interval: 50; running: false; repeat: false; onTriggered: { if (root.sessionUser !== "PAUSE" && authCenter.authStep === 1) { phoneInput.forceActiveFocus(); phoneInput.cursorPosition = 4 } } }
                                    Component.onCompleted: { if (root.sessionUser !== "PAUSE") focusTimer.start() }
                                    onVisibleChanged: { if (visible && authCenter.authStep === 1 && root.sessionUser !== "PAUSE") focusTimer.start() }
                                    onActiveFocusChanged: { if (activeFocus && (text === "+7 (   )   -  -  " || text === "")) { Qt.callLater(function() { phoneInput.cursorPosition = 4 }) } }
                                    onAccepted: { authCenter.authStep = 2 }
                                    background: Rectangle { color: phoneInput.activeFocus ? "#08120a" : "#0d130e"; border.color: phoneInput.activeFocus ? "#22c55e" : "#1a4d29"; border.width: phoneInput.activeFocus ? 2 : 1; radius: 4 }
                                }
                            }

                            Column {
                                visible: authCenter.authStep === 2
                                width: parent.width
                                spacing: 12
                                Text { text: "PIN-КОД"; color: "#22c55e"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2; anchors.horizontalCenter: parent.horizontalCenter }
                                TextField {
                                    id: pinInput
                                    width: parent.width
                                    height: 55
                                    font.pixelSize: 22
                                    font.bold: false
                                    font.family: "Roboto"
                                    font.letterSpacing: 4
                                    inputMask: "0000;_"
                                    echoMode: TextInput.Normal
                                    focus: authCenter.authStep === 2 && root.sessionUser !== "PAUSE"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: "white"
                                    selectionColor: "#22c55e"
                                    selectedTextColor: "black"
                                    onActiveFocusChanged: { if (activeFocus) { Qt.callLater(function() { pinInput.cursorPosition = 0 }) } }
                                    onAccepted: {
                                        if (root.isLoggingIn)
                                            return
                                        root.authErrorVisible = false
                                        root.isLoggingIn = true
                                        NetworkManager.login(phoneInput.text, pinInput.text, parseInt(root.terminalId))
                                    }
                                    background: Rectangle { color: pinInput.activeFocus ? "#08120a" : "#0d130e"; border.color: pinInput.activeFocus ? "#22c55e" : "#1a4d29"; border.width: pinInput.activeFocus ? 2 : 1; radius: 4 }
                                }
                            }

                            Text {
                                id: authErrorText
                                text: root.authErrorMessage
                                visible: root.authErrorVisible
                                color: "#ef4444"
                                font.bold: true
                                anchors.horizontalCenter: parent.horizontalCenter
                                Connections { target: pinInput; function onTextChanged() { root.authErrorVisible = false } }
                                Connections { target: phoneInput; function onTextChanged() { root.authErrorVisible = false } }
                            }

                            Item { height: 5; width: 1 }

                            Rectangle {
                                id: authActionBtn
                                width: parent.width
                                height: 55
                                radius: 4
                                color: {
                                    if (authBtnMouse.pressed)
                                        return "#15803d"
                                    if (authBtnMouse.containsMouse)
                                        return "#16a34a"
                                    return "#22c55e"
                                }
                                scale: authBtnMouse.pressed ? 0.95 : 1.0
                                opacity: (root.isLoggingIn && authCenter.authStep === 2) ? 0.85 : 1.0

                                Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
                                Behavior on color { ColorAnimation { duration: 120 } }
                                Behavior on opacity { NumberAnimation { duration: 150 } }

                                readonly property bool waitingLogin: root.isLoggingIn && authCenter.authStep === 2

                                Text {
                                    anchors.centerIn: parent
                                    visible: !authActionBtn.waitingLogin
                                    text: authCenter.authStep === 1 ? "ДАЛЕЕ" : "НАЧАТЬ СЕССИЮ"
                                    color: "#020202"
                                    font.pixelSize: 15
                                    font.bold: true
                                    font.letterSpacing: 2
                                }

                                // Простой надёжный спиннер (без BusyIndicator/Shape)
                                Item {
                                    id: authWaitClock
                                    anchors.centerIn: parent
                                    width: 28
                                    height: 28
                                    visible: authActionBtn.waitingLogin
                                    opacity: visible ? 1 : 0

                                    NumberAnimation on rotation {
                                        running: authWaitClock.visible
                                        from: 0
                                        to: 360
                                        loops: Animation.Infinite
                                        duration: 750
                                        easing.type: Easing.Linear
                                    }

                                    Canvas {
                                        id: authWaitCanvas
                                        anchors.fill: parent
                                        onPaint: {
                                            var ctx = getContext("2d")
                                            var cx = width / 2
                                            var cy = height / 2
                                            var r = Math.min(cx, cy) - 2.5
                                            ctx.reset()
                                            ctx.lineWidth = 3
                                            ctx.lineCap = "round"
                                            ctx.strokeStyle = "#020202"
                                            ctx.beginPath()
                                            ctx.arc(cx, cy, r, -Math.PI * 0.5, Math.PI * 0.9)
                                            ctx.stroke()
                                        }
                                        Component.onCompleted: requestPaint()
                                        onWidthChanged: requestPaint()
                                        onHeightChanged: requestPaint()
                                    }
                                }

                                MouseArea {
                                    id: authBtnMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: !(root.isLoggingIn && authCenter.authStep === 2)
                                    onClicked: {
                                        if (authCenter.authStep === 1) {
                                            authCenter.authStep = 2
                                        } else {
                                            root.authErrorVisible = false
                                            root.isLoggingIn = true
                                            NetworkManager.login(phoneInput.text, pinInput.text, parseInt(root.terminalId))
                                        }
                                    }
                                }
                            }

                            Text {
                                id: backBtn
                                text: "НАЗАД"
                                font.pixelSize: 12
                                font.bold: true
                                font.letterSpacing: 2
                                color: backMouse.containsMouse ? "#22c55e" : "#666666"
                                visible: authCenter.authStep === 2
                                anchors.horizontalCenter: parent.horizontalCenter
                                Layout.topMargin: 5

                                Behavior on color { ColorAnimation { duration: 100 } }

                                MouseArea {
                                    id: backMouse; anchors.fill: parent; anchors.margins: -10; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        pinInput.text = ""
                                        root.authErrorVisible = false
                                        authCenter.authStep = 1
                                        focusTimer.start()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    function updateOverlaysToScreen(response) {
        root.pendingOverlaysData = response
        if (overlaysContainer.status !== Loader.Ready || !overlaysContainer.item) {
            console.log("[OVERLAYS] Контейнер ещё не Ready — данные отложены")
            return
        }

        var actualData = response.data ? response.data : response
        var item = overlaysContainer.item
        var map = {
            "top_left": item.b1, "top_right": item.b2,
            "mid_left": item.b3, "mid_right": item.b5,
            "bottom_left": item.b4, "bottom_right": item.b6
        }

        for (var key in map) {
            if (actualData[key] && map[key]) {
                var vUrl = ""
                var blockData = actualData[key]
                var layers = null
                if (blockData.content && blockData.content.layers)
                    layers = blockData.content.layers

                // QVariantList из C++ не всегда проходит Array.isArray — проверяем length
                if (layers && layers.length !== undefined) {
                    for (var i = 0; i < layers.length; i++) {
                        var layer = layers[i]
                        if (layer && (layer.type === "video" || layer.type === "video_url")) {
                            vUrl = layer.value || ""
                            break
                        }
                    }
                }
                if (vUrl === "" && blockData.video_url)
                    vUrl = blockData.video_url

                console.log("[OVERLAYS] Слот", key, "-> video:", vUrl, "| active:", blockData.is_active)
                map[key].videoSourceUrl = vUrl
                map[key].content = blockData.content
                map[key].isActive = (blockData.is_active === undefined) ? true : !!blockData.is_active
            }
        }
    }

    Rectangle {
        id: gameLoadingOverlay
        anchors.fill: parent
        color: "#020202"
        z: 99999
        opacity: 0.0
        visible: opacity > 0.0
        Behavior on opacity { NumberAnimation { duration: 250 } }

        Image { anchors.fill: parent; source: Qt.resolvedUrl("images/hex_bg.png"); fillMode: Image.Tile; opacity: 0.25 }
        Column {
            anchors.centerIn: parent
            spacing: 30
            BusyIndicator {
                id: loadingSpinner
                anchors.horizontalCenter: parent.horizontalCenter
                running: gameLoadingOverlay.visible
                contentItem: Item {
                    implicitWidth: 64
                    implicitHeight: 64
                    RotationAnimator { target: loadingSpinnerItem; running: loadingSpinner.running; from: 0; to: 360; loops: Animation.Infinite; duration: 1200 }
                    Item {
                        id: loadingSpinnerItem
                        anchors.fill: parent
                        Shape {
                            anchors.fill: parent
                            ShapePath {
                                strokeColor: "#22c55e"
                                strokeWidth: 4
                                capStyle: ShapePath.RoundCap
                                PathAngleArc { centerX: 32; centerY: 32; radiusX: 28; radiusY: 28; startAngle: 0; sweepAngle: 280 }
                            }
                        }
                    }
                }
            }
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                Text { text: "ЗАПУСК ИГРОВОЙ СЕССИИ"; color: "#22c55e"; font.pixelSize: 24; font.bold: true; font.letterSpacing: 3; anchors.horizontalCenter: parent.horizontalCenter }
                Text {
                    id: loadingSubText
                    text: root.loadingGameTitle
                          ? ("Подготовка: " + root.loadingGameTitle)
                          : ("Платформа: " + (root.loadingPlatform || "…"))
                    color: "#666666"
                    font.pixelSize: 14
                    font.letterSpacing: 1
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }

    // Поверх Dashboard/Loader — тот же Window, не отдельный HWND
    LoadingOverlay {
        id: steamLoadingOverlay
        anchors.fill: parent
        z: 1000000
        platformName: root.loadingPlatform
        gameTitle: root.loadingGameTitle
    }
}
