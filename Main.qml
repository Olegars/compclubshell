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
    visibility: Window.FullScreen

    property int terminalId: 0
    property string sessionUser: "GUEST"
    property real sessionBalance: 0.0
    property string sessionTime: "00:00:00"
    property alias authScreen: screenSwitcher
    property string temporaryPausePin: "----"
    property string pcTypeFromDatabase: "standard"

    property bool isHardwareAdmin: false

    property bool hasActiveOrder: false
    property string orderStatusText: "ЗАКАЗ В РАБОТЕ"

    property string pcNameString: "PC-UNKNOWN"

    property string authErrorMessage: ""
    property bool authErrorVisible: false

    property string sessionPhone: ""

    property bool isLoggingIn: false

    readonly property string fallbackVideo: "file:///C:/ShellVideo/Cache/fallback_bg.mp4"

    readonly property int blockWidth: 524
    readonly property int blockHeight: 295
    readonly property int sideMargin: 50

    onTerminalIdChanged: {
        console.log("[DEBUG-MAIN] ТРИГГЕР: terminalId изменился на:", terminalId)
        if (terminalId > 0) {
            console.log("[START-TRACE] [QML-REACTION] Железо авторизовано. ID:", terminalId, ". Начинаем загрузку оверлеев...")
            fetchOverlays()
        }
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

    onSessionUserChanged: {
        console.log("[DEBUG-MAIN] ТРИГГЕР: sessionUser изменился на:", root.sessionUser)
        if (root.sessionUser === "PAUSE" || root.sessionUser === "GUEST" || root.sessionUser === "") {
            console.log("[SHELL-STATUS] Сессия изменилась. Запрос фоновых оверлеев...")
            root.fetchOverlays()

            if (root.sessionUser === "PAUSE") {
                console.log("[LIFECYCLE-MAIN] Активация режима Паузы. Блокируем интерфейс.")
                dashboardLoader.source = ""
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent
                }
            } else if (root.sessionUser === "GUEST" || root.sessionUser === "") {
                root.resetAuthForm()
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
            root.terminalId = root.pcNameString.replace(/[^0-9]/g, "") || 0
            console.log("[DEBUG-MAIN] Конец onAuthRequired. Итоговый terminalId =", root.terminalId)
        }
    }

    // ГЛOБАЛЬНЫЙ КОНТРОЛЬ КРУТИЛКИ В КОРНЕ ШЕЛЛА
        Connections {
            target: Launcher

            function onGameStarted() {
                console.log("[MAIN-LIFECYCLE] Процесс стартовал в ОС. Запуск 10-секундного страховочного таймера...")
                hideDelayTimer.start()
            }

            function onGameFinished() {
                console.log("[MAIN-LIFECYCLE] Сессия игры завершена. Освобождаем Steam-аккаунт в БД Laravel через POST...")
                hideDelayTimer.stop()
                root.isLoggingIn = false

                // Вычисляем базовый URL бэкенда динамически
                var baseUrl = (typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : NetworkManager.serverUrl
                var xhr = new XMLHttpRequest()

                // Стучимся строго через POST по правилам роутинга Laravel web.php
                var targetPostUrl = baseUrl + "/api/shell/games/free-account"

                xhr.open("POST", targetPostUrl)
                // Обязательно выставляем заголовок JSON, иначе Laravel не поймет тело запроса
                xhr.setRequestHeader("Content-Type", "application/json")

                xhr.onreadystatechange = function() {
                    if (xhr.readyState === XMLHttpRequest.DONE) {
                        console.log("[MAIN-LIFECYCLE] Ответ бэкенда на освобождение аккаунта. Статус HTTP:", xhr.status)
                        console.log("[MAIN-LIFECYCLE] Текст ответа бэкенда:", xhr.responseText)
                    }
                }

                // Упаковываем terminal_id в JSON-строку
                var payload = {
                    "terminal_id": parseInt(root.terminalId)
                }

                xhr.send(JSON.stringify(payload))
            }
        }

    // СТРАХОВОЧНЫЙ ТАЙМЕР-ПАРАШЮТ (10 СЕКУНД)
    Timer {
        id: hideDelayTimer
        interval: 10000
        running: false
        repeat: false
        onTriggered: {
            console.log("[MAIN-LIFECYCLE] Страховочный таймер сработал. Аварийное закрытие оверлея.")
            root.isLoggingIn = false
            root.hide()
        }
    }

    Component.onCompleted: {
        console.log("[START-TRACE] [STEP QML-A] Корневой Window загрузился. Собираем HWID...")
        var nativeHwid = "e41057e5-9695-440b-9750-94dcd95263a0"
        NetworkManager.fetchTerminalConfig(nativeHwid)
        console.log("[START-TRACE] [STEP QML-B] HWID передан. Запуск проверки статуса терминала...")
        NetworkManager.checkTerminalStatus()
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

        onActiveChanged: console.log("[DEBUG-CONTAINER] Свойство active для overlaysContainer изменилось на:", active)

        onStatusChanged: {
            if (status === Loader.Ready) {
                root.fetchOverlays()
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
                    }
                    OverlayBlock {
                        id: blockMidLeft
                        blockUniqueId: "b3"
                        title: "DAT_02 / MID_LEFT"
                        width: root.blockWidth
                        height: root.blockHeight
                    }
                    OverlayBlock {
                        id: blockBottomLeft
                        width: root.blockWidth
                        height: root.blockHeight
                        blockUniqueId: "b4"
                        title: "INF_03 / BOTTOM_LEFT"
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
                    }
                    OverlayBlock {
                        id: blockMidRight
                        blockUniqueId: "b5"
                        title: "DAT_05 / MID_RIGHT"
                        width: root.blockWidth
                        height: root.blockHeight
                    }
                    OverlayBlock {
                        id: blockBottomRight
                        width: root.blockWidth
                        height: root.blockHeight
                        blockUniqueId: "b6"
                        title: "INF_06 / BOTTOM_RIGHT"
                    }
                }
            }
        }
    }

    Loader {
        id: dashboardLoader
        anchors.fill: parent
        source: ""
        visible: dashboardLoader.status === Loader.Ready
        z: 10
    }

    Loader {
        id: screenSwitcher
        anchors.fill: parent
        z: 20
    }

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
                    source: "images/hex_bg.png"
                    fillMode: Image.Tile
                    opacity: 0.15
                }
                Shape {
                    anchors.fill: parent
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blur: 0.3
                    }
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
                    Text {
                        text: "REACTOR"
                        color: "#000"
                        font.pixelSize: 80
                        style: Text.Outline
                        styleColor: "#22c55e"
                    }
                    Text {
                        text: "0 4 5 1"
                        color: "#22c55e"
                        font.pixelSize: 60
                        font.bold: true
                        opacity: 0.8
                    }
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
                        Text {
                            text: "TERMINAL_ID"
                            color: root.sessionUser === "PAUSE" ? "#3b82f6" : "#22c55e"
                            font.pixelSize: 12
                            opacity: 0.6
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
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
                                onClicked: {
                                    if (mouse.modifiers & Qt.ControlModifier) {
                                        screenSwitcher.sourceComponent = null
                                        setupScreenLoader.source = "SetupScreen.qml"
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: root.sessionUser === "PAUSE" ? "#3b82f6" : "#22c55e"
                        opacity: 0.3
                    }

                    Item {
                        width: parent.width
                        height: 230

                        Column {
                            visible: root.sessionUser === "PAUSE"
                            width: parent.width
                            spacing: 15

                            Text {
                                text: "ОЖИДАЮ ВОЗВРАЩЕНИЯ"
                                color: "#3b82f6"
                                font.pixelSize: 20
                                font.bold: true
                                font.letterSpacing: 1
                                anchors.horizontalCenter: parent.horizontalCenter
                            }
                            Text {
                                text: "Введите PIN-код для разблокировки"
                                color: "#a3a3a3"
                                font.pixelSize: 12
                                anchors.horizontalCenter: parent.horizontalCenter
                            }

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

                                Timer {
                                    id: pauseFocusTimer
                                    interval: 80
                                    running: false
                                    repeat: false
                                    onTriggered: {
                                        pausePinInput.forceActiveFocus()
                                        pausePinInput.cursorPosition = 0
                                    }
                                }
                                Component.onCompleted: {
                                    if (root.sessionUser === "PAUSE") pauseFocusTimer.start()
                                }
                                onVisibleChanged: {
                                    if (visible && root.sessionUser === "PAUSE") pauseFocusTimer.start()
                                }
                                onAccepted: {
                                    var cleanPin = pausePinInput.text.trim().replace(/[^0-9]/g, "")
                                    if (cleanPin !== "") {
                                        root.loginToServer(root.sessionPhone, pausePinInput.text)
                                        pausePinInput.text = ""
                                    }
                                }
                                background: Rectangle {
                                    color: pausePinInput.activeFocus ? "#08162a" : "#0d1117"
                                    border.color: pausePinInput.activeFocus ? "#3b82f6" : "#1d4ed8"
                                    border.width: pausePinInput.activeFocus ? 2 : 1
                                    radius: 4
                                    layer.enabled: pausePinInput.activeFocus
                                    layer.effect: MultiEffect {
                                        blurEnabled: true
                                        blur: 0.2
                                        brightness: 0.1
                                    }
                                }
                            }

                            Text {
                                id: pauseErrorText
                                text: root.authErrorMessage !== "" ? root.authErrorMessage : "Неверный PIN-код"
                                visible: root.authErrorVisible
                                color: "#ef4444"
                                font.pixelSize: 12
                                anchors.horizontalCenter: parent.horizontalCenter
                            }

                            Button {
                                width: parent.width
                                height: 50
                                text: "Я ВЕРНУЛСЯ"
                                onClicked: {
                                    var cleanPin = pausePinInput.text.trim().replace(/[^0-9]/g, "")
                                    if (cleanPin !== "") {
                                        root.loginToServer(root.sessionPhone, pausePinInput.text)
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
                                Text {
                                    text: "НОМЕР ТЕЛЕФОНА"
                                    color: "#22c55e"
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 2
                                    anchors.horizontalCenter: parent.horizontalCenter
                                }
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

                                    Timer {
                                        id: focusTimer
                                        interval: 50
                                        running: false
                                        repeat: false
                                        onTriggered: {
                                            if (root.sessionUser !== "PAUSE" && authCenter.authStep === 1) {
                                                phoneInput.forceActiveFocus()
                                                phoneInput.cursorPosition = 4
                                            }
                                        }
                                    }

                                    Component.onCompleted: {
                                        if (root.sessionUser !== "PAUSE") focusTimer.start()
                                    }
                                    onVisibleChanged: {
                                        if (visible && authCenter.authStep === 1 && root.sessionUser !== "PAUSE") {
                                            focusTimer.start()
                                        }
                                    }
                                    onActiveFocusChanged: {
                                        if (activeFocus && (text === "+7 (   )    -  -  " || text === "") && root.sessionUser !== "PAUSE") {
                                            Qt.callLater(function() {
                                                phoneInput.cursorPosition = 4
                                            })
                                        }
                                    }
                                    onAccepted: {
                                        authCenter.authStep = 2
                                    }
                                    background: Rectangle {
                                        color: phoneInput.activeFocus ? "#08120a" : "#0d130e"
                                        border.color: phoneInput.activeFocus ? "#22c55e" : "#1a4d29"
                                        border.width: phoneInput.activeFocus ? 2 : 1
                                        radius: 4
                                        layer.enabled: phoneInput.activeFocus
                                        layer.effect: MultiEffect {
                                            blurEnabled: true
                                            blur: 0.2
                                            brightness: 0.1
                                        }
                                    }
                                }
                            }

                            Column {
                                visible: authCenter.authStep === 2
                                width: parent.width
                                spacing: 12
                                Text {
                                    text: "PIN-КОД"
                                    color: "#22c55e"
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 2
                                    anchors.horizontalCenter: parent.horizontalCenter
                                }
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

                                    onActiveFocusChanged: {
                                        if (activeFocus) {
                                            Qt.callLater(function() {
                                                pinInput.cursorPosition = 0
                                            })
                                        }
                                    }
                                    onAccepted: {
                                        loginToServer(phoneInput.text, pinInput.text)
                                    }
                                    background: Rectangle {
                                        color: pinInput.activeFocus ? "#08120a" : "#0d130e"
                                        border.color: pinInput.activeFocus ? "#22c55e" : "#1a4d29"
                                        border.width: pinInput.activeFocus ? 2 : 1
                                        radius: 4
                                        layer.enabled: pinInput.activeFocus
                                        layer.effect: MultiEffect {
                                            blurEnabled: true
                                            blur: 0.2
                                            brightness: 0.1
                                        }
                                    }
                                }
                            }

                            // СТРОКА 211 ФИКСИРОВАНА: Построчно разделены Connections, полностью убраны ";"
                            Text {
                                id: authErrorText
                                text: root.authErrorMessage
                                visible: root.authErrorVisible
                                color: "#ef4444"
                                font.bold: true
                                anchors.horizontalCenter: parent.horizontalCenter

                                Connections {
                                    target: pinInput
                                    function onTextChanged() {
                                        root.authErrorVisible = false
                                    }
                                }

                                Connections {
                                    target: phoneInput
                                    function onTextChanged() {
                                        root.authErrorVisible = false
                                    }
                                }
                            }

                            Item {
                                height: 5
                                width: 1
                            }

                            Button {
                                width: parent.width
                                height: 55
                                text: authCenter.authStep === 1 ? "ДАЛЕЕ" : "НАЧАТЬ СЕССИЮ"
                                onClicked: {
                                    if (authCenter.authStep === 1) {
                                        authCenter.authStep = 2
                                    } else {
                                        loginToServer(phoneInput.text, pinInput.text)
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

                                Behavior on color {
                                    ColorAnimation { duration: 100 }
                                }

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
            }
        }
    }

    function loginToServer(phone, pin) {
        console.log("[TRACE-AUTH] === СТАРТ ПРОЦЕССА АВТОРИЗАЦИИ ===")
        if (typeof NetworkManager === "undefined") return
        var baseUrl = (typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : NetworkManager.serverUrl
        var cleanPhone = phone.replace(/[^0-9]/g, "")
        var cleanPin = pin.replace(/[^0-9]/g, "")
        var targetTerminalId = parseInt(root.terminalId)

        if (cleanPhone === "" || cleanPin === "") return
        var targetUrl = baseUrl + "/api/shell/login"
        root.isLoggingIn = true

        var xhr = new XMLHttpRequest()
        xhr.open("POST", targetUrl)
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    try {
                        var response = JSON.parse(xhr.responseText)
                        if (response.status === "success") {
                            if (typeof Launcher !== "undefined") Launcher.applyQosPolicies(true)
                            root.sessionPhone = cleanPhone
                            root.sessionUser = response.user.name || "GUEST"
                            root.sessionBalance = parseFloat(response.user.balance) || 0
                            root.sessionTime = response.user.time_remaining || "00:00:00"
                            root.isLoggingIn = false
                            screenSwitcher.sourceComponent = null
                            dashboardLoader.source = "Dashboard.qml"
                            if (NetworkManager !== null) {
                                NetworkManager.fetchGames()
                                NetworkManager.fetchProducts()
                            }
                        } else {
                            root.isLoggingIn = false
                        }
                    } catch (e) {
                        root.isLoggingIn = false
                    }
                } else {
                    root.isLoggingIn = false
                    root.authErrorMessage = "Ошибка входа"
                    root.authErrorVisible = true
                }
            }
        }
        xhr.send(JSON.stringify({ "phone": cleanPhone, "pin": cleanPin, "terminal_id": targetTerminalId }))
    }

    function fetchOverlays() {
        if (typeof NetworkManager === "undefined" || root.terminalId === 0) return
        var baseUrl = (typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : NetworkManager.serverUrl
        var xhr = new XMLHttpRequest()
        xhr.open("GET", baseUrl + "/api/shell/overlays?terminal_id=" + root.terminalId + "&t=" + new Date().getTime())
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                try {
                    updateOverlaysToScreen(JSON.parse(xhr.responseText))
                } catch(e) {}
            }
        }
        xhr.send()
    }

    function updateOverlaysToScreen(response) {
        var actualData = response.data ? response.data : response
        if (overlaysContainer.status !== Loader.Ready || !overlaysContainer.item) return
        var item = overlaysContainer.item
        var map = {
            "top_left": item.b1,
            "top_right": item.b2,
            "mid_left": item.b3,
            "mid_right": item.b5,
            "bottom_left": item.b4,
            "bottom_right": item.b6
        }

        for (var key in map) {
            if (actualData[key] && map[key]) {
                var vUrl = ""
                var blockData = actualData[key]
                if (blockData.content && blockData.content.layers && Array.isArray(blockData.content.layers)) {
                    for (var i = 0; i < blockData.content.layers.length; i++) {
                        if (blockData.content.layers[i].type === "video" || blockData.content.layers[i].type === "video_url") {
                            vUrl = blockData.content.layers[i].value || ""
                            break
                        }
                    }
                }
                if (vUrl === "" && blockData.video_url) vUrl = blockData.video_url
                map[key].videoSourceUrl = vUrl
                map[key].content = blockData.content
                map[key].isActive = blockData.is_active
            }
        }
    }

    // ГЛOБАЛЬНЫЙ НЕОНОВЫЙ ОВЕРЛЕЙ ЗАГРУЗКИ: ДУГА КРУТИТСЯ БЕЗ СТОРОННИХ КВАДРАТНЫХ РАМОК
    Rectangle {
        anchors.fill: parent
        color: "#cc020202"
        visible: root.isLoggingIn
        z: 99999

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 20

            Item {
                id: customSpinnerItem
                Layout.alignment: Qt.AlignHCenter
                width: 60
                height: 60

                Shape {
                    anchors.fill: parent
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        blurEnabled: true
                        blur: 0.15
                        brightness: 0.1
                    }

                    ShapePath {
                        strokeColor: root.sessionUser === "PAUSE" ? "#3b82f6" : "#22c55e"
                        strokeWidth: 4
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap

                        PathAngleArc {
                            centerX: 30
                            centerY: 30
                            radiusX: 26
                            radiusY: 26
                            startAngle: 0
                            sweepAngle: 280
                        }
                    }
                }

                RotationAnimator {
                    target: customSpinnerItem
                    running: root.isLoggingIn
                    from: 0
                    to: 360
                    loops: Animation.Infinite
                    duration: 900
                }
            }

            Text {
                text: root.sessionUser === "PAUSE" ? "ПРОВЕРКА ПИН-КОДА..." :
                      (dashboardLoader.status === Loader.Ready ? "ПОДГОТОВКА ИГРЫ..." : "ЗАПУСК СЕКТOРА...")
                color: root.sessionUser === "PAUSE" ? "#3b82f6" : "#22c55e"
                font.pixelSize: 14
                font.bold: true
                font.letterSpacing: 2
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }
}