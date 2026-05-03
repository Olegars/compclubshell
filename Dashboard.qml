import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Shapes

Item {
    id: dashboardRoot
    anchors.fill: parent

    // Данные профиля (заполняются из Main.qml при логине)
    property string userName: "PLAYER_1"
    property real userBalance: 0 // real лучше подходит для денег (3840.00)
    property string timeRemaining: "00:00:00"

    // Безопасное получение ID терминала
    property string termId: (typeof root !== 'undefined' && root !== null) ? root.terminalId : "PC-0"

    // PRO-зона: компы с 1 по 5
    property bool isProBootcamp: {
        var num = parseInt(termId.replace(/[^0-9]/g, ""));
        return num >= 1 && num <= 5;
    }

    property string accentColor: isProBootcamp ? "#a855f7" : "#22c55e"
    property string darkBg: "#050a06"

    // --- ФОН ---
    Rectangle {
        anchors.fill: parent
        color: "#020202"

        Image {
            anchors.fill: parent
            source: "images/hex_bg.png"
            fillMode: Image.Tile
            opacity: 0.15 // Чуть ярче, чтобы видеть, что работает
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
        // ЛЕВАЯ ПАНЕЛЬ (ПРОФИЛЬ)
        // ==========================================
        Rectangle {
            Layout.preferredWidth: 380
            Layout.fillHeight: true
            color: darkBg; border.color: accentColor; border.width: 1; radius: 4

            Row {
                spacing: 10
                Layout.alignment: Qt.AlignHCenter

                Rectangle { width: 8; height: 8; radius: 4; color: "#22c55e"; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: "PING (VALORANT EU): " + NetManager.getLatency("162.249.72.1") + "ms"
                    color: "#666"; font.pixelSize: 10; font.bold: true
                }
            }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 30; spacing: 20

                // --- СТАТУС ПИНГА (Enterprise-style) ---
                Row {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    Rectangle { width: 6; height: 6; radius: 3; color: accentColor; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        // Чтобы не вешать интерфейс, пингуем не каждую миллисекунду,
                        // а только когда текст создается или по таймеру
                        text: "LATENCY (EU): " + NetManager.getLatency("162.249.72.1") + " MS"
                        color: accentColor; font.pixelSize: 9; font.bold: true; opacity: 0.5
                    }
                }

                // Статус зона (PRO/STANDARD)
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 35
                    // ... твой код ...
                }
                // Инфо
                Column {
                    Layout.fillWidth: true; spacing: 5
                    Text { text: "ПОЛЬЗОВАТЕЛЬ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                    Text { text: userName; color: "white"; font.pixelSize: 28; font.bold: true }

                    Item { height: 20; width: 1 }

                    Text { text: "ОСТАЛОСЬ ВРЕМЕНИ"; color: accentColor; font.pixelSize: 10; opacity: 0.7 }
                    Text {
                        text: timeRemaining; color: "white"; font.pixelSize: 52
                        font.family: "Monospace"; font.bold: true
                    }
                    Text { text: "БАЛАНС: " + userBalance.toFixed(2) + " ₽"; color: "#a3a3a3"; font.pixelSize: 18 }
                }

                Item { Layout.fillHeight: true } // Пружина

                // Кнопки
                Column {
                    Layout.fillWidth: true; spacing: 15
                    ActionBtn {
                        text: "МАРКЕТ И БАР"; icon: "🛒"; baseColor: "#eab308"
                        onClicked: storePopup.open() // Открываем модалку
                    }
                    ActionBtn { text: "ПОПОЛНИТЬ БАЛАНС"; icon: "💳" }
                    ActionBtn { text: "ВЫЗОВ АДМИНА"; icon: "⚠️"; baseColor: "#dc2626" }
                    ActionBtn {
                        text: "ВЫХОД"; icon: "🚪"; baseColor: "#525252"
                        onClicked: {
                            // Логика выхода (можно отправить сигнал в Main.qml)
                            if (typeof root !== 'undefined') root.loginScreen.visible = true
                            dashboardRoot.visible = false
                        }
                    }
                }
            }
        }

        // ==========================================
                // ПРАВАЯ ПАНЕЛЬ (СПИСОК ИГР)
                // ==========================================
                ColumnLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 25

                    Text {
                        text: "БИБЛИОТЕКА ИГР"; color: "white"; font.pixelSize: 24
                        font.bold: true; font.letterSpacing: 2
                    }

                    // --- ПАНЕЛЬ ФИЛЬТРОВ ---
                    Row {
                        Layout.fillWidth: true
                        spacing: 30

                        // Переменная для хранения текущей вкладки
                        property string activeTab: "ВСЕ ИГРЫ"

                        Repeater {
                            // Список твоих категорий
                            model: ["ВСЕ ИГРЫ", "STEAM", "EPIC", "БРАУЗЕРЫ", "УТИЛИТЫ"]

                            delegate: Text {
                                text: modelData
                                // Если вкладка активна - красим в зеленый/фиолетовый, иначе серый
                                color: parent.activeTab === modelData ? accentColor : "#666666"
                                font.pixelSize: 16
                                font.bold: true
                                font.letterSpacing: 1

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        parent.parent.activeTab = modelData // Меняем цвет UI
                                        gamesModel.setFilter(modelData)     // Отправляем команду в C++
                                    }
                                }
                            }
                        }
                    }

                    // --- СЕТКА ИГР ---
                    GridView {
                        id: gamesGrid
                        Layout.fillWidth: true; Layout.fillHeight: true
                        cellWidth: 230; cellHeight: 320; clip: true

                        model: gamesModel

                delegate: Item {
                    width: gamesGrid.cellWidth; height: gamesGrid.cellHeight
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 10
                        color: "#0a0a0a"; radius: 6; border.width: gArea.containsMouse ? 2 : 1
                        border.color: gArea.containsMouse ? accentColor : "#1a1a1a"

                        Image {
                            width: parent.width; height: parent.height - 45
                            source: model.poster || ""; fillMode: Image.PreserveAspectCrop
                            asynchronous: true; opacity: gArea.containsMouse ? 1.0 : 0.7
                        }

                        Rectangle {
                            width: parent.width; height: 45; anchors.bottom: parent.bottom
                            color: gArea.containsMouse ? accentColor : "#050505"
                            Text {
                                anchors.centerIn: parent; text: model.title
                                color: gArea.containsMouse ? "black" : "white"; font.bold: true
                            }
                        }

                        MouseArea {
                            id: gArea; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                console.log("Запуск игры:", model.title)
                                Launcher.launch(model.exePath, model.args)
                            }
                        }
                    }
                }
            }
        }
    }

    // Вспомогательный компонент кнопок
    component ActionBtn : Rectangle {
        property string text: "BUTTON"
        property string icon: ""
        property string baseColor: accentColor
        signal clicked()

        width: parent.width; height: 50; radius: 4
        color: bMouse.pressed ? baseColor : (bMouse.containsMouse ? "#111" : "transparent")
        border.color: baseColor; border.width: 1

        Row {
            anchors.centerIn: parent; spacing: 10
            Text { text: icon; font.pixelSize: 18 }
            Text {
                text: parent.parent.text; font.bold: true
                color: bMouse.pressed ? "black" : (bMouse.containsMouse ? "white" : baseColor)
            }
        }
        MouseArea { id: bMouse; anchors.fill: parent; hoverEnabled: true; onClicked: parent.clicked() }
    }
    // ==========================================
        // ВСПЛЫВАЮЩЕЕ ОКНО МАГАЗИНА (POPUP)
        // ==========================================
    // ==========================================
        // ВСПЛЫВАЮЩЕЕ ОКНО МАГАЗИНА (POPUP)
        // ==========================================
        Popup {
            id: storePopup
            width: 1400
            height: 850
            anchors.centerIn: parent
            modal: true
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            // Свойство для хранения товара, который юзер хочет купить
            property var selectedItem: null

            Overlay.modal: Rectangle {
                color: "#000000"
                opacity: 0.85
                Behavior on opacity { NumberAnimation { duration: 300 } }
            }

            background: Rectangle {
                color: "#050505"
                border.color: "#eab308"
                border.width: 2
                radius: 12
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 40
                spacing: 30

                // --- ШАПКА ---
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

                // --- ФИЛЬТРЫ КАТЕГОРИЙ ---
                // --- ФИЛЬТРЫ КАТЕГОРИЙ ---
                // --- ФИЛЬТРЫ КАТЕГОРИЙ ---
                Row {
                    id: filterRow
                    spacing: 15
                    property string activeCat: "Все"

                    Repeater {
                        // Объединяем визуальное название и реальный тег из базы
                        model: [
                            { name: "Все", tag: "" },
                            { name: "Напитки", tag: "drinks" },
                            { name: "Снэки", tag: "food" },
                            { name: "Еда", tag: "food" }
                        ]

                        delegate: Rectangle {
                            width: 120; height: 40; radius: 8
                            color: filterRow.activeCat === modelData.name ? "#eab308" : "#111"

                            Text {
                                anchors.centerIn: parent
                                text: modelData.name
                                color: filterRow.activeCat === modelData.name ? "black" : "white"
                                font.bold: true; font.pixelSize: 14
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    filterRow.activeCat = modelData.name;
                                    // Отправляем в С++ именно английский тэг (drinks, food или пустую строку)
                                    storeModel.setFilter(modelData.tag);
                                }
                            }
                        }
                    }
                }

                // --- СЕТКА ТОВАРОВ ---
                GridView {
                    id: storeGrid
                    Layout.fillWidth: true; Layout.fillHeight: true
                    cellWidth: 260; cellHeight: 340; clip: true

                    // Подключаем C++ модель
                    model: storeModel

                    delegate: Rectangle {
                        width: storeGrid.cellWidth - 20; height: storeGrid.cellHeight - 20
                        color: "#0a0a0a"; border.color: "#222"; border.width: 1; radius: 12

                        // Эффект отсутствия товара
                        opacity: model.stock > 0 ? 1.0 : 0.4

                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 20

                            // Картинка товара
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 140
                                color: "#111"; radius: 8; clip: true

                                Image {
                                    anchors.fill: parent; anchors.margins: 10
                                    source: model.image || ""
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                }

                                // Заглушка, если сервер не прислал картинку
                                Text {
                                    visible: !model.image
                                    anchors.centerIn: parent; text: "📦"; font.pixelSize: 40
                                }
                            }

                            // Данные из модели
                            Text { text: model.name; color: "white"; font.bold: true; font.pixelSize: 16; Layout.topMargin: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text { text: model.category; color: "#666"; font.pixelSize: 10; font.letterSpacing: 2; font.bold: true }

                            Item { Layout.fillHeight: true }

                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: model.price + " ₽"; color: "#eab308"; font.pixelSize: 24; font.bold: true; font.italic: true }
                                Item { Layout.fillWidth: true }

                                // Кнопка покупки
                                Rectangle {
                                    visible: model.stock > 0
                                    width: 50; height: 50; radius: 10; color: "#eab308"
                                    Text { anchors.centerIn: parent; text: "+"; color: "black"; font.pixelSize: 24; font.bold: true }
                                    MouseArea {
                                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            // Запоминаем выбранный товар и открываем подтверждение
                                            storePopup.selectedItem = { id: model.id, name: model.name, price: model.price };
                                            confirmPopup.open();
                                        }
                                    }
                                }

                                // Плашка "Пусто", если stock <= 0
                                Rectangle {
                                    visible: model.stock <= 0
                                    width: 70; height: 30; radius: 6; color: "#450a0a"; border.color: "#dc2626"
                                    Text { anchors.centerIn: parent; text: "SOLD OUT"; color: "#dc2626"; font.pixelSize: 10; font.bold: true }
                                }
                            }
                        }
                    }
                }
            }

            // ==========================================
            // ОКНО ПОДТВЕРЖДЕНИЯ (Внутри storePopup)
            // ==========================================
            Popup {
                id: confirmPopup
                width: 400; height: 350; anchors.centerIn: parent
                modal: true; focus: true
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                Overlay.modal: Rectangle { color: "#000000"; opacity: 0.9 }

                background: Rectangle {
                    color: "#050505"; border.color: "#eab308"; border.width: 2; radius: 16
                }

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 30; spacing: 10

                    Text { text: "ПОДТВЕРЖДЕНИЕ ЗАКАЗА"; color: "#eab308"; font.pixelSize: 10; font.letterSpacing: 4; font.bold: true; Layout.alignment: Qt.AlignHCenter }

                    Item { Layout.preferredHeight: 10 }

                    Text {
                        text: storePopup.selectedItem ? storePopup.selectedItem.name : ""
                        color: "white"; font.pixelSize: 24; font.bold: true; font.italic: true
                        Layout.alignment: Qt.AlignHCenter; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; Layout.fillWidth: true
                    }

                    Text {
                        text: (storePopup.selectedItem ? storePopup.selectedItem.price : 0) + " ₽"
                        color: "#eab308"; font.pixelSize: 42; font.bold: true; font.italic: true
                        Layout.alignment: Qt.AlignHCenter
                    }

                    Item { Layout.fillHeight: true }

                    // Кнопка оплаты
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 60; radius: 12; color: "#eab308"
                        Text { anchors.centerIn: parent; text: "ПОДТВЕРДИТЬ ОПЛАТУ"; color: "black"; font.bold: true; font.pixelSize: 16; font.letterSpacing: 1 }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (storePopup.selectedItem) {
                                    // 1. Отправляем запрос в C++
                                    NetManager.buyItem(storePopup.selectedItem.id, root.terminalId);

                                    // 2. Закрываем окна
                                    confirmPopup.close();
                                    storePopup.close();
                                }
                            }
                        }
                    }

                    // Кнопка отмены
                    Text {
                        text: "ОТМЕНА"; color: "#666"; font.pixelSize: 12; font.bold: true; font.letterSpacing: 2
                        Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 10
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: confirmPopup.close() }
                    }
                }
            }
        }
}