import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtWebSockets
import QtMultimedia

Window {
    id: root
    width: 1920
    height: 1080
    visible: true
    title: "REACTOR SHELL | Sector 0451"
    color: "#020202"
    // visibility: Window.FullScreen // Для релиза в клубе раскомментируй

    // Загрузка шрифта
    FontLoader { id: bomberFont; source: "qrc:/qt/qml/sector0451/fonts/bomber.otf" }

    // Константы терминала
    readonly property int blockWidth: 524
    readonly property int blockHeight: 295
    readonly property int sideMargin: 50
    property string terminalId: "PC-045"

    // ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ СЕССИИ (Сюда пишем при логине, Дашборд их читает)
    property string sessionUser: "GUEST"
    property real sessionBalance: 0.0
    property string sessionTime: "00:00:00"

    // Глобальный фон
    Rectangle { anchors.fill: parent; z: -1; color: "#050505" }

    // ==========================================
    // 1. ОВЕРЛЕИ (ВИДНЫ ТОЛЬКО НА ЭКРАНЕ БЛОКИРОВКИ)
    // ==========================================
    Column {
        id: leftColumn
        anchors.left: parent.left
        anchors.leftMargin: sideMargin
        anchors.verticalCenter: parent.verticalCenter
        spacing: 20
        z: 50
        visible: loginScreen.visible // Исчезнут при входе

        OverlayBlock { id: b1; title: "CAM_01 / TOP_LEFT"; width: root.blockWidth; height: root.blockHeight }
        OverlayBlock { id: b3; title: "DAT_02 / MID_LEFT"; width: root.blockWidth; height: root.blockHeight }
        OverlayBlock { id: b4; title: "INF_03 / BOTTOM_LEFT"; width: root.blockWidth; height: root.blockHeight }
    }

    Column {
        id: rightColumn
        anchors.right: parent.right
        anchors.rightMargin: sideMargin
        anchors.verticalCenter: parent.verticalCenter
        spacing: 20
        z: 50
        visible: loginScreen.visible // Исчезнут при входе

        OverlayBlock { id: b2; title: "CAM_04 / TOP_RIGHT"; width: root.blockWidth; height: root.blockHeight }
        OverlayBlock { id: b5; title: "DAT_05 / MID_RIGHT"; width: root.blockWidth; height: root.blockHeight }
        OverlayBlock { id: b6; title: "INF_06 / BOTTOM_RIGHT"; width: root.blockWidth; height: root.blockHeight }
    }

    // ==========================================
    // 2. ДАШБОРД (ИГРОВОЙ ИНТЕРФЕЙС)
    // ==========================================
    Loader {
        id: dashboardLoader
        anchors.fill: parent
        source: ""
        visible: dashboardLoader.status === Loader.Ready
        z: 10

        // АСИНХРОННАЯ ПЕРЕДАЧА ДАННЫХ
        onLoaded: {
            if (item) {
                item.userName = root.sessionUser;
                item.userBalance = root.sessionBalance;
                item.timeRemaining = root.sessionTime;
                console.log("Дашборд успешно загружен и получил данные");
            }
        }
    }

    // ==========================================
    // 3. ЭКРАН БЛОКИРОВКИ
    // ==========================================
    Item {
        id: loginScreen
        anchors.fill: parent
        visible: true
        z: 20

        // Логотип
        Item {
            id: logoArea
            width: 800; height: 120
            anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 40
            Row {
                anchors.centerIn: parent; spacing: 20
                Text { text: "REACTOR"; color: "#000"; font.family: bomberFont.name; font.pixelSize: 80; style: Text.Outline; styleColor: "#22c55e" }
                Text { text: "0 4 5 1"; color: "#22c55e"; font.pixelSize: 60; font.bold: true; opacity: 0.8 }
            }
        }

        // Окно ввода
        Rectangle {
            id: authCenter
            width: 420; height: 500; anchors.centerIn: parent
            color: "#050a06"; border.color: "#1a4d29"; radius: 4; opacity: 0.95
            property int authStep: 1

            Column {
                anchors.fill: parent; anchors.margins: 40; spacing: 30

                Column {
                    anchors.horizontalCenter: parent.horizontalCenter
                    Text { text: "TERMINAL_ID"; color: "#22c55e"; font.pixelSize: 12; opacity: 0.6; anchors.horizontalCenter: parent.horizontalCenter }
                    Text { text: root.terminalId; color: "white"; font.pixelSize: 54; font.bold: true; anchors.horizontalCenter: parent.horizontalCenter }
                }

                Rectangle { width: parent.width; height: 1; color: "#22c55e"; opacity: 0.3 }

                // Поля
                Item {
                    width: parent.width; height: 100
                    Column {
                        visible: authCenter.authStep === 1; width: parent.width; spacing: 12
                        Text { text: "НОМЕР ТЕЛЕФОНА"; color: "#22c55e"; font.pixelSize: 10 }
                        TextField { id: phoneInput; width: parent.width; height: 60; color: "#22c55e"; font.pixelSize: 24; inputMask: "+7 (999) 999-99-99"; focus: authCenter.authStep === 1 }
                    }
                    Column {
                        visible: authCenter.authStep === 2; width: parent.width; spacing: 12
                        Text { text: "PIN-КОД"; color: "#22c55e"; font.pixelSize: 10 }
                        TextField { id: pinInput; width: parent.width; height: 60; color: "white"; font.pixelSize: 32; echoMode: TextInput.Password; inputMask: "0000"; focus: authCenter.authStep === 2; horizontalAlignment: Text.AlignHCenter }
                    }
                }

                Button {
                    width: parent.width; height: 55
                    text: authCenter.authStep === 1 ? "ДАЛЕЕ" : "НАЧАТЬ СЕССИЮ"
                    onClicked: {
                        if (authCenter.authStep === 1) authCenter.authStep = 2;
                        else loginToServer(phoneInput.text, pinInput.text);
                    }
                }
            }
        }
    }

    // ==========================================
    // 4. СЕТЬ И JS ЛОГИКА
    // ==========================================
    function loginToServer(phone, pin) {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "http://127.0.0.1:22222/api/shell/login");
        xhr.setRequestHeader("Content-Type", "application/json");

        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    var response = JSON.parse(xhr.responseText);
                    if (response.status === "success") {
                        // 1. Сначала пишем данные в корень
                        root.sessionUser = response.user.name || "GUEST";
                        root.sessionBalance = parseFloat(response.user.balance) || 0;
                        root.sessionTime = response.user.ends_at || "00:00";

                        // 2. Скрываем логин (это автоматически скроет оверлеи)
                        loginScreen.visible = false;

                        // 3. Запускаем загрузку Дашборда
                        dashboardLoader.source = "Dashboard.qml";
                    }
                } else {
                    console.log("Ошибка авторизации: " + xhr.status);
                    authCenter.authStep = 1;
                    pinInput.text = "";
                }
            }
            // Внутри Main.qml в функции loginToServer
            if (response.status === "success") {
                root.sessionUser = response.user.name || "GUEST";
                // ... сохранение других данных ...

                loginScreen.visible = false;
                dashboardLoader.source = "Dashboard.qml";

                // ДЕРГАЕМ C++ МЕТОД ИЗ QML СЮДА:
                NetManager.fetchGames();
            }
        }
        xhr.send(JSON.stringify({
            "phone": phone.replace(/[^0-9]/g, ""),
            "pin": pin.replace(/[^0-9]/g, ""),
            "terminal_id": root.terminalId
        }));
    }

    // Загрузка оверлеев
    Timer { interval: 30000; running: true; repeat: true; triggeredOnStart: true; onTriggered: fetchOverlays() }

    function fetchOverlays() {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", "http://127.0.0.1:22222/api/shell/overlays?t=" + new Date().getTime());
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                updateOverlaysToScreen(JSON.parse(xhr.responseText));
            }
        }
        xhr.send();
    }

    function updateOverlaysToScreen(response) {
        if (!response.data) return;
        var map = {"top_left": b1, "top_right": b2, "mid_left": b3, "mid_right": b5, "bottom_left": b4, "bottom_right": b6};
        for (var key in map) {
            if (response.data[key]) {
                map[key].content = response.data[key].content;
                map[key].isActive = response.data[key].is_active;
            }
        }
    }

    // WebSocket (Pusher)
    WebSocket {
        id: terminalSocket
            url: "ws://127.0.0.1:8080/app/reactor_shell_key_2026?protocol=7&client=js&version=8.3.0"
            active: true
            onStatusChanged: {
                // Явно указываем terminalSocket.status
                if (terminalSocket.status === WebSocket.Open) {
                    terminalSocket.sendTextMessage(JSON.stringify({"event": "pusher:subscribe", "data": {"channel": "terminals.all"}}));
                }
        }
        onTextMessageReceived: {
            var response = JSON.parse(message);
            if (response.event === "overlay.changed") {
                var payload = JSON.parse(response.data);
                var map = {"top_left": b1, "top_right": b2, "mid_left": b3, "mid_right": b5, "bottom_left": b4, "bottom_right": b6};
                if (map[payload.block_position]) {
                    map[payload.block_position].content = payload.data.content;
                    map[payload.block_position].isActive = payload.data.is_active;
                }
            }
        }
    }

    // ==========================================
    // 5. КОМПОНЕНТ ОВЕРЛЕЯ (ФИКС ВИДЕО И ФОРМАТОВ)
    // ==========================================
    component OverlayBlock : Rectangle {
        property string title: "BLOCK"
        property bool isActive: true
        property var content: null

        color: "#0a0a0a"; border.color: isActive ? "#1a4d29" : "#050505"
        border.width: 1; clip: true; opacity: isActive ? 1.0 : 0.05
        Behavior on opacity { NumberAnimation { duration: 500 } }

        Text {
            text: title; color: "#22c55e"; font.pixelSize: 10; z: 10
            anchors.margins: 10; anchors.left: parent.left; anchors.top: parent.top; opacity: 0.6
        }

        Repeater {
            model: (content && content.layers) ? content.layers : []
            delegate: Item {
                anchors.fill: parent

                // 1. ТЕКСТ
                Text {
                    visible: modelData.type === "text"
                    text: modelData.value || ""
                    color: modelData.color || "white"
                    font.pixelSize: modelData.size || 16
                    font.bold: true
                    anchors.centerIn: parent
                }

                // 2. ИЗОБРАЖЕНИЕ (С защитой от попытки загрузить видео)
                Image {
                    visible: modelData.type === "image"
                    source: modelData.type === "image" ? (modelData.value || "") : ""
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }

                // 3. ВИДЕО (Безопасная загрузка через MediaPlayer для Qt 6)
                Loader {
                    active: modelData.type === "video"
                    anchors.fill: parent
                    sourceComponent: Component {
                        Item {
                            anchors.fill: parent
                            MediaPlayer {
                                id: mPlayer
                                source: modelData.value || ""
                                videoOutput: vOut
                                audioOutput: AudioOutput { muted: true }
                                loops: MediaPlayer.Infinite
                                Component.onCompleted: mPlayer.play()
                            }
                            VideoOutput {
                                id: vOut
                                anchors.fill: parent
                                fillMode: VideoOutput.PreserveAspectCrop
                            }
                        }
                    }
                }
            }
        }
    }
}