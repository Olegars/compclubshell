// Путь: Dashboard.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import Qt5Compat.GraphicalEffects

Item {
    id: dashboardRoot
    anchors.fill: parent

    // БУФЕР ДЛЯ ХРАНЕНИЯ ТОКЕНОВ СЕТЕВОЙ СЕССИИ (ОБЪЯВЛЕН СТРОГО ОДИН РАЗ)
    property string lastToken: ""
    property string lastLogin: ""
    property string lastId: ""
    property string lastPersonaName: ""

    // Данные текущей сессии пользователя
    property string userName: (typeof root !== 'undefined') ? root.sessionUser : "PLAYER_1"
    property real userBalance: (typeof root !== 'undefined') ? root.sessionBalance : 0.0
    property string timeRemaining: (typeof root !== 'undefined') ? root.sessionTime : "00:00:00"

    property int termId: (typeof root !== 'undefined' && root !== null) ? root.terminalId : 0
    property string pcType: (typeof root !== 'undefined' && root !== null) ? root.pcTypeFromDatabase : "standard"

    property bool isProBootcamp: (pcType.toLowerCase() === "pro" ||
                                  pcType.toLowerCase() === "bootcamp" ||
                                  pcType.toLowerCase() === "trio" ||
                                  pcType.toLowerCase() === "vip")

    property string zoneTitle: (pcType.toLowerCase() === "trio") ? "TRIO ZONE" :
                               (pcType.toLowerCase() === "vip") ? "VIP ZONE" :
                               (isProBootcamp ? "PRO BOOTCAMP ZONE" : "STANDARD ZONE")

    property string accentColor: isProBootcamp ? "#a855f7" : "#22c55e"
    property string darkBg: "#030704"
    property string currentLanguage: "RU"

    // Riot: выбор личного / клубного аккаунта перед запуском
    property int pendingRiotGameId: 0
    property string pendingRiotTitle: ""
    property string pendingRiotExe: ""
    property string pendingRiotArgs: ""
    property string pendingRiotPlatform: "riot"

    readonly property string defaultRiotClient: "C:\\Riot Games\\Riot Client\\RiotClientServices.exe"

    function looksLikeRiot(platform, exePath, args, title) {
        var p = String(platform || "").toLowerCase()
        var e = String(exePath || "").toLowerCase()
        var a = String(args || "").toLowerCase()
        var t = String(title || "").toLowerCase()
        return p === "riot" || p.indexOf("riot") >= 0
            || e.indexOf("riotclient") >= 0 || e.indexOf("riot games") >= 0
            || a.indexOf("valorant") >= 0 || a.indexOf("league_of_legends") >= 0
            || a.indexOf("league of legends") >= 0
            || t.indexOf("valorant") >= 0 || t.indexOf("league of legends") >= 0
            || t.indexOf("legends") >= 0 && t.indexOf("league") >= 0
    }

    function resolveRiotExe(exePath) {
        var e = String(exePath || "").toLowerCase()
        if (e.indexOf("riotclientservices") >= 0)
            return exePath
        return defaultRiotClient
    }

    function resolveRiotArgs(args, title, exePath) {
        var a = String(args || "").trim()
        var al = a.toLowerCase()
        // В args ошибочно положили путь к .exe
        if (a.length > 0 && al.indexOf("--launch-product") < 0
                && (al.indexOf(".exe") >= 0 || al.indexOf("riot client") >= 0
                    || a.indexOf(":\\") === 0 || (a.length >= 3 && a.charAt(1) === ":"))) {
            console.warn("[RIOT] args похожи на путь — очищаем:", a)
            a = ""
        }
        if (a.length > 0)
            return a
        var t = String(title || "").toLowerCase()
        if (t.indexOf("valorant") >= 0)
            return "--launch-product=valorant --launch-patchline=live"
        if (t.indexOf("league") >= 0)
            return "--launch-product=league_of_legends --launch-patchline=live"
        return ""
    }

    function launchRiotPersonal() {
        var exe = resolveRiotExe(pendingRiotExe)
        var title = pendingRiotTitle || "Riot"
        var args = resolveRiotArgs(pendingRiotArgs, title, pendingRiotExe)
        riotAccountPopup.close()
        if (typeof root !== 'undefined') {
            root.isLoggingIn = true
            root.currentGameId = pendingRiotGameId
            root.showGameLoading("riot", title)
        }
        if (typeof Launcher === 'undefined') {
            console.error("[RIOT] Launcher не найден")
            if (typeof root !== 'undefined') {
                root.isLoggingIn = false
                root.hideGameLoading()
            }
            return
        }
        var payload = {
            "platform": "riot",
            "platform_source": "personal_account",
            "exe_path": exe,
            "args": args,
            "login": "",
            "password": "",
            "game_id": pendingRiotGameId,
            "game_title": title,
            "terminal_id": parseInt(dashboardRoot.termId),
            "auth": { "mode": "personal" }
        }
        console.log("[RIOT] личный аккаунт → Riot Client", exe, args)
        Launcher.launchPlatformSessionString(JSON.stringify(payload), "")
        if (typeof root !== 'undefined')
            root.scheduleHideGameLoading()
    }

    function launchRiotClub() {
        var gameId = pendingRiotGameId
        var title = pendingRiotTitle || "Riot"
        riotAccountPopup.close()
        if (typeof root !== 'undefined') {
            root.isLoggingIn = true
            root.currentGameId = gameId
            root.showGameLoading("riot", title)
        }
        startClubTakeAccount(gameId, "riot", title, pendingRiotExe, pendingRiotArgs)
    }

    function startClubTakeAccount(gameId, modelPlatform, modelTitle, modelExe, modelArgs) {
        var baseUrl = "http://192.168.222.2:22222"
        if (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl !== 'undefined')
            baseUrl = NetworkManager.serverUrl

        var EastonXhr = new XMLHttpRequest()
        EastonXhr.open("POST", baseUrl + "/api/shell/games/take-account")
        EastonXhr.setRequestHeader("Content-Type", "application/json")
        EastonXhr.onreadystatechange = function() {
            if (EastonXhr.readyState !== XMLHttpRequest.DONE)
                return
            if (EastonXhr.status === 200) {
                try {
                    var res = JSON.parse(EastonXhr.responseText)
                    if (res.status === "success") {
                        dashboardRoot.lastToken = ""
                        dashboardRoot.lastLogin = res.login ? String(res.login) : ""
                        dashboardRoot.lastId = res.steam_id ? String(res.steam_id) : ""
                        dashboardRoot.lastPersonaName = res.persona_name ? String(res.persona_name) : ""

                        res["terminal_id"] = parseInt(dashboardRoot.termId)
                        res["game_id"] = parseInt(gameId)

                        var plat = String(res.platform || modelPlatform || "").toLowerCase()
                        if (!res.platform)
                            res.platform = plat || "steam"

                        var argsStr = String(res.args || modelArgs || "")
                        var exeStr = String(res.exe_path || modelExe || "")
                        var looksEpic = argsStr.toLowerCase().indexOf("com.epicgames.launcher") >= 0
                                || exeStr.toLowerCase().indexOf("epicgameslauncher") >= 0
                                || exeStr.toLowerCase().indexOf("epic games") >= 0
                        var looksEa = exeStr.toLowerCase().indexOf("eadesktop.exe") >= 0
                                || exeStr.toLowerCase().indexOf("ea desktop") >= 0
                                || argsStr.toLowerCase().indexOf("origin2://") >= 0
                                || argsStr.toLowerCase().indexOf("origin://") >= 0
                                || argsStr.toLowerCase().indexOf("eadm://") >= 0
                        var looksRiot = dashboardRoot.looksLikeRiot(res.platform, exeStr, argsStr, res.game_title || modelTitle)
                        if (looksEpic && res.platform !== "epic") {
                            res.platform = "epic"
                            res.platform_source = (res.platform_source || "") + "+qml_override_epic"
                        } else if (looksEa && res.platform !== "ea" && res.platform !== "epic") {
                            res.platform = "ea"
                            res.platform_source = (res.platform_source || "") + "+qml_override_ea"
                        } else if (looksRiot && res.platform !== "riot") {
                            res.platform = "riot"
                            res.platform_source = (res.platform_source || "") + "+qml_override_riot"
                        }

                        if (!res.game_title)
                            res.game_title = modelTitle || ""
                        if (!res.exe_path && modelExe)
                            res.exe_path = modelExe
                        if (!res.args && modelArgs)
                            res.args = modelArgs
                        if (looksRiot || res.platform === "riot") {
                            res.platform = "riot"
                            res.exe_path = dashboardRoot.resolveRiotExe(res.exe_path || modelExe || "")
                            res.args = dashboardRoot.resolveRiotArgs(
                                res.args || modelArgs || "",
                                res.game_title || modelTitle || "",
                                res.exe_path)
                        }

                        if (typeof root !== 'undefined' && root.updateGameLoading)
                            root.updateGameLoading(res.platform || "", res.game_title || modelTitle || "")

                        if (typeof Launcher !== 'undefined') {
                            console.log("[SESSION] take-account OK:", res.platform, res.login, "→ launch")
                            Launcher.launchPlatformSessionString(JSON.stringify(res), String(res.platform_app_id || ""))
                        } else {
                            console.error("[SESSION] Launcher не найден")
                        }
                    } else {
                        console.warn("[SESSION] take-account:", res.message || "ошибка")
                        if (String(res.message || "").indexOf("занят") >= 0
                            && dashboardRoot.looksLikeRiot(modelPlatform, modelExe, modelArgs, modelTitle)) {
                            riotBusyHintPopup.open()
                        }
                        if (typeof root !== 'undefined') {
                            root.isLoggingIn = false
                            root.hideGameLoading()
                        }
                    }
                } catch (e) {
                    console.error("[SESSION] parse error:", e.toString())
                    if (typeof root !== 'undefined') {
                        root.isLoggingIn = false
                        root.hideGameLoading()
                    }
                }
            } else {
                console.error("[SESSION] take-account HTTP", EastonXhr.status)
                if (typeof root !== 'undefined') {
                    root.isLoggingIn = false
                    root.hideGameLoading()
                }
            }
        }
        EastonXhr.send(JSON.stringify({
            "game_id": parseInt(gameId),
            "terminal_id": parseInt(dashboardRoot.termId)
        }))
    }

    Component.onCompleted: {
        if (typeof NetworkManager !== 'undefined') {
            NetworkManager.fetchProducts()
        }
    }

    ListModel {
        id: cartModel

        function updateTotalPrice() {
            var sum = 0;
            for (var i = 0; i < count; i++) {
                var item = get(i);
                if (item && item.price) {
                    sum += item.price * item.quantity;
                }
            }
            return sum.toFixed(0);
        }

        function addProduct(prodId, prodName, prodPrice) {
            var parsedPrice = parseFloat(prodPrice || 0);
            console.log("[CART-TRACE] Добавление в корзину -> ID:", prodId, "| Name:", prodName, "| Цена:", parsedPrice);
            for (var i = 0; i < count; i++) {
                if (get(i).productId === prodId) {
                    setProperty(i, "quantity", get(i).quantity + 1);
                    return;
                }
            }
            append({ "productId": prodId, "name": prodName, "price": parsedPrice, "quantity": 1 });
        }
    }

    Rectangle {
        id: bgContainer
        anchors.fill: parent
        color: "#020202"
        Image {
            anchors.fill: parent
            source: Qt.resolvedUrl("images/hex_bg.png")
            fillMode: Image.Tile
            opacity: 0.35
            horizontalAlignment: Image.AlignHCenter
            verticalAlignment: Image.AlignVCenter
            onStatusChanged: if (status === Image.Error)
                console.warn("[BG] hex_bg load failed:", source)
        }
        RadialGradient {
            anchors.fill: parent
            horizontalRadius: dashboardRoot.width / 1.2
            verticalRadius: dashboardRoot.height / 1.2
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: "black" }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 40

        Rectangle {
            id: leftPanelBox
            Layout.preferredWidth: 380
            Layout.fillHeight: true
            color: darkBg
            border.color: Qt.rgba(isProBootcamp ? 0.65 : 0.13, isProBootcamp ? 0.33 : 0.77, isProBootcamp ? 0.96 : 0.36, 0.4)
            border.width: 1
            radius: 6

            Item {
                anchors.fill: parent
                anchors.margins: 30

                Rectangle {
                    id: sosBtn
                    width: 60
                    height: 60
                    radius: 4
                    anchors.top: parent.top
                    anchors.right: parent.right
                    z: 100
                    color: sosMouse.containsMouse ? "#dc2626" : "#450a0a"
                    border.color: "#dc2626"
                    border.width: 1
                    layer.enabled: sosMouse.containsMouse
                    layer.effect: MultiEffect { blurEnabled: true; blur: 0.15 }

                    Column {
                        anchors.centerIn: parent
                        spacing: 2
                        Text { text: "⚠️"; font.pixelSize: 14; anchors.horizontalCenter: parent.horizontalCenter }
                        Text { text: "SOS"; color: sosMouse.containsMouse ? "black" : "white"; font.pixelSize: 12; font.bold: true; anchors.horizontalCenter: parent.horizontalCenter }
                    }

                    MouseArea {
                        id: sosMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            console.log("[QML-SOS] Нажата кнопка SOS! Форсированный пуш кэша...");
                            if (typeof Launcher !== 'undefined') {
                                Launcher.executeSosTestInject(dashboardRoot.lastToken, dashboardRoot.lastLogin, dashboardRoot.lastId, dashboardRoot.lastPersonaName);
                            }
                        }
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    Row {
                        Layout.alignment: Qt.AlignLeft
                        spacing: 8
                        Rectangle { width: 6; height: 6; radius: 3; color: accentColor; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: "LATENCY (EU): " + (typeof NetworkManager !== 'undefined' ? NetworkManager.getLatency("162.249.72.1") : 24) + " MS"; color: accentColor; font.pixelSize: 9; font.bold: true; opacity: 0.5 }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 35
                        color: "transparent"
                        Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; text: dashboardRoot.zoneTitle; color: accentColor; font.pixelSize: 14; font.bold: true; font.letterSpacing: 2 }
                    }

                    Column {
                        Layout.fillWidth: true
                        spacing: 5
                        Text { text: "ПОЛЬЗОВАТЕЛЬ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                        Text { text: dashboardRoot.userName; color: "white"; font.pixelSize: 28; font.bold: true }
                        Item { height: 10; width: 1 }
                        Text { text: "ОСТАЛОСЬ ВРЕМЕНИ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                        Text { text: dashboardRoot.timeRemaining; color: "white"; font.pixelSize: 52; font.family: "Monospace"; font.bold: true }
                        Text { text: "БАЛАНС: " + dashboardRoot.userBalance.toFixed(2) + " ₽"; color: "#a3a3a3"; font.pixelSize: 18 }
                    }

                    Item { Layout.fillHeight: true }
                    Text { text: "БЫСТРЫЙ ЗАПУСК ПЛАТФОРМ"; color: accentColor; font.pixelSize: 10; font.bold: true; font.letterSpacing: 2; opacity: 0.6; Layout.alignment: Qt.AlignHCenter }

                    GridLayout {
                        columns: 3
                        rows: 2
                        columnSpacing: 10
                        rowSpacing: 10
                        Layout.fillWidth: true
                        PlatformSquareBtn {
                            btnText: "STEAM"
                            iconText: "󰓓"
                            brandColor: "#00adef"
                            onClicked: {
                                if (typeof root !== 'undefined')
                                    root.showGameLoading()
                                var mockAuth = {
                                    "platform": "steam",
                                    "login": "",
                                    "password": "",
                                    "args": "-silent -shutdown"
                                }
                                if (typeof Launcher !== 'undefined') {
                                    console.log("[QML-CLICK] Быстрый запуск чистого Steam...");
                                    Launcher.launchPlatformSession(mockAuth, "")
                                }
                            }
                        }
                        PlatformSquareBtn { btnText: "EPIC"; iconText: "󰊗"; brandColor: "#ffffff"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe", "", "", "", "") } }
                        PlatformSquareBtn { btnText: "ROBLOX"; iconText: "󰩊"; brandColor: "#e11d48"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Users\\Public\\Desktop\\Roblox Player.lnk", "", "", "", "") } }
                        PlatformSquareBtn { btnText: "RIOT"; iconText: "󰊴"; brandColor: "#d32f2f"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Riot Games\\Riot Client\\RiotClientServices.exe", "", "", "", "") } }
                        PlatformSquareBtn { btnText: "EA APP"; iconText: "󰓡"; brandColor: "#ff5722"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe", "", "", "", "") } }
                        PlatformSquareBtn { btnText: "VK PLAY"; iconText: "󰕼"; brandColor: "#ff3347"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Users\\Public\\Desktop\\VK Play.lnk", "", "", "", "") } }
                    }

                    Item { height: 5; width: 1 }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        ActionBtn { id: storeActionBtn; text: "МАГАЗИН"; icon: "🛒"; baseColor: "#eab308"; isActiveStatus: (typeof root !== 'undefined') ? root.hasActiveOrder : false; orderIsFinished: (typeof root !== 'undefined' && root.orderStatusText === "ЗАКАЗ ВЫПОЛНЕН"); statusText: (typeof root !== 'undefined' && root.hasActiveOrder) ? root.orderStatusText : ""; onClicked: storePopup.open() }
                        ActionBtn { text: "ПОПОЛНИТЬ БАЛАНС"; icon: "💳"; baseColor: "#eab308"; onClicked: depositPopup.open() }
                        ActionBtn {
                            text: "ОТОЙТИ (ПАУЗА)"
                            icon: "⏳"
                            baseColor: "#3b82f6"
                            onClicked: {
                                var baseUrl = "http://192.168.222.2:22222"
                                if (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl !== 'undefined') {
                                    baseUrl = NetworkManager.serverUrl
                                }
                                var pcId = parseInt(dashboardRoot.termId)
                                if (!pcId) {
                                    console.error("[PAUSE] terminalId пуст")
                                    return
                                }
                                var xhr = new XMLHttpRequest()
                                xhr.open("POST", baseUrl + "/api/shell/games/pause")
                                xhr.setRequestHeader("Content-Type", "application/json")
                                xhr.onreadystatechange = function() {
                                    if (xhr.readyState !== XMLHttpRequest.DONE)
                                        return
                                    if (xhr.status !== 200) {
                                        console.error("[PAUSE] HTTP", xhr.status, xhr.responseText)
                                        return
                                    }
                                    try {
                                        var res = JSON.parse(xhr.responseText)
                                        if (res.status === "success" && res.pin_code && typeof root !== 'undefined') {
                                            root.sessionUserBeforePause = root.sessionUser
                                            root.temporaryPausePin = String(res.pin_code)
                                            root.sessionUser = "PAUSE"
                                            console.log("[PAUSE] OK, одноразовый PIN выдан")
                                        } else {
                                            console.error("[PAUSE] отказ:", res.message || xhr.responseText)
                                        }
                                    } catch (e) {
                                        console.error("[PAUSE] parse:", e)
                                    }
                                }
                                xhr.send(JSON.stringify({
                                    "computer_id": pcId,
                                    "booking_id": (typeof NetworkManager !== 'undefined') ? NetworkManager.lastBookingId : 0
                                }))
                            }
                        }
                        ActionBtn {
                            text: "ЗАКРЫТЬ СЕССИЮ"
                            icon: "🚪"
                            baseColor: "#525252"
                            onClicked: {
                                if (typeof NetworkManager !== "undefined") NetworkManager.logoutTerminal(dashboardRoot.termId)
                                if (typeof root !== 'undefined') root.sessionUser = ""
                                dashboardRoot.visible = false
                            }
                        }
                    }

                    Item { height: 10; width: 1 }

                    Rectangle {
                        id: volumeLangBox
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        color: "#0a0f0b"
                        border.color: "#162e1a"
                        radius: 4
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 12
                            RowLayout {
                                spacing: 6
                                Text { text: "🔊"; font.pixelSize: 12 }
                                Item {
                                    id: customSlider
                                    width: 90
                                    height: 20
                                    property int value: 50
                                    Rectangle {
                                        width: parent.width
                                        height: 4
                                        radius: 2
                                        color: "#222"
                                        anchors.verticalCenter: parent.verticalCenter
                                        Rectangle { width: (customSlider.value / 100) * parent.width; height: parent.height; color: accentColor; radius: 2 }
                                    }
                                    Rectangle { id: handleItem; x: (customSlider.value / 100) * (parent.width - width); anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; radius: 5; color: "white" }
                                    MouseArea {
                                        anchors.fill: parent
                                        property bool isDragging: false
                                        function updateVolume(mx) {
                                            var pct = Math.max(0, Math.min(1, mx / width))
                                            customSlider.value = Math.round(pct * 100)
                                            if (typeof Launcher !== 'undefined') Launcher.setSystemVolume(customSlider.value)
                                        }
                                        onPressed: function(mouse) { isDragging = true; updateVolume(mouse.x); }
                                        onPositionChanged: function(mouse) { if (isDragging) updateVolume(mouse.x); }
                                        onReleased: function(mouse) { isDragging = false; }
                                    }
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Rectangle {
                                width: 35
                                height: 22
                                color: "#111"
                                border.color: "#333"
                                radius: 3
                                Text { anchors.centerIn: parent; text: dashboardRoot.currentLanguage; color: "white"; font.pixelSize: 11; font.bold: true }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { dashboardRoot.currentLanguage = (dashboardRoot.currentLanguage === "RU") ? "EN" : "RU"; if (typeof Launcher !== 'undefined') Launcher.toggleSystemLanguage(); } }
                            }
                            Text {
                                id: sysClock
                                width: 90
                                height: 20
                                text: Qt.formatDateTime(new Date(), "hh:mm")
                                color: "white"
                                font.pixelSize: 13
                                font.bold: true
                                font.family: "Monospace"
                                Timer {
                                    interval: 1000
                                    running: true
                                    repeat: true
                                    onTriggered: { sysClock.text = Qt.formatDateTime(new Date(), "hh:mm") }
                                }
                            }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 25
            Text { text: "БИБЛИОТЕКА ИГР"; color: "white"; font.pixelSize: 24; font.bold: true; font.letterSpacing: 2 }
            Row {
                id: filterRow
                Layout.fillWidth: true
                spacing: 30
                property string activeTab: "ВСЕ ИГРЫ"
                Repeater {
                    model: ["ВСЕ ИГРЫ", "STEAM", "EPIC", "EA", "RIOT", "БРАУЗЕРЫ", "УТИЛИТЫ"]
                    delegate: Text {
                        text: modelData
                        color: filterRow.activeTab === modelData ? accentColor : "#666666"
                        font.pixelSize: 16
                        font.bold: true
                        font.letterSpacing: 1
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { filterRow.activeTab = modelData; if (typeof gamesModel !== 'undefined') gamesModel.setFilter(modelData) } }
                    }
                }
            }

            GridView {
                id: gamesGrid
                Layout.fillWidth: true
                Layout.fillHeight: true
                cellWidth: 230
                cellHeight: 320
                clip: true
                model: gamesModel

                delegate: Item {
                    width: gamesGrid.cellWidth
                    height: gamesGrid.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 10
                        color: "#0a0a0a"
                        radius: 6
                        border.width: gArea.containsMouse ? 2 : 1
                        border.color: gArea.containsMouse ? accentColor : "#1a1a1a"

                        Image {
                            width: parent.width
                            height: parent.height - 45
                            source: {
                                var pUrl = model.poster !== undefined ? model.poster : ""
                                if (pUrl === "") return ""
                                if (pUrl.indexOf("http") === 0 || pUrl.indexOf("file") === 0) return pUrl
                                var baseUrl = "http://192.168.222.2:22222"
                                if (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl !== 'undefined') {
                                    baseUrl = NetworkManager.serverUrl
                                }
                                return pUrl.indexOf("/") === 0 ? baseUrl + pUrl : baseUrl + "/" + pUrl
                            }
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            opacity: gArea.containsMouse ? 1.0 : 0.7
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 45
                            color: gArea.containsMouse ? accentColor : "#050505"
                            Text {
                                anchors.centerIn: parent
                                text: model.title || ""
                                color: gArea.containsMouse ? "black" : "white"
                                font.bold: true
                            }
                        }
                        MouseArea {
                            id: gArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                var currentGameId = model.id
                                var mPlat = model.platform || ""
                                var mTitle = model.title || ""
                                var mExe = model.exePath || ""
                                var mArgs = model.args || ""

                                if (dashboardRoot.looksLikeRiot(mPlat, mExe, mArgs, mTitle)) {
                                    dashboardRoot.pendingRiotGameId = parseInt(currentGameId)
                                    dashboardRoot.pendingRiotTitle = mTitle
                                    dashboardRoot.pendingRiotExe = mExe
                                    dashboardRoot.pendingRiotArgs = mArgs
                                    dashboardRoot.pendingRiotPlatform = mPlat || "riot"
                                    riotAccountPopup.open()
                                    return
                                }

                                if (typeof root !== 'undefined') {
                                    root.isLoggingIn = true
                                    root.currentGameId = parseInt(currentGameId)
                                    root.showGameLoading(mPlat, mTitle)
                                }
                                dashboardRoot.startClubTakeAccount(
                                    currentGameId, mPlat, mTitle, mExe, mArgs)
                            }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: storePopup
        width: 1500
        height: 880
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#050505"
            border.color: "#eab308"
            border.width: 2
            radius: 12
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 35
            spacing: 25

            RowLayout {
                Layout.fillWidth: true

                Column {
                    Text {
                        text: "REACTOR MARKET"
                        color: "#eab308"
                        font.pixelSize: 36
                        font.bold: true
                        font.italic: true
                    }
                    Text {
                        text: "СНАРЯЖЕНИЕ И ПРОВИЗИЯ ДЛЯ РЕЙДА"
                        color: "#eab308"
                        opacity: 0.5
                        font.pixelSize: 10
                        font.letterSpacing: 4
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 45
                    height: 45
                    color: "transparent"
                    border.color: "#222"
                    radius: 22

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: "white"
                        font.pixelSize: 18
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: storePopup.close()
                    }
                }
            }

            Row {
                id: filterCatRow
                spacing: 15
                property string activeCat: "Все"

                Repeater {
                    model: [
                        { name: "Все", tag: "" },
                        { name: "Напитки", tag: "drinks" },
                        { name: "Снэки", tag: "food" }
                    ]
                    delegate: Rectangle {
                        width: 120
                        height: 38
                        radius: 6
                        color: filterCatRow.activeCat === modelData.name ? "#eab308" : "#111"

                        Text {
                            anchors.centerIn: parent
                            text: modelData.name
                            color: filterCatRow.activeCat === modelData.name ? "black" : "white"
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                filterCatRow.activeCat = modelData.name
                                if (typeof storeModel !== 'undefined')
                                    storeModel.setFilter(modelData.tag)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 30

                GridView {
                    id: storeGrid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    cellWidth: 250
                    cellHeight: 330
                    clip: true
                    model: storeModel

                    delegate: Rectangle {
                        width: storeGrid.cellWidth - 15
                        height: storeGrid.cellHeight - 15
                        color: "#0a0a0a"
                        radius: 10
                        opacity: model.stock > 0 ? 1.0 : 0.35

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 140
                                color: "#111"
                                radius: 6
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 5
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    source: {
                                        var imgUrl = model.image || ""
                                        if (imgUrl === "")
                                            return ""
                                        if (imgUrl.indexOf("http") === 0 || imgUrl.indexOf("file") === 0)
                                            return imgUrl
                                        var baseUrl = "http://192.168.222.2:22222"
                                        if (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl !== 'undefined')
                                            baseUrl = NetworkManager.serverUrl
                                        return imgUrl.indexOf("/") === 0 ? baseUrl + imgUrl : baseUrl + "/" + imgUrl
                                    }
                                }
                                Text {
                                    visible: !model.image
                                    anchors.centerIn: parent
                                    text: "📦"
                                    font.pixelSize: 36
                                }
                            }

                            Text {
                                text: model.name || ""
                                color: "white"
                                font.bold: true
                                font.pixelSize: 15
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Item { Layout.fillHeight: true }

                            RowLayout {
                                Layout.fillWidth: true

                                Text {
                                    text: parseFloat(model.price || 0).toFixed(0) + " ₽"
                                    color: "#eab308"
                                    font.pixelSize: 22
                                    font.bold: true
                                }

                                Item { Layout.fillWidth: true }

                                Rectangle {
                                    visible: model.stock > 0
                                    width: 44
                                    height: 44
                                    radius: 8
                                    color: itemMouse.containsMouse ? "#ffffff" : "#eab308"

                                    Text {
                                        anchors.centerIn: parent
                                        text: "+"
                                        color: "black"
                                        font.pixelSize: 22
                                        font.bold: true
                                    }
                                    MouseArea {
                                        id: itemMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: cartModel.addProduct(model.id, model.name, model.price)
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    id: cartBoxContainer
                    Layout.preferredWidth: 400
                    Layout.fillHeight: true
                    color: "#0a0a0a"
                    border.color: "#1c1c1c"
                    radius: 8

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 15

                        Text {
                            text: "КОРЗИНА ЗАКАЗА (" + cartModel.count + ")"
                            color: "white"
                            font.bold: true
                        }

                        ListView {
                            id: cartListView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: cartModel
                            spacing: 10

                            delegate: Rectangle {
                                width: cartListView.width
                                height: 70
                                color: "#111"
                                radius: 6

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12

                                    Column {
                                        Layout.fillWidth: true

                                        Text {
                                            text: model.name
                                            color: "white"
                                            font.bold: true
                                            elide: Text.ElideRight
                                            width: 170
                                        }
                                        Text {
                                            text: (model.price * model.quantity).toFixed(0) + " ₽"
                                            color: "#eab308"
                                        }
                                    }

                                    Row {
                                        spacing: 5

                                        Rectangle {
                                            width: 28
                                            height: 28
                                            radius: 4
                                            color: "#222"

                                            Text {
                                                anchors.centerIn: parent
                                                text: "-"
                                                color: "white"
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (model.quantity > 1)
                                                        cartModel.setProperty(index, "quantity", model.quantity - 1)
                                                    else
                                                        cartModel.remove(index)
                                                }
                                            }
                                        }

                                        Text {
                                            text: model.quantity
                                            color: "white"
                                            font.bold: true
                                            width: 24
                                            horizontalAlignment: Text.AlignHCenter
                                            anchors.verticalCenter: parent.verticalCenter
                                        }

                                        Rectangle {
                                            width: 28
                                            height: 28
                                            radius: 4
                                            color: "#222"

                                            Text {
                                                anchors.centerIn: parent
                                                text: "+"
                                                color: "white"
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: cartModel.setProperty(index, "quantity", model.quantity + 1)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: checkoutBtnBox
                            Layout.fillWidth: true
                            Layout.preferredHeight: 55
                            radius: 8
                            color: cartModel.count > 0 ? "#eab308" : "#222"

                            Text {
                                anchors.centerIn: parent
                                text: "ОФОРМИТЬ ЗАКАЗ"
                                color: "black"
                                font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: cartModel.count > 0
                                onClicked: {
                                    var baseUrl = "http://192.168.222.2:22222"
                                    if (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl !== 'undefined')
                                        baseUrl = NetworkManager.serverUrl
                                    for (var i = 0; i < cartModel.count; i++) {
                                        var item = cartModel.get(i)
                                        for (var q = 0; q < item.quantity; q++) {
                                            var xhr = new XMLHttpRequest()
                                            xhr.open("POST", baseUrl + "/api/shell/store/checkout")
                                            xhr.setRequestHeader("Content-Type", "application/json")
                                            xhr.send(JSON.stringify({
                                                "product_id": item.productId,
                                                "terminal_id": dashboardRoot.termId
                                            }))
                                        }
                                    }
                                    cartModel.clear()
                                    storePopup.close()
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: depositPopup; width: 500; height: 520; anchors.centerIn: parent; modal: true; focus: true
        background: Rectangle { color: "#050505"; border.color: "#eab308"; radius: 8 }
        property string qrSourceUrl: ""
        property int selectedAmount: 100
        property bool showQrScreen: false

        ColumnLayout {
            anchors.fill: parent; anchors.margins: 30; spacing: 20
            Text { text: "ПОПОЛНЕНИЕ БАЛАНСА"; color: "#eab308"; font.bold: true }
            ColumnLayout {
                visible: !depositPopup.showQrScreen; Layout.fillWidth: true; spacing: 15
                RowLayout {
                    spacing: 10; Layout.fillWidth: true
                    Repeater {
                        model: [100, 300, 500, 1000]
                        delegate: Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 45; radius: 4; color: depositPopup.selectedAmount === modelData ? "#eab308" : "#0d0d0d"
                            Text { anchors.centerIn: parent; text: modelData + " ₽"; color: "white" }
                            MouseArea { anchors.fill: parent; onClicked: depositPopup.selectedAmount = modelData }
                        }
                    }
                }
                Button {
                    Layout.fillWidth: true; text: "Получить QR-код СБП"
                    onClicked: {
                        var baseUrl = "http://192.168.222.2:22222"
                        if (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl !== 'undefined') {
                            baseUrl = NetworkManager.serverUrl
                        }
                        depositPopup.qrSourceUrl = baseUrl + "/api/payments/sbp-mock-qr?amount=" + depositPopup.selectedAmount + "&computer_id=" + dashboardRoot.termId
                        depositPopup.showQrScreen = true
                    }
                }
            }
            ColumnLayout {
                visible: depositPopup.showQrScreen; Layout.fillWidth: true
                Rectangle { Layout.alignment: Qt.AlignHCenter; width: 200; height: 220; Image { anchors.fill: parent; source: depositPopup.qrSourceUrl; fillMode: Image.PreserveAspectFit } }
                Button { Layout.alignment: Qt.AlignHCenter; text: "Назад"; onClicked: depositPopup.showQrScreen = false }
            }
        }
    }

    Popup {
        id: steamLimitAlertPopup; width: 600; height: 380; anchors.centerIn: parent; modal: true
        background: Rectangle { color: "#0a0505"; border.color: accentColor; radius: 8 }
        property string targetExe: ""; property string targetArgs: ""
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 30; spacing: 15
            Text { text: "⚙️ СИСТЕМА КОНТРОЛЯ ТРАФИКА"; color: accentColor; font.bold: true; Layout.alignment: Qt.AlignHCenter }
            Button { text: "ПОНЯТНО, ЗАПУСТИТЬ"; Layout.alignment: Qt.AlignHCenter; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch(steamLimitAlertPopup.targetExe, steamLimitAlertPopup.targetArgs, "", "", ""); steamLimitAlertPopup.close() } }
        }
    }

    Popup {
        id: riotAccountPopup
        width: Math.min(640, parent.width - 40)
        height: 420
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#0a0505"
            border.color: "#d32f2f"
            radius: 10
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 16

            Text {
                text: "RIOT GAMES"
                color: "#d32f2f"
                font.pixelSize: 22
                font.bold: true
                font.letterSpacing: 2
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: dashboardRoot.pendingRiotTitle.length > 0
                      ? dashboardRoot.pendingRiotTitle : "Игра Riot"
                color: "#aaaaaa"
                font.pixelSize: 14
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: "#e5e5e5"
                font.pixelSize: 16
                lineHeight: 1.35
                text: "У вас есть личный аккаунт Riot?\n\n"
                      + "Личный — сохранит ранг, скины и прогресс.\n"
                      + "Клубный — гостевой вход из пула клуба (если свободны)."
            }
            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 6
                    color: "#d32f2f"
                    Text {
                        anchors.centerIn: parent
                        text: "СВОЙ АККАУНТ"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 14
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dashboardRoot.launchRiotPersonal()
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 6
                    color: "transparent"
                    border.color: accentColor
                    border.width: 2
                    Text {
                        anchors.centerIn: parent
                        text: "КЛУБНЫЙ АККАУНТ"
                        color: accentColor
                        font.bold: true
                        font.pixelSize: 14
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dashboardRoot.launchRiotClub()
                    }
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Отмена"
                color: "#666666"
                font.pixelSize: 13
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: riotAccountPopup.close()
                }
            }
        }
    }

    Popup {
        id: riotBusyHintPopup
        width: Math.min(560, parent.width - 40)
        height: 280
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#0a0505"
            border.color: "#d32f2f"
            radius: 10
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 18
            Text {
                text: "КЛУБНЫЕ АККАУНТЫ ЗАНЯТЫ"
                color: "#d32f2f"
                font.bold: true
                font.pixelSize: 18
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: "#cccccc"
                font.pixelSize: 15
                text: "Сейчас нет свободного клубного аккаунта Riot.\nМожно войти под своим — прогресс и скины останутся у вас."
            }
            Item { Layout.fillHeight: true }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                radius: 6
                color: "#d32f2f"
                Text {
                    anchors.centerIn: parent
                    text: "ВОЙТИ ПОД СВОИМ"
                    color: "white"
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        riotBusyHintPopup.close()
                        dashboardRoot.launchRiotPersonal()
                    }
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Закрыть"
                color: "#666666"
                MouseArea {
                    anchors.fill: parent
                    onClicked: riotBusyHintPopup.close()
                }
            }
        }
    }

    component ActionBtn : Rectangle {
        id: controlRoot
        property string text: "BUTTON"
        property string icon: ""
        property string baseColor: accentColor
        property bool isActiveStatus: false
        property bool orderIsFinished: false
        property string statusText: ""
        signal clicked()

        Layout.fillWidth: true
        Layout.preferredHeight: 50
        radius: 4
        color: "transparent"
        border.color: baseColor

        Row {
            anchors.centerIn: parent
            spacing: 6
            Text {
                text: icon
            }
            Text {
                text: controlRoot.text
                color: "white"
                font.bold: true
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                controlRoot.clicked()
            }
        }
    }

    component PlatformSquareBtn : Rectangle {
        property string btnText: "LAUNCH"
        property string iconText: "🎮"
        property string brandColor: accentColor
        signal clicked()

        Layout.fillWidth: true
        Layout.preferredHeight: 65
        radius: 4
        color: "#0a0d0b"
        border.color: brandColor

        Column {
            anchors.centerIn: parent
            spacing: 4
            Text {
                text: iconText
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                text: btnText
                color: "white"
                font.pixelSize: 10
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                parent.clicked()
            }
        }
    }
}
