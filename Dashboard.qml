// Путь: Dashboard.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Shapes

Item {
    id: dashboardRoot
    anchors.fill: parent

    // Данные профиля (заполняются автоматически из Main.qml при успешном логине)
    property string userName: "PLAYER_1"
    property real userBalance: 0
    property string timeRemaining: "00:00:00"

    // Принимаем числовой ID терминала и тип ПК напрямую из Main.qml
    property int termId: (typeof root !== 'undefined' && root !== null) ? root.terminalId : 0
    property string pcType: (typeof root !== 'undefined' && root !== null) ? root.pcTypeFromDatabase : "standard"

    // Динамическое определение PRO-зоны на основе ответа бэкенда
    property bool isProBootcamp: {
        var typeLower = pcType.toLowerCase();
        return typeLower === "pro" || typeLower === "bootcamp" || typeLower === "trio" || typeLower === "vip";
    }

    // Текст для отображения зоны
    property string zoneTitle: {
        if (pcType.toLowerCase() === "trio") return "TRIO ZONE";
        if (pcType.toLowerCase() === "vip") return "VIP ZONE";
        return isProBootcamp ? "PRO BOOTCAMP ZONE" : "STANDARD ZONE";
    }

    property string accentColor: isProBootcamp ? "#a855f7" : "#22c55e"
    property string darkBg: "#030704"

    // Глобальное свойство языка для UI
    property string currentLanguage: "RU"

    // --- ФОН ---
    Rectangle {
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

        Shape {
            anchors.fill: parent
            layer.enabled: true
            layer.effect: MultiEffect { blurEnabled: true; blur: 0.3 }
            ShapePath {
                fillGradient: RadialGradient {
                    centerX: dashboardRoot.width / 2
                    centerY: dashboardRoot.height / 2
                    centerRadius: Math.max(dashboardRoot.width, dashboardRoot.height) / 1.2
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: "black" }
                }
                PathRectangle { x: 0; y: 0; width: dashboardRoot.width; height: dashboardRoot.height }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 40

        // ==========================================
        // ЛЕВАЯ ПАНЕЛЬ (ПРОФИЛЬ ПОЛЬЗОВАТЕЛЯ)
        // ==========================================
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

                // ---------------------------------------------------------
                // КНОПКА "SOS" (ВЫЗОВ АДМИНИСТРАТОРА)
                // ---------------------------------------------------------
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
                        onClicked: NetManager.callAdmin(dashboardRoot.termId)
                    }
                }

                // ОСНОВНОЙ ВЕРТИКАЛЬНЫЙ СТЕК ПАНЕЛИ
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    // --- СТАТУС ПИНГА ---
                    Row {
                        Layout.alignment: Qt.AlignLeft
                        spacing: 8
                        Rectangle { width: 6; height: 6; radius: 3; color: accentColor; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            text: "LATENCY (EU): " + NetManager.getLatency("162.249.72.1") + " MS"
                            color: accentColor
                            font.pixelSize: 9
                            font.bold: true
                            opacity: 0.5
                        }
                    }

                    // Статус зоны
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

                    // Информация о сессии
                    Column {
                        Layout.fillWidth: true
                        spacing: 5

                        Text { text: "ПОЛЬЗОВАТЕЛЬ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                        Text { text: userName; color: "white"; font.pixelSize: 28; font.bold: true }

                        Item { height: 10; width: 1 }

                        Text { text: "ОСТАЛОСЬ ВРЕМЕНИ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                        Text {
                            text: timeRemaining
                            color: "white"
                            font.pixelSize: 52
                            font.family: "Monospace"
                            font.bold: true
                        }

                        Text { text: "БАЛАНС: " + userBalance.toFixed(2) + " ₽"; color: "#a3a3a3"; font.pixelSize: 18 }
                    }

                    Item { Layout.fillHeight: true }

                    // ==========================================
                    // БЛОК КВАДРАТНЫХ КНОПОК ПЛАТФОРМ (СЕТКА 3x2)
                    // ==========================================
                    Text {
                        text: "БЫСТРЫЙ ЗАПУСК ПЛАТФОРМ"
                        color: accentColor
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 2
                        opacity: 0.6
                        Layout.alignment: Qt.AlignHCenter
                    }

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
                            onClicked: Launcher.launch("C:\\Program Files (x86)\\Steam\\steam.exe", "")
                        }
                        PlatformSquareBtn {
                            btnText: "EPIC"
                            iconText: "󰊗"
                            brandColor: "#ffffff"
                            onClicked: Launcher.launch("C:\\Program Files (x86)\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe", "")
                        }
                        PlatformSquareBtn {
                            btnText: "ROBLOX"
                            iconText: "󰩊"
                            brandColor: "#e11d48"
                            onClicked: Launcher.launch("C:\\Users\\Public\\Desktop\\Roblox Player.lnk", "")
                        }
                        PlatformSquareBtn {
                            btnText: "RIOT"
                            iconText: "󰊴"
                            brandColor: "#d32f2f"
                            onClicked: Launcher.launch("C:\\Riot Games\\Riot Client\\RiotClientServices.exe", "")
                        }
                        PlatformSquareBtn {
                            btnText: "EA APP"
                            iconText: "󰓡"
                            brandColor: "#ff5722"
                            onClicked: Launcher.launch("C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe", "")
                        }
                        PlatformSquareBtn {
                            btnText: "VK PLAY"
                            iconText: "󰕼"
                            brandColor: "#ff3347"
                            onClicked: Launcher.launch("C:\\Users\\Public\\Desktop\\VK Play.lnk", "")
                        }
                    }

                    Item { height: 5; width: 1 }

                    // ==========================================
                    // КНОПКИ ДЕЙСТВИЙ (ВЫРАВНИВАНИЕ ПО ЛЕВОМУ КРАЮ)
                    // ==========================================
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        ActionBtn {
                            text: "МАРКЕТ И БАР"
                            icon: "🛒"
                            baseColor: "#eab308"
                            onClicked: storePopup.open()
                        }

                        ActionBtn {
                            text: "ПОПОЛНИТЬ БАЛАНС"
                            icon: "💳"
                        }

                        ActionBtn {
                            text: "ОТОЙТИ (ПАУЗА)"
                            icon: "⏳"
                            baseColor: "#3b82f6"
                            onClicked: {
                                var xhr = new XMLHttpRequest();
                                xhr.open("POST", "http://192.168.222.2:22222/api/shell/games/pause");
                                xhr.setRequestHeader("Content-Type", "application/json");
                                xhr.onreadystatechange = function() {
                                    if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                                        var res = JSON.parse(xhr.responseText);
                                        if (res.status === "success") {
                                            if (typeof root !== 'undefined') {
                                                root.temporaryPausePin = res.pin_code;
                                                root.sessionUser = "PAUSE"; // Выставляем статус паузы
                                                root.authScreen.visible = true; // Открываем экран блокировки
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
                                if (typeof root !== 'undefined') {
                                    root.authScreen.visible = true;
                                    root.sessionUser = "GUEST";
                                }
                                dashboardRoot.visible = false;
                            }
                        }
                    }

                    Item { height: 10; width: 1 }

                    // ==========================================
                    // РЕАЛЬНЫЙ НАТИВНЫЙ СИСТЕМНЫЙ БАР
                    // ==========================================
                    Rectangle {
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

                            // 1. НАДЁЖНЫЙ КАСТОМНЫЙ ПОЛЗУНОК ГРОМКОСТИ
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

                                        Rectangle {
                                            width: (customSlider.value / 100) * parent.width
                                            height: parent.height
                                            color: accentColor
                                            radius: 2
                                        }
                                    }

                                    Rectangle {
                                        id: handleItem
                                        x: (customSlider.value / 100) * (parent.width - width)
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 10; height: 10; radius: 5
                                        color: "white"
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        property bool isDragging: false

                                        function updateVolume(mx) {
                                            var pct = Math.max(0, Math.min(1, mx / width));
                                            customSlider.value = Math.round(pct * 100);
                                            Launcher.setSystemVolume(customSlider.value);
                                        }

                                        onPressed: function(mouse) { isDragging = true; updateVolume(mouse.x); }
                                        onPositionChanged: function(mouse) { if (isDragging) updateVolume(mouse.x); }
                                        onReleased: function(mouse) { isDragging = false; }
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            // 2. РЕАЛЬНОЕ ИЗМЕНЕНИЕ РАСКЛАДКИ WINDOWS ЧЕРЕЗ C++
                            Rectangle {
                                width: 35
                                height: 22
                                color: "#111"
                                border.color: "#333"
                                radius: 3
                                Text {
                                    anchors.centerIn: parent
                                    text: dashboardRoot.currentLanguage
                                    color: "white"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        dashboardRoot.currentLanguage = (dashboardRoot.currentLanguage === "RU") ? "EN" : "RU";
                                        Launcher.toggleSystemLanguage();
                                    }
                                }
                            }

                            // 3. Системные часы
                            Text {
                                id: sysClock
                                text: Qt.formatDateTime(new Date(), "hh:mm")
                                color: "white"
                                font.pixelSize: 13
                                font.bold: true
                                font.family: "Monospace"

                                Timer {
                                    interval: 1000
                                    running: true
                                    repeat: true
                                    onTriggered: sysClock.text = Qt.formatDateTime(new Date(), "hh:mm")
                                }
                            }
                        }
                    }

                }
            }
        }

        // ==========================================
        // ПРАВАЯ ПАНЕЛЬ (БИБЛИОТЕКА ИГР)
        // ==========================================
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 25

            Text {
                text: "БИБЛИОТЕКА ИГР"
                color: "white"; font.pixelSize: 24
                font.bold: true; font.letterSpacing: 2
            }

            Row {
                id: filterRow
                Layout.fillWidth: true
                spacing: 30
                property string activeTab: "ВСЕ ИГРЫ"

                Repeater {
                    model: ["ВСЕ ИГРЫ", "STEAM", "EPIC", "БРАУЗЕРЫ", "УТИЛИТЫ"]
                    delegate: Text {
                        text: modelData
                        color: filterRow.activeTab === modelData ? accentColor : "#666666"
                        font.pixelSize: 16; font.bold: true; font.letterSpacing: 1

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                filterRow.activeTab = modelData
                                gamesModel.setFilter(modelData)
                            }
                        }
                    }
                }
            }

            GridView {
                id: gamesGrid
                Layout.fillWidth: true; Layout.fillHeight: true
                cellWidth: 230; cellHeight: 320; clip: true
                model: gamesModel

                delegate: Item {
                    width: gamesGrid.cellWidth; height: gamesGrid.cellHeight

                    Rectangle {
                        anchors.fill: parent; anchors.margins: 10
                        color: "#0a0a0a"; radius: 6
                        border.width: gArea.containsMouse ? 2 : 1
                        border.color: gArea.containsMouse ? accentColor : "#1a1a1a"

                        Image {
                            width: parent.width; height: parent.height - 45
                            source: {
                                var pUrl = model.poster !== undefined ? model.poster : (poster !== undefined ? poster : "");
                                if (pUrl === "") return "";
                                if (pUrl.indexOf("http") === 0 || pUrl.indexOf("file") === 0) return pUrl;
                                if (pUrl.indexOf("/") === 0) return "http://192.168.222.2:22222" + pUrl;
                                return "http://192.168.222.2:22222/" + pUrl;
                            }
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true; opacity: gArea.containsMouse ? 1.0 : 0.7
                        }

                        Rectangle {
                            width: parent.width; height: 45; anchors.bottom: parent.bottom
                            color: gArea.containsMouse ? accentColor : "#050505"
                            Text {
                                anchors.centerIn: parent
                                text: (typeof title !== 'undefined') ? title : (model.title || "")
                                color: gArea.containsMouse ? "black" : "white"; font.bold: true
                            }
                        }

                        MouseArea {
                            id: gArea; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                var exe = (typeof exePath !== 'undefined') ? exePath : model.exePath;
                                var pArgs = (typeof args !== 'undefined') ? args : model.args;
                                Launcher.launch(exe, pArgs);
                            }
                        }
                    }
                }
            }
        }
    }

    // ==========================================
    // СТИЛИЗОВАННЫЕ КОМПОНЕНТЫ КНОПОК
    // ==========================================

    component ActionBtn : Rectangle {
        property string text: "BUTTON"
        property string icon: ""
        property string baseColor: accentColor
        signal clicked()

        Layout.fillWidth: true
        Layout.preferredHeight: 50
        radius: 4

        color: bMouse.pressed ? baseColor : (bMouse.containsMouse ? Qt.rgba(1,1,1,0.06) : "transparent")
        border.color: bMouse.containsMouse ? baseColor : Qt.rgba(1,1,1,0.15)
        border.width: 1

        Behavior on border.color { ColorAnimation { duration: 100 } }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 20
            spacing: 15

            Text { text: icon; font.pixelSize: 18; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: parent.parent.text; font.bold: true; font.pixelSize: 14
                color: bMouse.pressed ? "black" : (bMouse.containsMouse ? "white" : baseColor)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea { id: bMouse; anchors.fill: parent; hoverEnabled: true; onClicked: parent.clicked() }
    }

    component PlatformSquareBtn : Rectangle {
        property string btnText: "LAUNCH"
        property string iconText: "🎮"
        property string brandColor: accentColor
        signal clicked()

        Layout.fillWidth: true
        Layout.preferredHeight: 65
        radius: 4

        color: pMouse.containsMouse ? Qt.rgba(1,1,1,0.06) : "#0a0d0b"
        border.color: pMouse.containsMouse ? brandColor : "#1a1f1c"
        border.width: 1

        Behavior on border.color { ColorAnimation { duration: 100 } }

        layer.enabled: pMouse.containsMouse
        layer.effect: MultiEffect { blurEnabled: true; blur: 0.2; brightness: 0.1 }

        Column {
            anchors.centerIn: parent
            spacing: 6
            Text { text: iconText; color: pMouse.containsMouse ? brandColor : "#444"; font.pixelSize: 20; font.bold: true; anchors.horizontalCenter: parent.horizontalCenter }
            Text { text: btnText; color: pMouse.containsMouse ? "white" : "#666"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1; anchors.horizontalCenter: parent.horizontalCenter }
        }
        MouseArea { id: pMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
    }

    // ==========================================
    // ВСПЛЫВАЮЩЕЕ ОКНО МАРКЕТА
    // ==========================================
    Popup {
        id: storePopup
        width: 1400; height: 850
        anchors.centerIn: parent
        modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property var selectedItem: null

        Overlay.modal: Rectangle { color: "#000000"; opacity: 0.85 }
        background: Rectangle { color: "#050505"; border.color: "#eab308"; border.width: 2; radius: 12 }

        ColumnLayout {
            anchors.fill: parent; anchors.margins: 40; spacing: 30

            RowLayout {
                Layout.fillWidth: true
                Column {
                    Text { text: "REACTOR MARKET"; color: "#eab308"; font.pixelSize: 36; font.bold: true; font.italic: true }
                    Text { text: "СНАРЯЖЕНИЕ И ПРОВИЗИЯ ДЛЯ РЕЙДА"; color: "#eab308"; opacity: 0.5; font.pixelSize: 10; font.letterSpacing: 4 }
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 50; height: 50; color: "transparent"; border.color: "#333"; radius: 25
                    Text { anchors.centerIn: parent; text: "✕"; color: "white"; font.pixelSize: 20 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: storePopup.close() }
                }
            }

            Row {
                id: filterCatRow; spacing: 15; property string activeCat: "Все"
                Repeater {
                    model: [{ name: "Все", tag: "" }, { name: "Напитки", tag: "drinks" }, { name: "Снэки", tag: "food" }, { name: "Еда", tag: "food" }]
                    delegate: Rectangle {
                        width: 120; height: 40; radius: 8
                        color: filterCatRow.activeCat === modelData.name ? "#eab308" : "#111"
                        Text { anchors.centerIn: parent; text: modelData.name; color: filterCatRow.activeCat === modelData.name ? "black" : "white"; font.bold: true; font.pixelSize: 14 }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { filterCatRow.activeCat = modelData.name; storeModel.setFilter(modelData.tag); } }
                    }
                }
            }

            GridView {
                id: storeGrid; Layout.fillWidth: true; Layout.fillHeight: true; cellWidth: 260; cellHeight: 340; clip: true; model: storeModel
                delegate: Rectangle {
                    width: storeGrid.cellWidth - 20; height: storeGrid.cellHeight - 20; color: "#0a0a0a"; border.color: "#222"; border.width: 1; radius: 12; opacity: model.stock > 0 ? 1.0 : 0.4
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 20
                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 140
                            color: "#111"; radius: 8; clip: true
                            Image {
                                anchors.fill: parent; anchors.margins: 10
                                source: {
                                    var imgUrl = model.image || "";
                                    if (imgUrl === "") return "";
                                    if (imgUrl.indexOf("http") === 0 || imgUrl.indexOf("file") === 0) return imgUrl;
                                    if (imgUrl.indexOf("/") === 0) return "http://192.168.222.2:22222" + imgUrl;
                                    return "http://192.168.222.2:22222/" + imgUrl;
                                }
                                fillMode: Image.PreserveAspectFit; asynchronous: true
                            }
                            Text { visible: !model.image; anchors.centerIn: parent; text: "📦"; font.pixelSize: 40 }
                        }
                        Text { text: model.name; color: "white"; font.bold: true; font.pixelSize: 16; Layout.topMargin: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                        Text { text: model.category; color: "#666"; font.pixelSize: 10; font.letterSpacing: 2; font.bold: true }
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: model.price + " ₽"; color: "#eab308"; font.pixelSize: 24; font.bold: true; font.italic: true }
                            Item { Layout.fillWidth: true }
                            Rectangle {
                                visible: model.stock > 0; width: 50; height: 50; radius: 10; color: "#eab308"
                                Text { anchors.centerIn: parent; text: "+"; color: "black"; font.pixelSize: 24; font.bold: true }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { storePopup.selectedItem = { id: model.id, name: model.name, price: model.price }; confirmPopup.open(); } }
                            }
                            Rectangle { visible: model.stock <= 0; width: 70; height: 30; radius: 6; color: "#450a0a"; border.color: "#dc2626"; Text { anchors.centerIn: parent; text: "SOLD OUT"; color: "#dc2626"; font.pixelSize: 10; font.bold: true } }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: confirmPopup; width: 400; height: 350; anchors.centerIn: parent; modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#000000"; opacity: 0.9 }
        background: Rectangle { color: "#050505"; border.color: "#eab308"; border.width: 2; radius: 16 }
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 30; spacing: 10
            Text { text: "ПОДТВЕРЖДЕНИЕ ЗАКАЗА"; color: "#eab308"; font.pixelSize: 10; font.letterSpacing: 4; font.bold: true; Layout.alignment: Qt.AlignHCenter }
            Item { Layout.preferredHeight: 10 }
            Text { text: storePopup.selectedItem ? storePopup.selectedItem.name : ""; color: "white"; font.pixelSize: 24; font.bold: true; font.italic: true; Layout.alignment: Qt.AlignHCenter; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Text { text: (storePopup.selectedItem ? storePopup.selectedItem.price : 0) + " ₽"; color: "#eab308"; font.pixelSize: 42; font.bold: true; font.italic: true; Layout.alignment: Qt.AlignHCenter }
            Item { Layout.fillHeight: true }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 60; radius: 12; color: "#eab308"
                Text { anchors.centerIn: parent; text: "ПОДТВЕРДИТЬ ОПЛАТУ"; color: "black"; font.bold: true; font.pixelSize: 16; font.letterSpacing: 1 }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { if (storePopup.selectedItem) { NetManager.buyItem(storePopup.selectedItem.id, dashboardRoot.termId); confirmPopup.close(); storePopup.close(); } } }
            }
            Text { text: "ОТМЕНА"; color: "#666"; font.pixelSize: 12; font.bold: true; font.letterSpacing: 2; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 10; MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: confirmPopup.close() } }
        }
    }
}