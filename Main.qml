// Путь: Main.qml
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtWebSockets
import QtMultimedia
import QtQuick.Shapes
import sector0451

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
    property string pcTypeFromDatabase: (typeof NetworkManager !== "undefined" && NetworkManager.zoneSlug)
                                        ? NetworkManager.zoneSlug : "singl"
    property string zoneNameFromDatabase: (typeof NetworkManager !== "undefined")
                                          ? NetworkManager.zoneName : ""
    property int currentGameId: 0
    property bool isHardwareAdmin: false

    property bool hasActiveOrder: false
    property string orderStatusText: "В РАБОТЕ"
    property string orderStatusCode: ""
    property var orderItems: []
    property real orderItemsTotal: 0.0
    property int trackedOrderId: 0

    property string pcNameString: "PC-UNKNOWN"

    property string authErrorMessage: ""
    property bool authErrorVisible: false

    property string sessionPhone: ""
    property string sessionUserBeforePause: ""
    property bool isLoggingIn: false
    property bool gameLoadingVisible: false
    property var pendingOverlaysData: null
    property string loadingPlatform: ""
    property string loadingGameTitle: ""
    property bool quickMenuIntroduced: false
    // Видео оверлеев крутим, пока экран логина/паузы на экране.
    readonly property bool overlayPlaybackAllowed: (root.sessionUser === "GUEST"
                                                    || root.sessionUser === ""
                                                    || root.sessionUser === "PAUSE")
                                                   && screenSwitcher.item !== null
                                                   && screenSwitcher.item.visible
                                                   && !setupScreenLoader.item
                                                   && !root.gameLoadingVisible

    // Схлопываем шторм fetchOverlays (auth + terminalId + Ready + sessionUser).
    Timer {
        id: overlaysFetchDebounce
        interval: 350
        repeat: false
        property int pendingTerminalId: 0
        onTriggered: {
            if (typeof NetworkManager !== "undefined")
                NetworkManager.fetchOverlays(pendingTerminalId > 0 ? pendingTerminalId : 1)
        }
    }

    function requestOverlays(terminalId) {
        overlaysFetchDebounce.pendingTerminalId = terminalId > 0 ? terminalId : 1
        overlaysFetchDebounce.restart()
    }

    function clearGameSearchField() {
        if (typeof Launcher !== "undefined" && typeof Launcher.requestClearGameSearch === "function")
            Launcher.requestClearGameSearch()
    }

    function showGameLoading(platform, gameTitle) {
        hideGameLoadingTimer.stop()
        clearGameSearchField()
        if (typeof platform === "string" && platform.length > 0)
            root.loadingPlatform = platform
        if (typeof gameTitle === "string" && gameTitle.length > 0)
            root.loadingGameTitle = gameTitle
        steamLoadingOverlay.platformName = root.loadingPlatform || "LAUNCHER"
        steamLoadingOverlay.gameTitle = root.loadingGameTitle || "ИГРЫ"
        steamLoadingOverlay.running = true
        steamLoadingOverlay.visible = true
        root.gameLoadingVisible = true
        root.isLoggingIn = true
        if (typeof Launcher !== "undefined")
            Launcher.setShellTopmost(true)
        root.raise()
        root.requestActivate()
        // Обучение показываем один раз за пользовательскую сессию. При следующих
        // запусках остаётся только тонкая полоска у левого края.
        if (!root.quickMenuIntroduced) {
            root.quickMenuIntroduced = true
            quickMenu.openExpanded(true)
        } else {
            quickMenu.collapseToCorner()
        }
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
        root.gameLoadingVisible = false
        root.isLoggingIn = false
        // Только оверлей. Shell hide/show — C++ (hideShellForGame / showShellAfterGame /
        // showShellKeepGame|switchToShell / switchToGame). Mid-session toggle ≠ session end.
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
            root.requestOverlays(terminalId)
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

        // Адрес бэкенда берём только из config.ini через NetworkManager.
        var baseUrl = (typeof NetworkManager !== 'undefined' && NetworkManager.serverUrl)
                ? String(NetworkManager.serverUrl) : ""
        if (baseUrl.length === 0) {
            console.warn("[PAUSE] Адрес бэкенда не задан: проверьте Network/api_ip в config.ini")
            return false
        }

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
            if (typeof NetworkManager !== "undefined")
                NetworkManager.refreshBalance()
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
            root.requestOverlays(root.terminalId > 0 ? root.terminalId : 1)

            if (root.sessionUser === "PAUSE") {
                dashboardLoader.source = ""
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent
                }
            } else if (root.sessionUser === "GUEST" || root.sessionUser === "") {
                if (typeof SessionAlert !== "undefined")
                    SessionAlert.reset()
                root.sessionTime = "00:00:00"
                root.hasActiveOrder = false
                root.trackedOrderId = 0
                root.orderStatusText = "В РАБОТЕ"
                root.orderStatusCode = ""
                root.orderItems = []
                root.orderItemsTotal = 0.0
                if (!root.isLoggingIn) {
                    root.resetAuthForm()
                }
                dashboardLoader.source = ""
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent
                }
                // Музыку лобби не стартуем здесь повторно — см. lobbyStartTimer.
            }
        }
    }

    Connections {
        target: typeof SessionAlert !== "undefined" ? SessionAlert : null
        function onTimeRemainingChanged() {
            if (typeof SessionAlert !== "undefined")
                root.sessionTime = SessionAlert.timeRemaining
        }
        function onSessionExpired() {
            console.log("[SESSION] Локальный таймер истёк — выход")
            if (typeof HidMonitor !== "undefined")
                HidMonitor.stopWatch()
            if (typeof NetworkManager !== "undefined") {
                NetworkManager.stopClimateControl()
                NetworkManager.setFan("auto")
                var tid = NetworkManager.computerId > 0 ? NetworkManager.computerId : root.terminalId
                if (tid > 0)
                    NetworkManager.logoutTerminal(tid)
            }
            root.sessionUser = ""
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
            root.terminalId = NetworkManager.computerId > 0
                ? NetworkManager.computerId
                : (parseInt(root.pcNameString.replace(/[^0-9]/g, "")) || 0)
            console.log("[DEBUG-MAIN] Конец onAuthRequired. Итоговый terminalId =", root.terminalId)
            // LobbyAudio после фокуса телефона — COM/MediaPlayer не бьёт в тот же кадр, что оверлеи.
            lobbyStartTimer.restart()
            if (typeof NetworkManager !== "undefined")
                NetworkManager.requestQrChallenge(root.terminalId)
        }

        function onSessionForceEnded() {
            console.log("[POWER] Сессия закрыта на сервере — сброс UI")
            if (typeof HidMonitor !== "undefined")
                HidMonitor.stopWatch()
            if (typeof NetworkManager !== "undefined") {
                NetworkManager.stopClimateControl()
                NetworkManager.setFan("auto")
            }
            root.sessionUser = ""
        }

        function onSessionTimeUpdated(timeRemaining, sessionActive) {
            if (!sessionActive)
                return
            if (typeof SessionAlert !== "undefined" && SessionAlert.sessionActive)
                SessionAlert.syncTimeRemaining(timeRemaining)
            else if (root.sessionUser && root.sessionUser !== "GUEST" && root.sessionUser !== "")
                root.sessionTime = timeRemaining
        }

        function onLoginSucceeded(userName, balance, timeRemaining, phone) {
            lobbyStartTimer.stop()
            if (typeof NetworkManager !== "undefined")
                NetworkManager.stopQrLoginPoll()
            if (typeof Launcher !== "undefined") Launcher.applyQosPolicies(true)
            root.quickMenuIntroduced = false
            root.sessionPhone = phone
            root.sessionUser = userName
            root.sessionBalance = balance
            root.sessionTime = timeRemaining
            if (typeof SessionAlert !== "undefined")
                SessionAlert.startSession(timeRemaining)
            // Fade lobby music on speakers, then play personalized AI greeting.
            if (typeof LobbyAudio !== "undefined")
                LobbyAudio.onLoginSucceeded()
            screenSwitcher.sourceComponent = null
            dashboardLoader.source = "Dashboard.qml"
            NetworkManager.fetchGames()
            NetworkManager.fetchQuickApps()
            NetworkManager.fetchProducts()
            NetworkManager.refreshBalance()
            NetworkManager.startClimateControl()
            if (typeof HidMonitor !== "undefined") {
                var cid = NetworkManager.computerId > 0 ? NetworkManager.computerId : root.terminalId
                var bid = NetworkManager.lastBookingId || 0
                HidMonitor.captureAndBind(cid, bid)
                HidMonitor.startWatch(cid, bid)
            }
        }

        function onBalanceUpdated(balance) {
            if (typeof balance === "number" && Math.abs(root.sessionBalance - balance) >= 0.005)
                root.sessionBalance = balance
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
            // Держим оверлей дольше: Riot/League UI грузится не мгновенно
            hideGameLoadingTimer.interval =
                (String(root.loadingPlatform || "").toLowerCase().indexOf("riot") >= 0
                 || String(root.loadingGameTitle || "").toLowerCase().indexOf("league") >= 0
                 || String(root.loadingGameTitle || "").toLowerCase().indexOf("riot") >= 0)
                ? 6000 : 2500
            hideGameLoadingTimer.restart()
        }

        function onGameFinished() {
            // Не вызывать hideGameLoading→hide shell: C++ уже showShellAfterGame()
            steamLoadingOverlay.running = false
            steamLoadingOverlay.visible = false
            root.gameLoadingVisible = false
            root.isLoggingIn = false
            // restoreShellUi already requestClearGameSearch; Dashboard clears TextField
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
        }
    }

    // Poll shop order status while session is active (faster while order is open)
    Timer {
        id: orderStatusPollTimer
        interval: root.hasActiveOrder ? 5000 : 25000
        running: root.sessionUser !== "GUEST" && root.sessionUser !== "" && root.sessionUser !== "PAUSE"
                 && root.terminalId > 0
        repeat: true
        triggeredOnStart: false
        onTriggered: {
            if (typeof NetworkManager === "undefined")
                return
            if (root.hasActiveOrder || root.trackedOrderId > 0)
                NetworkManager.checkOrderStatus(root.terminalId, root.trackedOrderId)
            else
                NetworkManager.fetchProducts()
        }
    }

    // Keep wallet balance fresh during an active session (top-ups / admin credits / shop)
    Timer {
        id: balancePollTimer
        interval: 20000
        running: root.sessionUser !== "GUEST" && root.sessionUser !== "" && root.sessionUser !== "PAUSE"
                 && root.terminalId > 0
        repeat: true
        triggeredOnStart: false
        onTriggered: {
            if (typeof NetworkManager !== "undefined")
                NetworkManager.refreshBalance()
        }
    }

    // Питание: пока шелл на экране логина/паузы — периодически помечаем ПК онлайн.
    // Новый бинарник бьёт /power/heartbeat; иначе — overlays (бэкенд тоже делает touch).
    Timer {
        id: powerKeepaliveTimer
        interval: 30000
        running: root.terminalId > 0 && !setupScreenLoader.item
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            if (typeof NetworkManager === "undefined")
                return
            if (typeof NetworkManager.sendPowerHeartbeat === "function")
                NetworkManager.sendPowerHeartbeat()
            else if (typeof NetworkManager.sendPowerHeartbeat !== "function")
                root.requestOverlays(root.terminalId)
        }
    }

    onHasActiveOrderChanged: {
        if (!root.hasActiveOrder
                && (root.orderStatusText.indexOf("ВЫПОЛНЕН") >= 0
                    || root.orderStatusText.indexOf("ОТМЕН") >= 0)) {
            // Keep finished banner briefly, then clear tracker
            orderFinishedClearTimer.restart()
        }
    }

    Timer {
        id: orderFinishedClearTimer
        interval: 12000
        repeat: false
        onTriggered: {
            if (!root.hasActiveOrder) {
                root.trackedOrderId = 0
                root.orderStatusText = "В РАБОТЕ"
                root.orderStatusCode = ""
                root.orderItems = []
                root.orderItemsTotal = 0.0
            }
        }
    }

    Component.onCompleted: {
        console.log("[START-TRACE] [STEP QML-A] ...Загрузка корневого окна...")
        // Прогрев multimedia backend до экрана телефона / overlaysReady.
        mediaWarmupTimer.start()
    }

    Timer {
        id: mediaWarmupTimer
        interval: 300
        repeat: false
        onTriggered: {
            // Берём любой мелкий mp4 из кэша, иначе пропускаем.
            var candidates = [
                "file:///C:/ShellVideo/Cache/fallback_bg.mp4",
                "file:///C:/ShellVideo/lobby-ambient.wav"
            ]
            for (var i = 0; i < candidates.length; i++) {
                mediaWarmup.source = candidates[i]
                mediaWarmup.play()
                break
            }
            mediaWarmupStop.restart()
        }
    }

    Timer {
        id: mediaWarmupStop
        interval: 800
        repeat: false
        onTriggered: {
            mediaWarmup.stop()
            mediaWarmup.source = ""
        }
    }

    // Скрытый прогрев Qt Multimedia (не на экране).
    MediaPlayer {
        id: mediaWarmup
        audioOutput: AudioOutput { muted: true }
        videoOutput: mediaWarmupOut
    }
    VideoOutput {
        id: mediaWarmupOut
        width: 1
        height: 1
        visible: false
        anchors.left: parent.left
        anchors.top: parent.top
    }

    Timer {
        id: lobbyStartTimer
        interval: 1200
        repeat: false
        onTriggered: {
            if ((root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE")
                    && typeof LobbyAudio !== "undefined")
                LobbyAudio.setGuestMode(true)
        }
    }

    Binding {
        target: Theme
        property: "viewportWidth"
        value: root.width > 320 ? root.width : Theme.baseWidth
    }
    Binding {
        target: Theme
        property: "viewportHeight"
        value: root.height > 240 ? root.height : Theme.baseHeight
    }
    Binding {
        target: Theme
        property: "zoneType"
        value: root.pcTypeFromDatabase
    }

    function closeSetupScreen() {
        setupScreenLoader.source = ""
        if (!screenSwitcher.sourceComponent)
            screenSwitcher.sourceComponent = loginScreenComponent
        if (root.terminalId <= 0 && typeof NetworkManager !== "undefined")
            root.terminalId = NetworkManager.computerId
    }

    // Весь интерфейс живёт в макете 1920x1080 и масштабируется целиком.
    // 16:9 разрешения (1080p / 1440p / 4K) заполняются без полей, а вложенные
    // фиксированные размеры (сайдбар, попапы, карточки) остаются корректными.
    Item {
        id: uiRoot
        width: Theme.baseWidth
        height: Theme.baseHeight
        anchors.centerIn: parent
        transformOrigin: Item.Center
        scale: Theme.scale

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
                    root.requestOverlays(root.terminalId > 0 ? root.terminalId : 1)
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
                        openDelayMs: 0
                        playbackAllowed: root.overlayPlaybackAllowed && blockTopLeft.isActive
                    }
                    OverlayBlock {
                        id: blockMidLeft
                        blockUniqueId: "b3"
                        title: "DAT_02 / MID_LEFT"
                        width: root.blockWidth
                        height: root.blockHeight
                        fallbackVideo: root.fallbackVideo
                        openDelayMs: 500
                        playbackAllowed: root.overlayPlaybackAllowed && blockMidLeft.isActive
                    }
                    OverlayBlock {
                        id: blockBottomLeft
                        width: root.blockWidth
                        height: root.blockHeight
                        blockUniqueId: "b4"
                        title: "INF_03 / BOTTOM_LEFT"
                        fallbackVideo: root.fallbackVideo
                        openDelayMs: 1000
                        playbackAllowed: root.overlayPlaybackAllowed && blockBottomLeft.isActive
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
                        openDelayMs: 250
                        playbackAllowed: root.overlayPlaybackAllowed && blockTopRight.isActive
                    }
                    OverlayBlock {
                        id: blockMidRight
                        blockUniqueId: "b5"
                        title: "DAT_05 / MID_RIGHT"
                        width: root.blockWidth
                        height: root.blockHeight
                        fallbackVideo: root.fallbackVideo
                        openDelayMs: 750
                        playbackAllowed: root.overlayPlaybackAllowed && blockMidRight.isActive
                    }
                    OverlayBlock {
                        id: blockBottomRight
                        width: root.blockWidth
                        height: root.blockHeight
                        blockUniqueId: "b6"
                        title: "INF_06 / BOTTOM_RIGHT"
                        fallbackVideo: root.fallbackVideo
                        openDelayMs: 1250
                        playbackAllowed: root.overlayPlaybackAllowed && blockBottomRight.isActive
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
                    asynchronous: true
                    onStatusChanged: if (status === Image.Error)
                        console.warn("[BG] hex_bg load failed:", source)
                }
                // Без MultiEffect/layer: полноэкранный blur на первом кадре логина
                // стабильно подвешивал ввод телефона на несколько секунд.
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 1.0; color: "#cc000000" }
                    }
                }
            }

            FontLoader {
                id: bomberFont
                source: Qt.resolvedUrl("fonts/bomber.otf")
            }

            // Высота средней колонки = три оверлея + два spacing (как leftColumn/rightColumn).
            readonly property int overlayColumnHeight: root.blockHeight * 3 + 40

            ColumnLayout {
                id: authColumn
                width: 420
                height: loginScreen.overlayColumnHeight
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Text {
                    id: logo0451
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredHeight: 78
                    text: "0451"
                    color: Theme.accent
                    font.family: bomberFont.status === FontLoader.Ready ? bomberFont.name : "sans-serif"
                    font.pixelSize: 78
                    font.letterSpacing: 6
                    style: Text.Outline
                    styleColor: "#001a0a"
                }

                // Шапка терминала
                Rectangle {
                    id: authHeader
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    color: Theme.accentPanel
                    border.color: root.sessionUser === "PAUSE" ? Theme.infoDeep : Theme.accentBorder
                    border.width: 2
                    radius: 4
                    opacity: 0.95

                    Column {
                        anchors.centerIn: parent
                        spacing: 1
                        Text {
                            text: "TERMINAL_ID"
                            color: root.sessionUser === "PAUSE" ? Theme.info : Theme.accent
                            font.pixelSize: 10
                            opacity: 0.6
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                        Text {
                            text: root.pcNameString
                            color: "white"
                            font.pixelSize: 30
                            font.bold: true
                            anchors.horizontalCenter: parent.horizontalCenter
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton
                                onClicked: {
                                    if (mouse.modifiers & Qt.ControlModifier) {
                                        screenSwitcher.sourceComponent = null
                                        setupScreenLoader.source = "SetupScreen.qml"
                                    }
                                }
                            }
                        }
                    }
                }

                // Блок: вход по QR
                Rectangle {
                    id: qrLoginPanel
                    visible: root.sessionUser !== "PAUSE"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 220
                    color: Theme.accentPanel
                    border.color: Theme.accentBorder
                    border.width: 2
                    radius: 4
                    opacity: 0.95
                    property string qrPayload: ""
                    property string qrHint: "Загрузка QR…"
                    // Квадрат QR: почти ширина блока, но оставляет место подписи сверху/снизу.
                    readonly property int qrSide: Math.max(160, Math.min(width - 40, Math.floor(height * 0.58)))

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 0

                        Item { Layout.fillHeight: true; Layout.preferredHeight: 10 }

                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: "ВХОД ПО QR КОДУ"
                            color: Theme.accent
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 2
                        }

                        Item { Layout.preferredHeight: 10 }

                        Text {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            text: "Отсканируйте код в личном кабинете"
                            color: Theme.textSecondary
                            font.pixelSize: 11
                        }

                        Item { Layout.fillHeight: true; Layout.preferredHeight: 14 }

                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: qrLoginPanel.qrSide
                            Layout.preferredHeight: qrLoginPanel.qrSide
                            Rectangle {
                                anchors.fill: parent
                                color: "#ffffff"
                                radius: 4
                            }
                            Image {
                                anchors.centerIn: parent
                                width: parent.width - 14
                                height: parent.height - 14
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                source: qrLoginPanel.qrPayload.length
                                    ? ("https://api.qrserver.com/v1/create-qr-code/?size=320x320&data="
                                       + encodeURIComponent(qrLoginPanel.qrPayload))
                                    : ""
                            }
                        }

                        Item { Layout.fillHeight: true; Layout.preferredHeight: 14 }

                        Text {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            text: qrLoginPanel.qrHint
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }

                        Item { Layout.fillHeight: true; Layout.preferredHeight: 10 }
                    }

                    Connections {
                        target: NetworkManager
                        function onQrChallengeReady(token, qrPayload, expiresAt) {
                            qrLoginPanel.qrPayload = qrPayload
                            qrLoginPanel.qrHint = "Код обновится автоматически"
                        }
                        function onQrChallengeFailed(message) {
                            qrLoginPanel.qrHint = message || "QR недоступен"
                        }
                    }
                }

                // Блок: вход по PIN (телефон + PIN) / пауза — компактный, без fillHeight
                Rectangle {
                    id: authCenter
                    Layout.fillWidth: true
                    Layout.fillHeight: root.sessionUser === "PAUSE"
                    Layout.preferredHeight: root.sessionUser === "PAUSE" ? 260 : 238
                    Layout.maximumHeight: root.sessionUser === "PAUSE" ? 400 : 250
                    color: Theme.accentPanel
                    border.color: root.sessionUser === "PAUSE" ? Theme.infoDeep : Theme.accentBorder
                    border.width: 2
                    radius: 4
                    opacity: 0.95
                    property int authStep: 1

                    Column {
                        id: authInner
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8

                        Text {
                            visible: root.sessionUser !== "PAUSE"
                            text: "ВХОД ПО PIN КОДУ"
                            color: Theme.accent
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 2
                            anchors.horizontalCenter: parent.horizontalCenter
                        }

                        Column {
                            visible: root.sessionUser === "PAUSE"
                            width: parent.width
                            spacing: 12

                            Text { text: "ОЖИДАЮ ВОЗВРАЩЕНИЯ"; color: Theme.info; font.pixelSize: 16; font.bold: true; font.letterSpacing: 1; anchors.horizontalCenter: parent.horizontalCenter }
                            Text { text: "Введите PIN-код для разблокировки"; color: Theme.textSecondary; font.pixelSize: 11; anchors.horizontalCenter: parent.horizontalCenter }

                            TextField {
                                id: pausePinInput
                                width: parent.width
                                height: 40
                                font.pixelSize: 17
                                font.family: "Roboto"
                                font.letterSpacing: 4
                                inputMask: "0000;_"
                                echoMode: TextInput.Normal
                                color: "white"
                                selectionColor: Theme.info
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
                                background: Rectangle { color: pausePinInput.activeFocus ? Theme.infoSurface : Theme.infoSurfaceIdle; border.color: pausePinInput.activeFocus ? Theme.info : Theme.infoDeep; border.width: pausePinInput.activeFocus ? 2 : 1; radius: 4 }
                            }
                            Text { id: pauseErrorText; text: "Неверный PIN-код"; visible: false; color: Theme.danger; font.pixelSize: 12; anchors.horizontalCenter: parent.horizontalCenter }
                            Button {
                                id: resumePauseBtn
                                width: parent.width
                                height: 44
                                text: "Я ВЕРНУЛСЯ"
                                scale: resumePauseBtn.down ? 0.96 : 1.0
                                Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                contentItem: Text {
                                    text: resumePauseBtn.text
                                    color: resumePauseBtn.hovered ? "#020202" : Theme.info
                                    font.pixelSize: 14
                                    font.bold: true
                                    font.letterSpacing: 2
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: 4
                                    color: resumePauseBtn.down ? Theme.infoDeep : (resumePauseBtn.hovered ? Theme.info : Theme.infoSurfaceIdle)
                                    border.color: resumePauseBtn.hovered ? Theme.info : Theme.infoDeep
                                    border.width: resumePauseBtn.hovered ? 2 : 1
                                }
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
                            id: authFormCol
                            visible: root.sessionUser !== "PAUSE"
                            width: parent.width
                            spacing: 0

                            Column {
                                visible: authCenter.authStep === 1
                                width: parent.width
                                spacing: 0
                                Text { text: "НОМЕР ТЕЛЕФОНА"; color: Theme.accent; font.pixelSize: 9; font.bold: true; font.letterSpacing: 2; anchors.horizontalCenter: parent.horizontalCenter }
                                Item { width: 1; height: Math.round(authActionBtn.height * 0.5) }
                                TextField {
                                    id: phoneInput
                                    width: parent.width
                                    height: 36
                                    font.pixelSize: 16
                                    font.family: "Roboto"
                                    font.letterSpacing: 1
                                    inputMask: "+7 (999) 999-99-99;_"
                                    focus: authCenter.authStep === 1 && root.sessionUser !== "PAUSE"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: "white"
                                    selectionColor: Theme.accent
                                    selectedTextColor: "black"
                                    Timer { id: focusTimer; interval: 50; running: false; repeat: false; onTriggered: { if (root.sessionUser !== "PAUSE" && authCenter.authStep === 1) { phoneInput.forceActiveFocus(); phoneInput.cursorPosition = 4 } } }
                                    Component.onCompleted: { if (root.sessionUser !== "PAUSE") focusTimer.start() }
                                    onVisibleChanged: { if (visible && authCenter.authStep === 1 && root.sessionUser !== "PAUSE") focusTimer.start() }
                                    onActiveFocusChanged: { if (activeFocus && (text === "+7 (   )   -  -  " || text === "")) { Qt.callLater(function() { phoneInput.cursorPosition = 4 }) } }
                                    onAccepted: {
                                        if (!phoneInput.acceptableInput) {
                                            root.authErrorMessage = "Введите номер телефона"
                                            root.authErrorVisible = true
                                            return
                                        }
                                        authCenter.authStep = 2
                                    }
                                    background: Rectangle { color: phoneInput.activeFocus ? Theme.accentSurface : Theme.accentSurfaceIdle; border.color: phoneInput.activeFocus ? Theme.accent : Theme.accentBorder; border.width: phoneInput.activeFocus ? 2 : 1; radius: 4 }
                                }
                            }

                            Column {
                                visible: authCenter.authStep === 2
                                width: parent.width
                                spacing: 0
                                Text { text: "PIN-КОД"; color: Theme.accent; font.pixelSize: 9; font.bold: true; font.letterSpacing: 2; anchors.horizontalCenter: parent.horizontalCenter }
                                Item { width: 1; height: Math.round(authActionBtn.height * 0.5) }
                                TextField {
                                    id: pinInput
                                    width: parent.width
                                    height: 36
                                    font.pixelSize: 16
                                    font.family: "Roboto"
                                    font.letterSpacing: 4
                                    inputMask: "0000;_"
                                    echoMode: TextInput.Normal
                                    focus: authCenter.authStep === 2 && root.sessionUser !== "PAUSE"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: TextInput.AlignVCenter
                                    color: "white"
                                    selectionColor: Theme.accent
                                    selectedTextColor: "black"
                                    onActiveFocusChanged: { if (activeFocus) { Qt.callLater(function() { pinInput.cursorPosition = 0 }) } }
                                    onAccepted: {
                                        if (root.isLoggingIn)
                                            return
                                        root.authErrorVisible = false
                                        root.isLoggingIn = true
                                        NetworkManager.login(phoneInput.text, pinInput.text, parseInt(root.terminalId))
                                    }
                                    background: Rectangle { color: pinInput.activeFocus ? Theme.accentSurface : Theme.accentSurfaceIdle; border.color: pinInput.activeFocus ? Theme.accent : Theme.accentBorder; border.width: pinInput.activeFocus ? 2 : 1; radius: 4 }
                                }
                            }

                            Text {
                                id: authErrorText
                                text: root.authErrorMessage
                                visible: root.authErrorVisible
                                color: Theme.danger
                                font.bold: true
                                font.pixelSize: 12
                                anchors.horizontalCenter: parent.horizontalCenter
                                Connections { target: pinInput; function onTextChanged() { root.authErrorVisible = false } }
                                Connections { target: phoneInput; function onTextChanged() { root.authErrorVisible = false } }
                            }

                            // Зазор = половина высоты кнопки «ДАЛЕЕ».
                            Item {
                                width: 1
                                height: Math.round(authActionBtn.height * 0.5)
                            }

                            Rectangle {
                                id: authActionBtn
                                width: parent.width
                                height: 40
                                radius: 4
                                color: {
                                    if (authBtnMouse.pressed)
                                        return Theme.accentPressed
                                    if (authBtnMouse.containsMouse)
                                        return Theme.accentDeep
                                    return Theme.accent
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
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.letterSpacing: 2
                                }

                                Item {
                                    id: authWaitClock
                                    anchors.centerIn: parent
                                    width: 26
                                    height: 26
                                    visible: authActionBtn.waitingLogin
                                    Item {
                                        id: authWaitSpinnerItem
                                        anchors.fill: parent
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
                                    RotationAnimator {
                                        id: authWaitSpin
                                        target: authWaitSpinnerItem
                                        from: 0
                                        to: 360
                                        duration: 750
                                        loops: Animation.Infinite
                                        running: authActionBtn.waitingLogin
                                        easing.type: Easing.Linear
                                    }
                                    Connections {
                                        target: authActionBtn
                                        function onWaitingLoginChanged() {
                                            if (authActionBtn.waitingLogin) {
                                                authWaitSpinnerItem.rotation = 0
                                                authWaitSpin.restart()
                                            } else {
                                                authWaitSpin.stop()
                                            }
                                        }
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
                                            if (!phoneInput.acceptableInput) {
                                                root.authErrorMessage = "Введите номер телефона"
                                                root.authErrorVisible = true
                                                phoneInput.forceActiveFocus()
                                                return
                                            }
                                            authCenter.authStep = 2
                                        } else {
                                            root.authErrorVisible = false
                                            root.isLoggingIn = true
                                            NetworkManager.login(phoneInput.text, pinInput.text, parseInt(root.terminalId))
                                        }
                                    }
                                }
                            }

                            Item {
                                visible: authCenter.authStep === 2
                                width: 1
                                height: 10
                            }

                            Text {
                                id: backBtn
                                text: "НАЗАД"
                                font.pixelSize: 12
                                font.bold: true
                                font.letterSpacing: 2
                                color: backMouse.containsMouse ? Theme.accent : Theme.textMuted
                                visible: authCenter.authStep === 2
                                anchors.horizontalCenter: parent.horizontalCenter
                                Behavior on color { ColorAnimation { duration: 100 } }
                                MouseArea {
                                    id: backMouse
                                    anchors.fill: parent
                                    anchors.margins: -10
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
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

                // Часы
                Rectangle {
                    id: authClock
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    Layout.maximumHeight: 92
                    color: Theme.accentPanel
                    border.color: root.sessionUser === "PAUSE" ? Theme.infoDeep : Theme.accentBorder
                    border.width: 2
                    radius: 4
                    opacity: 0.95

                    readonly property color tint: root.sessionUser === "PAUSE" ? Theme.info : Theme.accent
                    property date now: new Date()

                    Timer {
                        interval: 1000
                        running: true
                        repeat: true
                        triggeredOnStart: true
                        onTriggered: authClock.now = new Date()
                    }

                    Column {
                        anchors.centerIn: parent
                        spacing: 3

                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 4
                            Text {
                                text: Qt.formatTime(authClock.now, "HH")
                                color: "white"
                                font.pixelSize: 38
                                font.bold: true
                                font.family: "Consolas"
                                font.letterSpacing: 2
                            }
                            Text {
                                text: ":"
                                color: authClock.tint
                                font.pixelSize: 38
                                font.bold: true
                                font.family: "Consolas"
                                opacity: authClock.now.getSeconds() % 2 === 0 ? 1.0 : 0.25
                                Behavior on opacity { NumberAnimation { duration: 320 } }
                            }
                            Text {
                                text: Qt.formatTime(authClock.now, "mm")
                                color: "white"
                                font.pixelSize: 38
                                font.bold: true
                                font.family: "Consolas"
                                font.letterSpacing: 2
                            }
                            Text {
                                text: Qt.formatTime(authClock.now, "ss")
                                color: authClock.tint
                                font.pixelSize: 18
                                font.bold: true
                                font.family: "Consolas"
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 4
                            }
                        }

                        Rectangle {
                            width: authClock.width - 80
                            height: 1
                            color: authClock.tint
                            opacity: 0.3
                            anchors.horizontalCenter: parent.horizontalCenter
                        }

                        Text {
                            text: Qt.formatDate(authClock.now, "dddd, d MMMM yyyy").toUpperCase()
                            color: Theme.textSecondary
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 2
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }
                }
            } // authColumn
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

    // Быстрое меню доп.софта: отдельный HWND (живёт поверх игры).
    QuickMenu {
        id: quickMenu
    }

    // Hold-to-talk: красная точка (bottom-right), отдельный HWND.
    VoiceIndicator {
        id: voiceIndicator
    }

    // Полоска / панель только пока идёт загрузка или шелл спрятан под игру.
    // На дашборде без игры не мешаем.
    Connections {
        target: typeof Launcher !== "undefined" ? Launcher : null
        function onShellHiddenForGameChanged() {
            root.syncQuickMenu()
        }
        function onHasActiveGameChanged() {
            root.syncQuickMenu()
        }
    }

    function syncQuickMenu() {
        var loading = root.gameLoadingVisible
        var overGame = typeof Launcher !== "undefined"
                && Launcher.hasActiveGame
                && Launcher.shellHiddenForGame

        if (loading) {
            // Уже открыли в showGameLoading; если гость закрыл — оставляем полоску.
            if (!quickMenu.visible)
                quickMenu.collapseToCorner()
            return
        }

        if (overGame) {
            // Не сворачиваем, если гость уже держит панель открытой.
            if (!quickMenu.visible)
                quickMenu.collapseToCorner()
            else
                quickMenu.reassertTopmost()
            return
        }

        quickMenu.hideAll()
    }

    onGameLoadingVisibleChanged: root.syncQuickMenu()
    // ^ конец uiRoot

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

                var nextActive = (blockData.is_active === undefined) ? true : !!blockData.is_active
                if (map[key].videoSourceUrl === vUrl && map[key].isActive === nextActive)
                    continue

                console.log("[OVERLAYS] Слот", key, "-> video:", vUrl, "| active:", nextActive)
                map[key].videoSourceUrl = vUrl
                map[key].content = blockData.content
                map[key].isActive = nextActive
            }
        }
    }

}
