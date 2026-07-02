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
    title: "REACTOR SHELL | Sector 0451"
    color: "#020202"

    property int terminalId: 0
    property string sessionUser: "GUEST"
    property real sessionBalance: 0.0
    property string sessionTime: "00:00:00"
    property alias authScreen: loginScreen
    property string temporaryPausePin: "----"
    property string pcTypeFromDatabase: "standard"
    property bool isHardwareAdmin: false

    readonly property string fallbackVideo: "file:///C:/ShellVideo/Cache/fallback_bg.mp4"

    readonly property int blockWidth: 524
    readonly property int blockHeight: 295
    readonly property int sideMargin: 50

    onTerminalIdChanged: {
        if (terminalId > 0) {
            console.log("[START-TRACE] [QML-REACTION] Железо авторизовано. ID:", terminalId, ". Начинаем загрузку оверлеев...");
            fetchOverlays();
        }
    }

    Component.onCompleted: {
        console.log("[START-TRACE] [STEP QML-A] Корневой Window загрузился. Ожидаем авторизации от REACTOR CONTROL...")
    }

    // ==========================================
    // 1. ОВЕРЛЕИ (КАЖДЫЙ СЛОТ ПОЛУЧАЕТ СВОЙ URL)
    // ==========================================
    Loader {
        id: overlaysContainer
        anchors.fill: parent
        z: 50
        active: (root.sessionUser === "GUEST" || root.sessionUser === "")

        sourceComponent: Component {
            Item {
                anchors.fill: parent
                Component.onCompleted: console.log("[START-TRACE] [STEP QML-B] Loader контейнера оверлеев активирован")

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

    // ==========================================
    // 2. ДАШБОРД
    // ==========================================
    Loader {
        id: dashboardLoader
        anchors.fill: parent
        source: ""
        visible: dashboardLoader.status === Loader.Ready
        z: 10
    }

    // ==========================================
    // 3. ЭКРАН БЛОКИРОВКИ (СТАТИКА БЕЗ ВИДЕО)
    // ==========================================
    Item {
        id: loginScreen
        anchors.fill: parent
        visible: root.sessionUser === "GUEST" || root.sessionUser === ""
        z: 20

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
            width: 800; height: 120
            anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 40
            Row {
                anchors.centerIn: parent; spacing: 20
                Text { text: "REACTOR"; color: "#000"; font.pixelSize: 80; style: Text.Outline; styleColor: "#22c55e" }
                Text { text: "0 4 5 1"; color: "#22c55e"; font.pixelSize: 60; font.bold: true; opacity: 0.8 }
            }
        }

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
                    Text { text: "PC-" + root.terminalId; color: "white"; font.pixelSize: 54; font.bold: true; anchors.horizontalCenter: parent.horizontalCenter }
                }
                Rectangle { width: parent.width; height: 1; color: "#22c55e"; opacity: 0.3 }
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
                    onClicked: { if (authCenter.authStep === 1) authCenter.authStep = 2; else loginToServer(phoneInput.text, pinInput.text); }
                }
            }
        }
    }

    // ==========================================
    // 4. СЕТЬ И ЛОГИКА ПАРСИНГА МАССИВА СЛОЕВ
    // ==========================================
    function loginToServer(phone, pin) {
        if (typeof NetManager === "undefined") return;
        var xhr = new XMLHttpRequest();
        xhr.open("POST", NetManager.serverUrl + "/api/shell/login");
        xhr.setRequestHeader("Content-Type", "application/json");
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                var response = JSON.parse(xhr.responseText);
                if (response.status === "success") {
                    root.sessionUser = response.user.name || "GUEST";
                    root.sessionBalance = parseFloat(response.user.balance) || 0;
                    root.sessionTime = response.user.time_remaining || "00:00:00";
                    loginScreen.visible = false;
                    dashboardLoader.source = "Dashboard.qml";
                    if (NetManager !== null) NetManager.fetchGames();
                }
            }
        }
        xhr.send(JSON.stringify({ "phone": phone.replace(/[^0-9]/g, ""), "pin": pin.replace(/[^0-9]/g, ""), "terminal_id": root.terminalId }));
    }

    Timer { interval: 30000; running: true; repeat: true; triggeredOnStart: false; onTriggered: fetchOverlays() }

    function fetchOverlays() {
        if (typeof NetManager === "undefined") return;
        if (root.terminalId === 0) return;

        var xhr = new XMLHttpRequest();
        xhr.open("GET", NetManager.serverUrl + "/api/shell/overlays?terminal_id=" + root.terminalId + "&t=" + new Date().getTime());
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200) {
                    updateOverlaysToScreen(JSON.parse(xhr.responseText));
                }
            }
        }
        xhr.send();
    }

    function updateOverlaysToScreen(response) {
        var actualData = response.data ? response.data : response;

        if (!actualData || overlaysContainer.status !== Loader.Ready) return;
        var item = overlaysContainer.item;
        if (!item) return;

        var map = {
            "top_left": item.b1,
            "top_right": item.b2,
            "mid_left": item.b3,
            "mid_right": item.b5,
            "bottom_left": item.b4,
            "bottom_right": item.b6
        };

        for (var key in map) {
            if (actualData[key] && map[key]) {
                var vUrl = "";
                var blockData = actualData[key];

                // ИСПРАВЛЕНО: Парсим массив layers внутри content, чтобы вытащить слой с типом "video"
                if (blockData.content && blockData.content.layers && Array.isArray(blockData.content.layers)) {
                    var layersArray = blockData.content.layers;
                    for (var i = 0; i < layersArray.length; i++) {
                        if (layersArray[i].type === "video" || layersArray[i].type === "video_url") {
                            vUrl = layersArray[i].value || "";
                            break;
                        }
                    }
                }

                // Резервный поиск на случай, если бэкенд поменяет формат
                if (vUrl === "" && blockData.video_url) vUrl = blockData.video_url;
                if (vUrl === "" && blockData.content && blockData.content.video_url) vUrl = blockData.content.video_url;

                console.log("[DEBUG-API] Слот:", key, "-> Распарсен URL из слоев:", vUrl);

                map[key].videoSourceUrl = vUrl;
                map[key].content = blockData.content;
                map[key].isActive = blockData.is_active;
            }
        }
    }

    // ==========================================
    // ИЗОЛИРОВАННЫЙ КОМПОНЕНТ ОВЕРЛЕЙ-БЛОКА
    // ==========================================
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
            text: title; color: "#22c55e"; font.pixelSize: 10; z: 10
            anchors.margins: 10; anchors.left: parent.left; anchors.top: parent.top; opacity: 0.6
        }

        // Текстовые слои и картинки рендерятся поверх видео (z: 20)
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
                    font.bold: true; anchors.centerIn: parent
                }

                Image {
                    visible: modelData.type === "image"
                    source: modelData.type === "image" ? (modelData.value || "") : ""
                    anchors.fill: parent; fillMode: Image.PreserveAspectCrop; asynchronous: true
                }
            }
        }

        // Подложка плеера (z: 1)
        Loader {
            active: parent.videoSourceUrl !== ""
            anchors.fill: parent
            z: 1

            sourceComponent: Component {
                Item {
                    anchors.fill: parent

                    Connections {
                        target: NetManager
                        function onFileDownloaded(remoteUrl, localPath, target) {
                            if (target === blockUniqueId && videoSourceUrl === remoteUrl) {
                                console.log("[OVERLAY-CACHE]", blockUniqueId, "перешел на локальный кэш-файл:", localPath);
                                overlayBgPlayer.source = localPath;
                            }
                        }
                    }

                    MediaPlayer {
                        id: overlayBgPlayer
                        videoOutput: vOutOverlayBg
                        audioOutput: AudioOutput { muted: true }
                        loops: MediaPlayer.Infinite

                        Component.onCompleted: {
                            if (videoSourceUrl !== "" && typeof NetManager !== "undefined") {
                                overlayBgPlayer.source = NetManager.getLocalPath(videoSourceUrl, blockUniqueId);
                            } else {
                                overlayBgPlayer.source = root.fallbackVideo;
                            }
                        }

                        onMediaStatusChanged: {
                            if (mediaStatus === MediaPlayer.LoadedMedia && loginScreen.visible) {
                                overlayBgPlayer.play();
                            } else if (mediaStatus === MediaPlayer.NoMedia || mediaStatus === MediaPlayer.InvalidMedia) {
                                if (overlayBgPlayer.source !== root.fallbackVideo) {
                                    overlayBgPlayer.source = root.fallbackVideo;
                                    overlayBgPlayer.play();
                                }
                            }
                        }

                        onErrorOccurred: function(error, errorString) {
                            if (source !== root.fallbackVideo) {
                                overlayBgPlayer.source = root.fallbackVideo;
                                overlayBgPlayer.play();
                            }
                        }
                    }

                    VideoOutput {
                        id: vOutOverlayBg
                        anchors.fill: parent
                        fillMode: VideoOutput.PreserveAspectCrop
                    }
                }
            }
        }
    }
}