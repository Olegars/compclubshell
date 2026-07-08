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

    // ГЛОБАЛЬНЫЕ СВОЙСТВА ДЛЯ УПРАВЛЕНИЯ ОШИБКОЙ АВТОРИЗАЦИИ
    property string authErrorMessage: ""
    property bool authErrorVisible: false

    // ФЛАГ АКТИВНОСТИ ИНДИКАТОРА ЗАГРУЗКИ
    property bool isLoggingIn: false

    readonly property string fallbackVideo: "file:///C:/ShellVideo/Cache/fallback_bg.mp4"

    readonly property int blockWidth: 524
    readonly property int blockHeight: 295
    readonly property int sideMargin: 50

    onTerminalIdChanged: {
        console.log("[DEBUG-MAIN] ТРИГГЕР: terminalId изменился на:", terminalId);
        if (terminalId > 0) {
            console.log("[START-TRACE] [QML-REACTION] Железо авторизовано. ID:", terminalId, ". Начинаем загрузку оверлеев...");
            fetchOverlays();
        }
    }

    function resetAuthForm() {
        if (typeof authCenter !== 'undefined' && authCenter !== null) {
            authCenter.authStep = 1;
        }

        if (typeof phoneInput !== 'undefined' && phoneInput !== null) {
            phoneInput.text = "";
        }
        if (typeof pinInput !== 'undefined' && pinInput !== null) {
            pinInput.text = "";
        }

        root.authErrorVisible = false;
        root.authErrorMessage = "";
        root.isLoggingIn = false;

        // Принудительный сброс фокуса на телефон при логауте
        if (typeof phoneInput !== 'undefined' && phoneInput !== null) {
            phoneInput.forceActiveFocus();
            phoneInput.cursorPosition = 4;
        }
    }

    onSessionUserChanged: {
        console.log("[DEBUG-MAIN] ТРИГГЕР: sessionUser изменился на:", root.sessionUser);

        if (root.sessionUser === "PAUSE" || root.sessionUser === "GUEST" || root.sessionUser === "") {
            console.log("[SHELL-STATUS] Сессия изменилась. Запрос фоновых оверлеев...");
            root.fetchOverlays();

            // 1. ЕСЛИ ЭТО РЕЖИМ ПАУЗЫ (БЛОКИРОВКА ЭКРАНА)
            if (root.sessionUser === "PAUSE") {
                console.log("[LIFECYCLE-MAIN] Активация режима Паузы. Блокируем интерфейс.");
                dashboardLoader.source = "";
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent;
                }
            }
            // 2. ЕСЛИ ЭТО ПОЛНЫЙ ВЫХОД (СВОБОДНЫЙ СТЕНД)
            else if (root.sessionUser === "GUEST" || root.sessionUser === "") {
                root.resetAuthForm();
                dashboardLoader.source = "";
                if (screenSwitcher.sourceComponent !== loginScreenComponent) {
                    screenSwitcher.sourceComponent = loginScreenComponent;
                }
            }
        }
    }

    Connections {
        target: NetworkManager

        function onSetupRequired() {
            console.log("[REACTOR-SHELL] Получен сигнал setupRequired. Переключение на SetupScreen.qml");
            screenSwitcher.sourceComponent = null;
            setupScreenLoader.source = "SetupScreen.qml";
        }

        function onAuthRequired() {
            console.log("[REACTOR-SHELL] Получен сигнал authRequired. Загрузка AuthScreen.");
            root.pcNameString = NetworkManager.getCurrentPcName();
            setupScreenLoader.source = "";
            screenSwitcher.sourceComponent = loginScreenComponent;
            root.terminalId = root.pcNameString.replace(/[^0-9]/g, "") || 0;
            console.log("[DEBUG-MAIN] Конец onAuthRequired. Итоговый terminalId =", root.terminalId);
        }
    }

    Component.onCompleted: {
        console.log("[START-TRACE] [STEP QML-A] Корневой Window загрузился. Собираем HWID...");
        var nativeHwid = "e41057e5-9695-440b-9750-94dcd95263a0";
        NetworkManager.fetchTerminalConfig(nativeHwid);
        console.log("[START-TRACE] [STEP QML-B] HWID передан. Запуск проверки статуса терминала...");
        NetworkManager.checkTerminalStatus();
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

        onActiveChanged: console.log("[DEBUG-CONTAINER] Свойство active для overlaysContainer изменилось на:", active);

        onStatusChanged: {
            console.log("[DEBUG-CONTAINER] Статус overlaysContainer изменился:", status, "(1-Loading, 2-Ready, 3-Error, 0-Null)");
            if (status === Loader.Ready) {
                console.log("[LIFECYCLE] overlaysContainer полностью инициализирован в памяти. Повторный запрос сетки оверлеев...");
                root.fetchOverlays();
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

                Component.onCompleted: console.log("[DEBUG-CONTAINER] Внутренний Item со слотами блогов b1-b6 УСПЕШНО создан в памяти!");

                Column {
                    id: leftColumn
                    anchors.left: parent.left
                    anchors.leftMargin: root.sideMargin
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 20
                    OverlayBlock { id: blockTopLeft; blockUniqueId: "b1"; title: "CAM_01 / TOP_LEFT"; width: root.blockWidth; height: root.blockHeight }
                    OverlayBlock { id: blockMidLeft; blockUniqueId: "b3"; title: "DAT_02 / MID_LEFT"; width: root.blockWidth; height: root.blockHeight }
                    OverlayBlock { id: blockBottomLeft; blockUniqueId: "b4"; title: "INF_03 / BOTTOM_LEFT"; width: root.blockWidth; height: root.blockHeight }
                }

                Column {
                    id: rightColumn
                    anchors.right: parent.right
                    anchors.rightMargin: root.sideMargin
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 20
                    OverlayBlock { id: blockTopRight; blockUniqueId: "b2"; title: "CAM_04 / TOP_RIGHT"; width: root.blockWidth; height: root.blockHeight }
                    OverlayBlock { id: blockMidRight; blockUniqueId: "b5"; title: "DAT_05 / MID_RIGHT"; width: root.blockWidth; height: root.blockHeight }
                    OverlayBlock { id: blockBottomRight; blockUniqueId: "b6"; title: "INF_06 / BOTTOM_RIGHT"; width: root.blockWidth; height: root.blockHeight }
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
                Image { anchors.fill: parent; source: "images/hex_bg.png"; fillMode: Image.Tile; opacity: 0.15 }
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
                                        console.log("[ADMIN-SHELL] Секретная комбинация Ctrl+Клик сработала. Вызов SetupScreen.");
                                        screenSwitcher.sourceComponent = null;
                                        setupScreenLoader.source = "SetupScreen.qml";
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

                        // БЛОК ОЖИДАНИЯ С ПАУЗЫ (ОКНО Я ВЕРНУЛСЯ)
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
                                focus: root.sessionUser === "PAUSE"

                                onAccepted: {
                                    var cleanPin = pausePinInput.text.trim().replace(/[^0-9]/g, "");
                                    var cleanTarget = root.temporaryPausePin.trim().replace(/[^0-9]/g, "");

                                    if (cleanPin === cleanTarget && cleanTarget !== "") {
                                        pauseErrorText.visible = false;
                                        pausePinInput.text = "";
                                        root.temporaryPausePin = "----";
                                        root.sessionUser = "PLAYER_1";
                                        screenSwitcher.sourceComponent = null;
                                        dashboardLoader.source = "Dashboard.qml";
                                    } else {
                                        pauseErrorText.visible = true;
                                    }
                                }

                                background: Rectangle {
                                    color: pausePinInput.activeFocus ? "#08162a" : "#0d1117"
                                    border.color: pausePinInput.activeFocus ? "#3b82f6" : "#1d4ed8"
                                    border.width: pausePinInput.activeFocus ? 2 : 1
                                    radius: 4

                                    Behavior on color { ColorAnimation { duration: 150 } }
                                    Behavior on border.color { ColorAnimation { duration: 150 } }

                                    layer.enabled: pausePinInput.activeFocus
                                    layer.effect: MultiEffect { blurEnabled: true; blur: 0.2; brightness: 0.1 }
                                }
                            }

                            Text {
                                id: pauseErrorText
                                text: "Неверный PIN-код"
                                color: "#ef4444"
                                font.pixelSize: 12
                                visible: false
                                anchors.horizontalCenter: parent.horizontalCenter
                            }

                            Button {
                                width: parent.width
                                height: 50
                                text: "Я ВЕРНУЛСЯ"
                                onClicked: {
                                    var cleanPin = pausePinInput.text.trim().replace(/[^0-9]/g, "");
                                    var cleanTarget = root.temporaryPausePin.trim().replace(/[^0-9]/g, "");

                                    if (cleanPin === cleanTarget && cleanTarget !== "") {
                                        pauseErrorText.visible = false;
                                        pausePinInput.text = "";
                                        root.temporaryPausePin = "----";
                                        root.sessionUser = "PLAYER_1";
                                        screenSwitcher.sourceComponent = null;
                                        dashboardLoader.source = "Dashboard.qml";
                                    } else {
                                        pauseErrorText.visible = true;
                                    }
                                }
                            }
                        }

                        // СТАНДАРТНЫЙ БЛОК ГОСТЕВОГО ВХОДА (ТЕЛЕФОН + ПИН)
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
                                            phoneInput.forceActiveFocus();
                                            phoneInput.cursorPosition = 4;
                                            console.log("[TIMER-FOCUS] Принудительный фокус и курсор выставлены на позицию 4.");
                                        }
                                    }

                                    Component.onCompleted: {
                                        focusTimer.start();
                                    }

                                    onVisibleChanged: {
                                        if (visible && authCenter.authStep === 1) {
                                            focusTimer.start();
                                        }
                                    }

                                    onActiveFocusChanged: {
                                        if (activeFocus && (text === "+7 (   )    -  -  " || text === "")) {
                                            Qt.callLater(function() {
                                                phoneInput.cursorPosition = 4;
                                            });
                                        }
                                    }

                                    onAccepted: {
                                        authCenter.authStep = 2;
                                    }

                                    background: Rectangle {
                                        color: phoneInput.activeFocus ? "#08120a" : "#0d130e"
                                        border.color: phoneInput.activeFocus ? "#22c55e" : "#1a4d29"
                                        border.width: phoneInput.activeFocus ? 2 : 1
                                        radius: 4

                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on border.color { ColorAnimation { duration: 150 } }

                                        layer.enabled: phoneInput.activeFocus
                                        layer.effect: MultiEffect { blurEnabled: true; blur: 0.2; brightness: 0.1 }
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
                                                pinInput.cursorPosition = 0;
                                            });
                                        }
                                    }

                                    onAccepted: {
                                        loginToServer(phoneInput.text, pinInput.text);
                                    }

                                    background: Rectangle {
                                        color: pinInput.activeFocus ? "#08120a" : "#0d130e"
                                        border.color: pinInput.activeFocus ? "#22c55e" : "#1a4d29"
                                        border.width: pinInput.activeFocus ? 2 : 1
                                        radius: 4

                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on border.color { ColorAnimation { duration: 150 } }

                                        layer.enabled: pinInput.activeFocus
                                        layer.effect: MultiEffect { blurEnabled: true; blur: 0.2; brightness: 0.1 }
                                    }
                                }
                            }

                            Text {
                                id: authErrorText
                                text: root.authErrorMessage
                                visible: root.authErrorVisible
                                color: "#ef4444"
                                font.pixelSize: 12
                                font.bold: true
                                anchors.horizontalCenter: parent.horizontalCenter

                                Connections {
                                    target: pinInput
                                    function onTextChanged() { root.authErrorVisible = false; }
                                }
                                Connections {
                                    target: phoneInput
                                    function onTextChanged() { root.authErrorVisible = false; }
                                }
                            }

                            Item { height: 5; width: 1 }

                            Button {
                                width: parent.width
                                height: 55
                                text: authCenter.authStep === 1 ? "ДАЛЕЕ" : "НАЧАТЬ СЕССИЮ"
                                onClicked: {
                                    if (authCenter.authStep === 1) {
                                        authCenter.authStep = 2;
                                    } else {
                                        loginToServer(phoneInput.text, pinInput.text);
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
                                    id: backMouse
                                    anchors.fill: parent
                                    anchors.margins: -10
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        pinInput.text = "";
                                        root.authErrorVisible = false;
                                        authCenter.authStep = 1;
                                        focusTimer.start();
                                    }
                                }
                            }
                        }
                    }
                }

                // КАСTОМНЫЙ ОВЕРЛЕЙ BusyIndicator ДЛЯ БЛОКИРОВКИ И ОЖИДАНИЯ ОТВЕТА СЕРВЕРА
                Rectangle {
                    anchors.fill: parent
                    color: "#cc050a06"
                    radius: 4
                    visible: root.isLoggingIn
                    z: 10

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 15

                        BusyIndicator {
                            id: loginSpinner
                            Layout.alignment: Qt.AlignHCenter
                            running: root.isLoggingIn

                            contentItem: Rectangle {
                                implicitWidth: 48
                                implicitHeight: 48
                                color: "transparent"
                                border.color: "#22c55e"
                                border.width: 3
                                radius: 24

                                RotationAnimator on rotation {
                                    running: loginSpinner.running
                                    from: 0
                                    to: 360
                                    loops: Animation.Infinite
                                    duration: 1000
                                }
                            }
                        }

                        Text {
                            text: "ЗАПУСК СЕКТOРА..."
                            color: "#22c55e"
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 2
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }
            } // Конец authCenter
        }
    }

    function loginToServer(phone, pin) {
        console.log("[TRACE-AUTH] === СТАРТ ПРОЦЕССА АВТОРИЗАЦИИ ===");
        if (typeof NetworkManager === "undefined") {
            console.log("[TRACE-AUTH] КРИТИЧЕСКАЯ ОШИБКА: Объект NetworkManager не найден в контексте QML!");
            return;
        }

        var baseUrl = (typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : NetworkManager.serverUrl;
        console.log("[TRACE-AUTH] Базовый URL бэкенда:", baseUrl);
        console.log("[TRACE-AUTH] Текущий terminalId в корневом окне:", root.terminalId);
        console.log("[TRACE-AUTH] Сырые данные на входе -> Телефон:", phone, "| PIN:", pin);

        var cleanPhone = phone.replace(/[^0-9]/g, "");
        var cleanPin = pin.replace(/[^0-9]/g, "");
        var targetTerminalId = parseInt(root.terminalId);

        console.log("[TRACE-AUTH] Очищенные данные для JSON -> Телефон:", cleanPhone, "| PIN:", cleanPin, "| ID терминала (int):", targetTerminalId);
        if (cleanPhone === "" || cleanPin === "") {
            console.log("[TRACE-AUTH] ВНИМАНИЕ: Телефон или PIN пустые после очистки! Отмена отправки.");
            return;
        }

        var targetUrl = baseUrl + "/api/shell/login";
        console.log("[TRACE-AUTH] Итоговый URL запроса:", targetUrl);

        // ВКЛЮЧАЕМ ИНДИКАТOР ЗАГРУЗКИ ПО КЛИКУ "НАЧАТЬ СЕССИЮ"
        root.isLoggingIn = true;

        var xhr = new XMLHttpRequest();
        xhr.open("POST", targetUrl);
        xhr.setRequestHeader("Content-Type", "application/json");

        xhr.onreadystatechange = function() {
            console.log("[TRACE-AUTH] Смена состояния сети: readyState =", xhr.readyState);
            if (xhr.readyState === XMLHttpRequest.DONE) {
                console.log("[TRACE-AUTH] Запрос завершен! HTTP статус-код ответа =", xhr.status);
                console.log("[TRACE-AUTH] Сырой текстовый ответ сервера:", xhr.responseText);

                if (xhr.status === 200) {
                    try {
                        var response = JSON.parse(xhr.responseText);
                        console.log("[TRACE-AUTH] JSON успешно распарсен, статус в теле:", response.status);

                        if (response.status === "success") {
                            console.log("[TRACE-AUTH] УСПЕХ! Бэкенд подтвердил сессию. Загружаем дашборд...");
                            if (typeof Launcher !== "undefined") {
                                console.log("[TRACE-AUTH] Применяем политики QOS...");
                                Launcher.applyQosPolicies(true);
                            }

                            root.sessionUser = response.user.name || "GUEST";
                            root.sessionBalance = parseFloat(response.user.balance) || 0;
                            root.sessionTime = response.user.time_remaining || "00:00:00";

                            screenSwitcher.sourceComponent = null;
                            dashboardLoader.source = "Dashboard.qml";
                            if (NetworkManager !== null) {
                                NetworkManager.fetchGames();
                                NetworkManager.fetchProducts();
                            }
                        } else {
                            root.isLoggingIn = false; // ГАСИМ ИНДИКАТOР, ЕСЛИ СЕРВЕР ОТКАЗАЛ
                            console.log("[TRACE-AUTH] ОШИБКА: Бэкенд вернул 200, но статус в JSON не success:", xhr.responseText);
                        }
                    } catch (e) {
                        root.isLoggingIn = false;
                        console.log("[TRACE-AUTH] КРИТИЧЕСКАЯ ОШИБКА: Сбой парсинга JSON ответа:", e);
                    }
                } else if (xhr.status === 0) {
                    console.log("[TRACE-AUTH] КРИТИЧЕСКАЯ ОШИБКА: HTTP Status 0! Сервер лежит.");
                    root.isLoggingIn = false; // ГАСИМ ИНДИКАТOР ПРИ КРАШЕ СЕТИ
                    root.authErrorMessage = "Ошибка сети: Сервер недоступен";
                    root.authErrorVisible = true;
                } else {
                    console.log("[TRACE-AUTH] ОШИБКА СЕРВЕРА: Бэкенд отклонил запрос. Код ошибки:", xhr.status);
                    root.isLoggingIn = false; // ГАСИМ ИНДИКАТOР ПРИ НЕВЕРНОМ ПИНЕ (ОТВЕТ 401/422)

                    try {
                        var errResponse = JSON.parse(xhr.responseText);
                        root.authErrorMessage = errResponse.message ? errResponse.message : "Логин или PIN-код не найдены";
                    } catch(e) {
                        root.authErrorMessage = "Логин или PIN-код не найдены";
                    }
                    root.authErrorVisible = true;

                    if (typeof pinInput !== "undefined" && pinInput !== null) {
                        pinInput.text = "";
                    }
                }
            }
        };

        var payload = {
            "phone": cleanPhone,
            "pin": cleanPin,
            "terminal_id": targetTerminalId
        };
        var jsonString = JSON.stringify(payload);
        console.log("[TRACE-AUTH] Отправка сформированного JSON пакета в xhr.send():", jsonString);
        try {
            xhr.send(jsonString);
            console.log("[TRACE-AUTH] Вызов xhr.send() выполнен успешно, ждем ответ...");
        } catch (err) {
            root.isLoggingIn = false;
            console.log("[TRACE-AUTH] КРИТИЧЕСКАЯ ОШИБКА на этапе отправки запроса через send():", err.message);
        }
    }

    function fetchOverlays() {
        console.log("[DEBUG-NET] Вход в fetchOverlays(). Текущий terminalId =", root.terminalId);
        if (typeof NetworkManager === "undefined") {
            console.log("[DEBUG-NET] КРИТИЧЕСКАЯ ОШИБКА: NetworkManager не объявлен в QML контексте!");
            return;
        }
        if (root.terminalId === 0) {
            console.log("[DEBUG-NET] ОТМЕНА: Запрос заблокирован, так как terminalId == 0.");
            return;
        }

        var baseUrl = (typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : NetworkManager.serverUrl;
        var targetUrl = baseUrl + "/api/shell/overlays?terminal_id=" + root.terminalId + "&t=" + new Date().getTime();
        console.log("[DEBUG-NET] СТАРТ AJAX-запроса на URL:", targetUrl);
        var xhr = new XMLHttpRequest();
        xhr.open("GET", targetUrl);
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                console.log("[DEBUG-NET] ОТВЕТ СЕТИ: Статус HTTP =", xhr.status);
                if (xhr.status === 200) {
                    console.log("[DEBUG-NET] JSON успешно получен. Размер строки:", xhr.responseText.length);
                    try {
                        var parsedJson = JSON.parse(xhr.responseText);
                        updateOverlaysToScreen(parsedJson);
                    } catch(e) {
                        console.log("[DEBUG-NET] ОШИБКА парсинга JSON:", e);
                    }
                } else {
                    console.log("[DEBUG-NET] Сбой запроса. Сервер вернул ошибку.");
                }
            }
        }
        xhr.send();
    }

    function updateOverlaysToScreen(response) {
        console.log("[DEBUG-PARSER] Вход в updateOverlaysToScreen()");
        var actualData = response.data ? response.data : response;

        console.log("[DEBUG-PARSER] Проверка готовности контейнера. Свойство overlaysContainer.status =", overlaysContainer.status);
        if (overlaysContainer.status !== Loader.Ready) {
            console.log("[DEBUG-PARSER] ОТМЕНА: overlaysContainer ещё не Ready (или active=false), обработка прервана.");
            return;
        }

        var item = overlaysContainer.item;
        if (!item) {
            console.log("[DEBUG-PARSER] КРИТИЧЕСКАЯ ОШИБКА: overlaysContainer.item равен NULL!");
            return;
        }

        console.log("[DEBUG-PARSER] Контейнер на месте. b1-b6 доступны. Начинаем маппинг слотов...");
        var map = {
            "top_left": item.b1, "top_right": item.b2,
            "mid_left": item.b3, "mid_right": item.b5,
            "bottom_left": item.b4, "bottom_right": item.b6
        };
        for (var key in map) {
            if (actualData[key] && map[key]) {
                var vUrl = "";
                var blockData = actualData[key];

                if (blockData.content && blockData.content.layers && Array.isArray(blockData.content.layers)) {
                    var layersArray = blockData.content.layers;
                    for (var i = 0; i < layersArray.length; i++) {
                        if (layersArray[i].type === "video" || layersArray[i].type === "video_url") {
                            vUrl = layersArray[i].value || "";
                            break;
                        }
                    }
                }
                if (vUrl === "" && blockData.video_url) vUrl = blockData.video_url;
                console.log("[DEBUG-PARSER] Слот:", key, "| isActive:", blockData.is_active, "| Найдено video URL:", vUrl);

                map[key].videoSourceUrl = vUrl;
                map[key].content = blockData.content;
                map[key].isActive = blockData.is_active;
            } else {
                console.log("[DEBUG-PARSER] Варнинг: Слот", key, "отсутствует в JSON бэкенда или в устройстве QML.");
            }
        }
    }

    component OverlayBlock : Rectangle {
        property string title: "BLOCK"
        property string blockUniqueId: ""
        property bool isActive: true
        property var content: null
        property string videoSourceUrl: ""

        color: "#0a0a0a"
        border.color: isActive ? "#1a4d29" : "#050505"
        border.width: 1
        clip: true
        opacity: isActive ? 1.0 : 0.05

        Behavior on opacity { NumberAnimation { duration: 500 } }

        Text {
            text: title
            color: "#22c55e"
            font.pixelSize: 10
            z: 10
            anchors.margins: 10
            anchors.left: parent.left
            anchors.top: parent.top
            opacity: 0.6
        }

        Repeater {
            model: (content && content.layers) ? content.layers : []
            delegate: Item {
                anchors.fill: parent
                z: 20
                Text {
                    visible: modelData.type === "text"
                    text: modelData.value || ""
                    color: modelData.color || "white"
                    font.pixelSize: modelData.size || 16
                    font.bold: true
                    anchors.centerIn: parent
                }
                Image {
                    visible: modelData.type === "image"
                    source: modelData.type === "image" ? (modelData.value || "") : ""
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }
        }

        Loader {
            id: overlayVideoLoader
            active: parent.videoSourceUrl !== ""
            anchors.fill: parent
            z: 1
            sourceComponent: Component {
                Item {
                    id: videoInnerItem
                    anchors.fill: parent

                    property string currentPlayingPath: ""

                    function updateSource() {
                        var rawUrl = overlayVideoLoader.parent.videoSourceUrl;
                        var blockId = overlayVideoLoader.parent.blockUniqueId;

                        if (rawUrl !== "" && typeof NetworkManager !== "undefined") {
                            var path = NetworkManager.getLocalPath(rawUrl, blockId);
                            if (overlayBgPlayer.source.toString() === path && path !== "") {
                                return;
                            }

                            console.log("[PLAYER-OPTIMIZED]", blockId, "-> Переключение источника на:", path);
                            if (path !== "") {
                                overlayBgPlayer.stop();
                                overlayBgPlayer.source = path;
                                overlayBgPlayer.play();
                            } else {
                                if (overlayBgPlayer.source.toString() !== root.fallbackVideo) {
                                    overlayBgPlayer.stop();
                                    overlayBgPlayer.source = root.fallbackVideo;
                                    overlayBgPlayer.play();
                                }
                            }
                        }
                    }

                    Connections {
                        target: overlayVideoLoader.parent
                        function onVideoSourceUrlChanged() {
                            console.log("[PLAYER-SIGNAL]", overlayVideoLoader.parent.blockUniqueId, "-> Смена URL бэкенда:", overlayVideoLoader.parent.videoSourceUrl);
                            videoInnerItem.updateSource();
                        }
                    }

                    Connections {
                        target: NetworkManager
                        function onFileDownloaded(remoteUrl, localPath, target) {
                            var blockId = overlayVideoLoader.parent.blockUniqueId;
                            var currentUrl = overlayVideoLoader.parent.videoSourceUrl;
                            if (target === blockId && currentUrl === remoteUrl) {
                                if (overlayBgPlayer.source.toString() !== localPath) {
                                    console.log("[OVERLAY-CACHE] Слот", blockId, "считал скачанный файл:", localPath);
                                    overlayBgPlayer.stop();
                                    overlayBgPlayer.source = localPath;
                                    overlayBgPlayer.play();
                                }
                            }
                        }
                    }

                    MediaPlayer {
                        id: overlayBgPlayer
                        videoOutput: vOutOverlayBg
                        audioOutput: AudioOutput { muted: true }
                        loops: MediaPlayer.Infinite

                        Component.onCompleted: {
                            videoInnerItem.updateSource();
                        }

                        onMediaStatusChanged: {
                            var blockId = overlayVideoLoader.parent.blockUniqueId;
                            if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia) {
                                if (root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE") {
                                    overlayBgPlayer.play();
                                } else {
                                    overlayBgPlayer.stop();
                                }
                            } else if (mediaStatus === MediaPlayer.InvalidMedia) {
                                console.log("[PLAYER-ERROR]", blockId, "-> Ошибка кодека. Уход на fallback.");
                                overlayBgPlayer.stop();
                                if (overlayBgPlayer.source.toString() !== root.fallbackVideo) {
                                    overlayBgPlayer.source = root.fallbackVideo;
                                    if (root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE") {
                                        overlayBgPlayer.play();
                                    }
                                }
                            }
                        }
                    }
                    VideoOutput { id: vOutOverlayBg; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectCrop; layer.enabled: false }
                }
            }
        }
    }
}