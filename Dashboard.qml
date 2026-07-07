// Путь: Dashboard.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import Qt5Compat.GraphicalEffects

Item {
    id: dashboardRoot
    anchors.fill: parent

    property string userName: (typeof root !== 'undefined') ? root.sessionUser : "PLAYER_1"
    property real userBalance: (typeof root !== 'undefined') ? root.sessionBalance : 0.0
    property string timeRemaining: (typeof root !== 'undefined') ? root.sessionTime : "00:00:00"

    property int termId: (typeof root !== 'undefined' && root !== null) ? root.terminalId : 0
    property string pcType: (typeof root !== 'undefined' && root !== null) ? root.pcTypeFromDatabase : "standard"

    property bool isProBootcamp: {
        var typeLower = pcType.toLowerCase();
        return typeLower === "pro" || typeLower === "bootcamp" || typeLower === "trio" || typeLower === "vip";
    }

    property string zoneTitle: {
        if (pcType.toLowerCase() === "trio") return "TRIO ZONE"
        if (pcType.toLowerCase() === "vip") return "VIP ZONE"
        return isProBootcamp ? "PRO BOOTCAMP ZONE" : "STANDARD ZONE"
    }

    property string accentColor: isProBootcamp ? "#a855f7" : "#22c55e"
    property string darkBg: "#030704"
    property string currentLanguage: "RU"

    Component.onCompleted: {
        if (typeof NetworkManager !== 'undefined') {
            NetworkManager.fetchProducts();
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
            append({
                "productId": prodId,
                "name": prodName,
                "price": parsedPrice,
                "quantity": 1
            });
        }
    }

    Rectangle {
        id: bgContainer
        anchors.fill: parent
        color: "#020202"

        Image {
            anchors.fill: parent
            source: "images/hex_bg.png"
            fillMode: Image.Tile
            opacity: 0.15
            horizontalAlignment: Image.AlignHCenter
            verticalAlignment: Image.AlignVCenter
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

        // ЛЕВАЯ ПАНЕЛЬ С КНОПКАМИ И ИНДИКАТОРОМ ЗАКАЗА
        Rectangle {
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
                        Text {
                            text: "SOS"
                            color: sosMouse.containsMouse ? "black" : "white"
                            font.pixelSize: 12
                            font.bold: true
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }

                    MouseArea {
                        id: sosMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof NetworkManager !== 'undefined') NetworkManager.callAdmin(dashboardRoot.termId);
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
                        Text {
                            text: "LATENCY (EU): " + (typeof NetworkManager !== 'undefined' ? NetworkManager.getLatency("162.249.72.1") : 24) + " MS"
                            color: accentColor
                            font.pixelSize: 9
                            font.bold: true
                            opacity: 0.5
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 35
                        color: "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            text: dashboardRoot.zoneTitle
                            color: accentColor
                            font.pixelSize: 14
                            font.bold: true
                            font.letterSpacing: 2
                        }
                    }

                    Column {
                        Layout.fillWidth: true
                        spacing: 5
                        Text { text: "ПОЛЬЗОВАТЕЛЬ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                        Text { text: dashboardRoot.userName; color: "white"; font.pixelSize: 28; font.bold: true }
                        Item { height: 10; width: 1 }
                        Text { text: "ОСТАЛОСЬ ВРЕМЕНИ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                        Text {
                            text: dashboardRoot.timeRemaining
                            color: "white"
                            font.pixelSize: 52
                            font.family: "Monospace"
                            font.bold: true
                        }
                        Text { text: "БАЛАНС: " + dashboardRoot.userBalance.toFixed(2) + " ₽"; color: "#a3a3a3"; font.pixelSize: 18 }
                    }

                    Item { Layout.fillHeight: true }

                    Text {
                        text: "БЫСТРЫЙ ЗАПУСК ПЛАТФОРМ"
                        color: accentColor
                        font.pixelSize: 10; font.bold: true; font.letterSpacing: 2; opacity: 0.6; Layout.alignment: Qt.AlignHCenter
                    }

                    GridLayout {
                        columns: 3; rows: 2; columnSpacing: 10; rowSpacing: 10
                        Layout.fillWidth: true

                        PlatformSquareBtn {
                            btnText: "STEAM"; iconText: "󰓓"; brandColor: "#00adef";
                            onClicked: {
                                steamLimitAlertPopup.targetExe = "C:\\Program Files (x86)\\Steam\\steam.exe";
                                steamLimitAlertPopup.targetArgs = "";
                                steamLimitAlertPopup.open();
                            }
                        }
                        PlatformSquareBtn { btnText: "EPIC"; iconText: "󰊗"; brandColor: "#ffffff"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe", ""); } }
                        PlatformSquareBtn { btnText: "ROBLOX"; iconText: "󰩊"; brandColor: "#e11d48"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Users\\Public\\Desktop\\Roblox Player.lnk", ""); } }
                        PlatformSquareBtn { btnText: "RIOT"; iconText: "󰊴"; brandColor: "#d32f2f"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Riot Games\\Riot Client\\RiotClientServices.exe", ""); } }
                        PlatformSquareBtn { btnText: "EA APP"; iconText: "󰓡"; brandColor: "#ff5722"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe", ""); } }
                        PlatformSquareBtn { btnText: "VK PLAY"; iconText: "󰕼"; brandColor: "#ff3347"; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch("C:\\Users\\Public\\Desktop\\VK Play.lnk", ""); } }
                    }

                    Item { height: 5; width: 1 }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        ActionBtn {
                            id: storeActionBtn
                            text: "МАГАЗИН"
                            icon: "🛒"
                            baseColor: "#eab308"
                            isActiveStatus: (typeof root !== 'undefined') ? root.hasActiveOrder : false
                            orderIsFinished: (typeof root !== 'undefined' && root.orderStatusText === "ЗАКАЗ ВЫПОЛНЕН")
                            statusText: (typeof root !== 'undefined' && root.hasActiveOrder) ? root.orderStatusText : ""
                            onClicked: storePopup.open()
                        }

                        ActionBtn {
                            text: "ПОПОЛНИТЬ БАЛАНС"
                            icon: "💳"
                            baseColor: "#eab308"
                            onClicked: depositPopup.open()
                        }

                        ActionBtn {
                            text: "ОТОЙТИ (ПАУЗА)"
                            icon: "⏳"
                            baseColor: "#3b82f6"
                            onClicked: {
                                var baseUrl = (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : "http://192.168.222.2:22222";
                                var xhr = new XMLHttpRequest();
                                xhr.open("POST", baseUrl + "/api/shell/games/pause");
                                xhr.setRequestHeader("Content-Type", "application/json");
                                xhr.onreadystatechange = function() {
                                    if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                                        var res = JSON.parse(xhr.responseText);
                                        if (res.status === "success") {
                                            if (typeof root !== 'undefined') {
                                                root.temporaryPausePin = res.pin_code;
                                                root.sessionUser = "PAUSE";
                                                dashboardRoot.visible = false;
                                            }
                                        }
                                    }
                                }
                                xhr.send(JSON.stringify({ "computer_id": dashboardRoot.termId }));
                            }
                        }

                        ActionBtn {
                            text: "ЗАКРЫТЬ СЕССИЮ"
                            icon: "🚪"
                            baseColor: "#525252"
                            onClicked: {
                                console.log("[DASHBOARD-LOGOUT] Инициация выхода для терминала ID:", dashboardRoot.termId);

                                if (typeof NetworkManager !== "undefined") {
                                    NetworkManager.logoutTerminal(dashboardRoot.termId);
                                }

                                if (typeof root !== 'undefined') {
                                    root.sessionUser = "";
                                }
                                dashboardRoot.visible = false;
                            }
                        }
                    }

                    Item { height: 10; width: 1 }

                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 40
                        color: "#0a0f0b"; border.color: "#162e1a"; radius: 4

                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 12

                            RowLayout {
                                spacing: 6
                                Text { text: "🔊"; font.pixelSize: 12 }
                                Item {
                                    id: customSlider; width: 90; height: 20
                                    property int value: 50
                                    Rectangle {
                                        width: parent.width; height: 4; radius: 2; color: "#222"
                                        anchors.verticalCenter: parent.verticalCenter
                                        Rectangle { width: (customSlider.value / 100) * parent.width; height: parent.height; color: accentColor; radius: 2 }
                                    }
                                    Rectangle { id: handleItem; x: (customSlider.value / 100) * (parent.width - width); anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; radius: 5; color: "white" }
                                    MouseArea {
                                        anchors.fill: parent; property bool isDragging: false
                                        function updateVolume(mx) {
                                            var pct = Math.max(0, Math.min(1, mx / width));
                                            customSlider.value = Math.round(pct * 100);
                                            if (typeof Launcher !== 'undefined') Launcher.setSystemVolume(customSlider.value);
                                        }
                                        onPressed: function(mouse) { isDragging = true; updateVolume(mouse.x); }
                                        onPositionChanged: function(mouse) { if (isDragging) updateVolume(mouse.x); }
                                        onReleased: function(mouse) { isDragging = false; }
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Rectangle {
                                width: 35; height: 22; color: "#111"; border.color: "#333"; radius: 3
                                Text { anchors.centerIn: parent; text: dashboardRoot.currentLanguage; color: "white"; font.pixelSize: 11; font.bold: true }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor;
                                    onClicked: {
                                        dashboardRoot.currentLanguage = (dashboardRoot.currentLanguage === "RU") ? "EN" : "RU";
                                        if (typeof Launcher !== 'undefined') Launcher.toggleSystemLanguage();
                                    }
                                }
                            }

                            Text {
                                id: sysClock; text: Qt.formatDateTime(new Date(), "hh:mm"); color: "white"; font.pixelSize: 13; font.bold: true; font.family: "Monospace"
                                Timer { interval: 1000; running: true; repeat: true; onTriggered: sysClock.text = Qt.formatDateTime(new Date(), "hh:mm") }
                            }
                        }
                    }
                }
            }
        }

        // ПРАВАЯ ПАНЕЛЬ (БИБЛИОТЕКА ИГР)
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 25
            Text { text: "БИБЛИОТЕКА ИГР"; color: "white"; font.pixelSize: 24; font.bold: true; font.letterSpacing: 2 }
            Row {
                id: filterRow; Layout.fillWidth: true; spacing: 30; property string activeTab: "ВСЕ ИГРЫ"
                Repeater {
                    model: ["ВСЕ ИГРЫ", "STEAM", "EPIC", "БРАУЗЕРЫ", "УТИЛИТЫ"]
                    delegate: Text {
                        text: modelData; color: filterRow.activeTab === modelData ? accentColor : "#666666"; font.pixelSize: 16; font.bold: true; font.letterSpacing: 1
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor;
                            onClicked: {
                                filterRow.activeTab = modelData;
                                if (typeof gamesModel !== 'undefined') gamesModel.setFilter(modelData);
                            }
                        }
                    }
                }
            }

            GridView {
                id: gamesGrid; Layout.fillWidth: true; Layout.fillHeight: true; cellWidth: 230; cellHeight: 320; clip: true
                model: (typeof gamesModel !== 'undefined') ? gamesModel : null
                delegate: Item {
                    width: gamesGrid.cellWidth; height: gamesGrid.cellHeight
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 10; color: "#0a0a0a"; radius: 6
                        border.width: gArea.containsMouse ? 2 : 1
                        border.color: gArea.containsMouse ? accentColor : "#1a1a1a"
                        Image {
                            width: parent.width; height: parent.height - 45
                            source: {
                                var pUrl = model.poster !== undefined ? model.poster : ""; if (pUrl === "") return "";
                                if (pUrl.indexOf("http") === 0 || pUrl.indexOf("file") === 0) return pUrl;
                                var baseUrl = (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : "http://192.168.222.2:22222";
                                if (pUrl.indexOf("/") === 0) return baseUrl + pUrl;
                                return baseUrl + "/" + pUrl;
                            }
                            fillMode: Image.PreserveAspectCrop; asynchronous: true; opacity: gArea.containsMouse ? 1.0 : 0.7
                        }
                        Rectangle {
                            width: parent.width; height: 45; anchors.bottom: parent.bottom; color: gArea.containsMouse ? accentColor : "#050505"
                            Text { anchors.centerIn: parent; text: model.title || ""; color: gArea.containsMouse ? "black" : "white"; font.bold: true }
                        }
                        MouseArea {
                            id: gArea; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                var currentGameId = model.id;
                                var defaultExe = model.exePath;
                                var defaultArgs = model.args;
                                console.log("[LAUNCH-TRACE] Запрос клубного аккаунта для Game ID:", currentGameId);

                                var baseUrl = (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : "http://192.168.222.2:22222";
                                var xhr = new XMLHttpRequest();
                                xhr.open("POST", baseUrl + "/api/shell/games/take-account");
                                xhr.setRequestHeader("Content-Type", "application/json");
                                xhr.onreadystatechange = function() {
                                    if (xhr.readyState === XMLHttpRequest.DONE) {
                                        if (xhr.status === 200) {
                                            var res = JSON.parse(xhr.responseText);
                                            if (res.status === "success" && res.login) {
                                                var clubArgs = "-login " + res.login + " " + res.password + " " + (res.args ? res.args : defaultArgs);
                                                if (typeof Launcher !== 'undefined') Launcher.launch(res.exe_path ? res.exe_path : defaultExe, clubArgs);
                                            } else {
                                                if (typeof Launcher !== 'undefined') Launcher.launch(defaultExe, defaultArgs);
                                            }
                                        } else {
                                            if (typeof Launcher !== 'undefined') Launcher.launch(defaultExe, defaultArgs);
                                        }
                                    }
                                }
                                xhr.send(JSON.stringify({ "game_id": parseInt(currentGameId), "terminal_id": parseInt(dashboardRoot.termId) }));
                            }
                        }
                    }
                }
            }
        }
    }

    // ==========================================
    // ВСПЛЫВАЮЩЕЕ ОКНО МАРКЕТА
    // ==========================================
    Popup {
        id: storePopup
        width: 1500; height: 880; anchors.centerIn: parent; modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#000000"; opacity: 0.85 }
        background: Rectangle { color: "#050505"; border.color: "#eab308"; border.width: 2; radius: 12 }

        ColumnLayout {
            anchors.fill: parent; anchors.margins: 35; spacing: 25
            RowLayout {
                Layout.fillWidth: true
                Column {
                    Text { text: "REACTOR MARKET"; color: "#eab308"; font.pixelSize: 36; font.bold: true; font.italic: true }
                    Text { text: "СНАРЯЖЕНИЕ И ПРОВИЗИЯ ДЛЯ РЕЙДА"; color: "#eab308"; opacity: 0.5; font.pixelSize: 10; font.letterSpacing: 4 }
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 45; height: 45; color: "transparent"; border.color: "#222"; radius: 22
                    Text { anchors.centerIn: parent; text: "✕"; color: "white"; font.pixelSize: 18 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: storePopup.close() }
                }
            }

            Row {
                id: filterCatRow; spacing: 15; property string activeCat: "Все"
                Repeater {
                    model: [{ name: "Все", tag: "" }, { name: "Напитки", tag: "drinks" }, { name: "Снэки", tag: "food" }]
                    delegate: Rectangle {
                        width: 120; height: 38; radius: 6; color: filterCatRow.activeCat === modelData.name ? "#eab308" : "#111"
                        Text { anchors.centerIn: parent; text: modelData.name; color: filterCatRow.activeCat === modelData.name ? "black" : "white"; font.bold: true; font.pixelSize: 13 }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor;
                            onClicked: {
                                filterCatRow.activeCat = modelData.name;
                                if (typeof storeModel !== 'undefined') storeModel.setFilter(modelData.tag);
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 30

                GridView {
                    id: storeGrid; Layout.fillWidth: true; Layout.fillHeight: true; cellWidth: 250; cellHeight: 330; clip: true;
                    model: (typeof storeModel !== 'undefined') ? storeModel : null
                    delegate: Rectangle {
                        width: storeGrid.cellWidth - 15; height: storeGrid.cellHeight - 15; color: "#0a0a0a"; border.color: "#1c1c1c"; border.width: 1; radius: 10
                        opacity: model.stock > 0 ? 1.0 : 0.35

                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 15
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 140; color: "#111"; radius: 6; clip: true
                                Image {
                                    anchors.fill: parent; anchors.margins: 5
                                    source: {
                                        var imgUrl = model.image || ""; if (imgUrl === "") return "";
                                        if (imgUrl.indexOf("http") === 0 || imgUrl.indexOf("file") === 0) return imgUrl;
                                        var baseUrl = (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : "http://192.168.222.2:22222";
                                        if (imgUrl.indexOf("/") === 0) return baseUrl + imgUrl;
                                        return baseUrl + "/" + imgUrl;
                                    }
                                    fillMode: Image.PreserveAspectFit; asynchronous: true
                                }
                                Text { visible: !model.image; anchors.centerIn: parent; text: "📦"; font.pixelSize: 36 }
                            }
                            Text { text: model.name || ""; color: "white"; font.bold: true; font.pixelSize: 15; Layout.topMargin: 5; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text { text: (model.category || "BAR").toUpperCase(); color: "#555"; font.pixelSize: 9; font.letterSpacing: 2; font.bold: true }
                            Item { Layout.fillHeight: true }

                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: parseFloat(model.price || 0).toFixed(0) + " ₽"
                                    color: "#eab308"; font.pixelSize: 22; font.bold: true; font.italic: true
                                }
                                Item { Layout.fillWidth: true }
                                Rectangle {
                                    visible: model.stock > 0
                                    width: 44; height: 44; radius: 8; color: itemMouse.containsMouse ? "#ffffff" : "#eab308"
                                    Text { anchors.centerIn: parent; text: "+"; color: "black"; font.pixelSize: 22; font.bold: true }
                                    MouseArea {
                                        id: itemMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: cartModel.addProduct(model.id, model.name, model.price);
                                    }
                                }
                                Rectangle { visible: model.stock <= 0; width: 75; height: 28; radius: 4; color: "#300c0c"; border.color: "#991b1b"
                                    Text { anchors.centerIn: parent; text: "SOLD OUT"; color: "#ef4444"; font.pixelSize: 9; font.bold: true }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 400; Layout.fillHeight: true; color: "#0a0a0a"; border.color: "#1c1c1c"; radius: 8
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 20; spacing: 15
                        Text { text: "КОРЗИНА ЗАКАЗА (" + cartModel.count + ")"; color: "white"; font.pixelSize: 16; font.bold: true; font.letterSpacing: 1 }
                        ListView {
                            id: cartListView; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: cartModel; spacing: 10
                            delegate: Rectangle {
                                width: cartListView.width; height: 70; color: "#111"; radius: 6; border.color: "#222"
                                RowLayout {
                                    anchors.fill: parent; anchors.margins: 12; spacing: 10
                                    Column {
                                        Layout.fillWidth: true
                                        Text { text: model.name; color: "white"; font.bold: true; font.pixelSize: 13; elide: Text.ElideRight; width: 170 }
                                        Text { text: (model.price * model.quantity).toFixed(0) + " ₽"; color: "#eab308"; font.pixelSize: 12; font.bold: true }
                                    }
                                    Row {
                                        spacing: 5; Layout.alignment: Qt.AlignVCenter
                                        Rectangle {
                                            width: 28; height: 28; radius: 4; color: "#222"
                                            Text { anchors.centerIn: parent; text: "-"; color: "white"; font.bold: true }
                                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { if (model.quantity > 1) cartModel.setProperty(index, "quantity", model.quantity - 1); else cartModel.remove(index); } }
                                        }
                                        Text { text: model.quantity; color: "white"; font.pixelSize: 14; font.bold: true; width: 24; horizontalAlignment: Text.AlignHCenter; anchors.verticalCenter: parent.verticalCenter }
                                        Rectangle {
                                            width: 28; height: 28; radius: 4; color: "#222"
                                            Text { anchors.centerIn: parent; text: "+"; color: "white"; font.bold: true }
                                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: cartModel.setProperty(index, "quantity", model.quantity + 1) }
                                        }
                                    }
                                    Rectangle {
                                        width: 28; height: 28; color: "transparent"
                                        Text { anchors.centerIn: parent; text: "🗑"; color: "#ef4444"; font.pixelSize: 14 }
                                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: cartModel.remove(index) }
                                    }
                                }
                            }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#222" }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 12
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "ИТОГО К ОПЛАТЕ:"; color: "#888"; font.pixelSize: 12; font.bold: true }
                                Item { Layout.fillWidth: true }
                                Text { text: cartModel.updateTotalPrice() + " ₽"; color: "#eab308"; font.pixelSize: 24; font.bold: true; font.italic: true }
                            }
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 55; radius: 8; color: cartModel.count > 0 ? "#eab308" : "#222"; opacity: cartModel.count > 0 ? 1.0 : 0.4
                                Text { anchors.centerIn: parent; text: "ОФОРМИТЬ ЗАКАЗ"; color: cartModel.count > 0 ? "black" : "#666"; font.bold: true; font.pixelSize: 14; font.letterSpacing: 1 }
                                MouseArea {
                                    anchors.fill: parent; enabled: cartModel.count > 0; cursorShape: cartModel.count > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        var baseUrl = (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : "http://192.168.222.2:22222";
                                        for (var i = 0; i < cartModel.count; i++) {
                                            var item = cartModel.get(i);
                                            for (var q = 0; q < item.quantity; q++) {
                                                var xhr = new XMLHttpRequest();
                                                xhr.open("POST", baseUrl + "/api/shell/store/checkout");
                                                xhr.setRequestHeader("Content-Type", "application/json");
                                                xhr.send(JSON.stringify({ "product_id": item.productId, "terminal_id": dashboardRoot.termId }));
                                            }
                                        }
                                        cartModel.clear();
                                        storePopup.close();
                                        if (typeof NetworkManager !== 'undefined') NetworkManager.fetchProducts();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ==========================================
    // МОДАЛЬНОЕ ОКНО ОНЛАЙН-ПОПОЛНЕНИЯ БАЛАНСА ПО QR
    // ==========================================
    Popup {
        id: depositPopup
        width: 500; height: 520; anchors.centerIn: parent; modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#000000"; opacity: 0.85 }
        background: Rectangle { color: "#050505"; border.color: "#eab308"; border.width: 1; radius: 8 }

        property string qrSourceUrl: ""
        property int selectedAmount: 100
        property bool showQrScreen: false

        onClosed: {
            showQrScreen = false;
            qrSourceUrl = "";
        }

        ColumnLayout {
            anchors.fill: parent; anchors.margins: 30; spacing: 20

            RowLayout {
                Layout.fillWidth: true
                Text { text: "ПОПОЛНЕНИЕ БАЛАНСА"; color: "#eab308"; font.pixelSize: 18; font.bold: true; font.letterSpacing: 1 }
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 28; height: 28; color: "transparent"; border.color: "#222"; radius: 14
                    Text { anchors.centerIn: parent; text: "✕"; color: "#666"; font.pixelSize: 12 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: depositPopup.close() }
                }
            }

            ColumnLayout {
                visible: !depositPopup.showQrScreen
                Layout.fillWidth: true; spacing: 15
                Text { text: "Выберите сумму к зачислению:"; color: "#888"; font.pixelSize: 12 }

                RowLayout {
                    spacing: 10; Layout.fillWidth: true
                    Repeater {
                        model: [100, 300, 500, 1000]
                        delegate: Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 45; radius: 4
                            color: depositPopup.selectedAmount === modelData ? "#eab308" : "#0d0d0d"
                            border.color: depositPopup.selectedAmount === modelData ? "#eab308" : "#222"
                            Text { anchors.centerIn: parent; text: modelData + " ₽"; color: depositPopup.selectedAmount === modelData ? "black" : "white"; font.bold: true; font.pixelSize: 13 }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: depositPopup.selectedAmount = modelData }
                        }
                    }
                }

                Item { height: 15; width: 1 }

                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 55; radius: 6
                    color: "#111"; border.color: sbpMouse.containsMouse ? "#eab308" : "#222"
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 15; spacing: 15
                        Text { text: "📲"; font.pixelSize: 20 }
                        Text { text: "Получить QR-код на оплату"; color: "white"; font.bold: true; font.pixelSize: 14 }
                        Item { Layout.fillWidth: true }
                        Text { text: "⚡"; color: "#eab308" }
                    }
                    MouseArea {
                        id: sbpMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var baseUrl = (typeof NetworkManager !== 'undefined' && typeof NetworkManager.serverUrl === "function") ? NetworkManager.serverUrl() : "http://192.168.222.2:22222";
                            depositPopup.qrSourceUrl = baseUrl + "/api/payments/sbp-mock-qr?amount=" + depositPopup.selectedAmount + "&computer_id=" + dashboardRoot.termId + "&t=" + Date.now();
                            depositPopup.showQrScreen = true;
                        }
                    }
                }
            }

            ColumnLayout {
                visible: depositPopup.showQrScreen
                Layout.fillWidth: true; spacing: 15

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter; width: 220; height: 220; color: "white"; radius: 6
                    Image { anchors.fill: parent; anchors.margins: 10; source: depositPopup.qrSourceUrl; fillMode: Image.PreserveAspectFit; asynchronous: true }
                }

                Text { Layout.alignment: Qt.AlignHCenter; text: "Сумма к зачислению: " + depositPopup.selectedAmount + " ₽"; color: "#eab308"; font.pixelSize: 16; font.bold: true }
                Text { Layout.alignment: Qt.AlignHCenter; text: "Отсканируйте камерой смартфона для симуляции СБП.\nЗаглушка бэкенда начислит рубли автоматически."; color: "#555"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter }
                Item { height: 5; width: 1 }
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter; width: 140; height: 35; radius: 4; color: "#111"; border.color: "#333"
                    Text { anchors.centerIn: parent; text: "Назад к суммам"; color: "#aaa"; font.pixelSize: 12 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: depositPopup.showQrScreen = false }
                }
            }
        }
    }

    // ==========================================
    // ПОПАП ПРЕДУПРЕЖДЕНИЯ О ЛИМИТЕ СКАЧИВАНИЯ STEAM
    // ==========================================
    Popup {
        id: steamLimitAlertPopup
        width: 600; height: 380; anchors.centerIn: parent; modal: true; focus: true; z: 9999
        closePolicy: Popup.NoAutoClose
        Overlay.modal: Rectangle { color: "#000000"; opacity: 0.85 }
        background: Rectangle { color: "#0a0505"; border.color: accentColor; border.width: 1; radius: 8 }

        property string targetExe: ""
        property string targetArgs: ""

        ColumnLayout {
            anchors.fill: parent; anchors.margins: 30; spacing: 15

            Text { Layout.alignment: Qt.AlignHCenter; text: "⚙️ СИСТЕМА КОНТРОЛЯ ТРАФИКА"; color: accentColor; font.pixelSize: 20; font.bold: true; font.letterSpacing: 1 }
            Text { Layout.alignment: Qt.AlignHCenter; text: "Внимание: Активирован игровой режим сети"; color: "white"; font.pixelSize: 13; font.bold: true }
            Text { Layout.alignment: Qt.AlignHCenter; text: "Скорость скачивания и обновлений внутри Steam ограничена до 1 МБ/с.\nЭто необходимо для поддержания идеального пинга у всех игроков в клубе."; color: "#a3a3a3"; font.pixelSize: 12; font.bold: true; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }

            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 70; color: "#1a0505"; border.color: "#dc2626"; border.width: 1; radius: 4
                Layout.topMargin: 5; Layout.bottomMargin: 5

                Column {
                    anchors.centerIn: parent; spacing: 4
                    Text { text: "ЕСЛИ НУЖНОЙ ИГРЫ НЕТ НА КОМПЬЮТЕРЕ"; color: "#ef4444"; font.pixelSize: 15; font.bold: true; font.letterSpacing: 1; anchors.horizontalCenter: parent.horizontalCenter }
                    Text { text: "Воспользуйтесь услугой «Заказ предустановки» в личном кабинете,\nи мы скачаем её к вашему следующему визиту на максимальной скорости."; color: "#fca5a5"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; anchors.horizontalCenter: parent.horizontalCenter }
                }
            }

            Item { height: 5; Layout.fillHeight: true }

            Rectangle {
                Layout.alignment: Qt.AlignHCenter; width: 220; height: 45; radius: 4; color: accentColor
                Text { anchors.centerIn: parent; text: "ПОНЯТНО, ЗАПУСТИТЬ"; color: "black"; font.bold: true; font.pixelSize: 12 }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof Launcher !== 'undefined') Launcher.launch(steamLimitAlertPopup.targetExe, steamLimitAlertPopup.targetArgs);
                        steamLimitAlertPopup.close();
                    }
                }
            }
        }
    }

    // ==========================================
    // СТИЛИЗОВАННЫЕ КОМПОНЕНТЫ КНОПОК
    // ==========================================
    component ActionBtn : Rectangle {
        id: controlRoot
        property string text: "BUTTON"
        property string icon: ""
        property string baseColor: accentColor
        property bool isActiveStatus: false
        property bool orderIsFinished: false
        property string statusText: ""
        signal clicked()

        Layout.fillWidth: true; Layout.preferredHeight: 50; radius: 4
        color: bMouse.pressed ? baseColor : "transparent"
        border.color: bMouse.containsMouse || isActiveStatus ? baseColor : Qt.rgba(1,1,1,0.15)
        border.width: 1

        Row {
            anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 20; spacing: 6
            Text { text: icon; font.pixelSize: 18; anchors.verticalCenter: parent.verticalCenter }
            Text { text: controlRoot.text; font.bold: true; font.pixelSize: 14; color: bMouse.pressed ? "black" : (bMouse.containsMouse ? "white" : baseColor); anchors.verticalCenter: parent.verticalCenter }
            Text { text: " (" + controlRoot.statusText + ")"; font.bold: true; font.pixelSize: 14; visible: controlRoot.isActiveStatus && controlRoot.statusText !== ""; color: controlRoot.orderIsFinished ? "#22c55e" : "#ef4444"; anchors.verticalCenter: parent.verticalCenter }
        }
        MouseArea { id: bMouse; anchors.fill: parent; hoverEnabled: true; onClicked: controlRoot.clicked() }
    }

    component PlatformSquareBtn : Rectangle {
        property string btnText: "LAUNCH"; property string iconText: "🎮"; property string brandColor: accentColor
        signal clicked()
        Layout.fillWidth: true; Layout.preferredHeight: 65; radius: 4
        color: pMouse.containsMouse ? Qt.rgba(1,1,1,0.06) : "#0a0d0b"
        border.color: pMouse.containsMouse ? brandColor : "#1a1f1c"; border.width: 1
        Behavior on border.color { ColorAnimation { duration: 100 } }
        layer.enabled: pMouse.containsMouse
        layer.effect: MultiEffect { blurEnabled: true; blur: 0.2; brightness: 0.1 }
        Column {
            anchors.centerIn: parent; spacing: 6
            Text { text: iconText; color: pMouse.containsMouse ? brandColor : "#444"; font.pixelSize: 20; font.bold: true; anchors.horizontalCenter: parent.horizontalCenter }
            Text { text: btnText; color: pMouse.containsMouse ? "white" : "#666"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1; anchors.horizontalCenter: parent.horizontalCenter }
        }
        MouseArea { id: pMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
    }
}