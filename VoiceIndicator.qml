import QtQuick
import QtQuick.Window

// Маленький красный индикатор hold-to-talk (отдельный topmost HWND).
Window {
    id: voiceIndicator
    title: ((typeof NetworkManager !== "undefined" && NetworkManager.clubName)
            ? NetworkManager.clubName : "Клуб") + " Voice"
    visible: false
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    transientParent: null
    width: (voiceState === "error" && lastError.length > 0) ? 460 : 22
    height: (voiceState === "error" && lastError.length > 0) ? 44 : 22

    property string voiceState: "idle"
    property string lastError: ""

    readonly property color solidRed: "#e11d48"
    readonly property color dimRed: "#9f1239"

    function placeBottomRight() {
        var screens = Qt.application.screens
        var scr = (screens && screens.length > 0) ? screens[0] : Screen
        if (!scr)
            return
        var margin = 18
        x = scr.virtualX + scr.width - width - margin
        y = scr.virtualY + scr.height - height - margin
    }

    function reassertTopmost() {
        if (!visible)
            return
        if (typeof Launcher !== "undefined"
                && typeof Launcher.raiseTopmostToolWindow === "function")
            Launcher.raiseTopmostToolWindow(voiceIndicator)
        else
            raise()
    }

    function syncFromAssistant() {
        if (typeof VoiceAssistant === "undefined" || !VoiceAssistant)
            return
        voiceState = VoiceAssistant.state || "idle"
        lastError = VoiceAssistant.lastError || ""
        const show = voiceState === "listening"
                  || voiceState === "thinking"
                  || voiceState === "speaking"
                  || voiceState === "error"
        if (show) {
            placeBottomRight()
            visible = true
            reassertTopmost()
        } else {
            visible = false
            pulseAnim.stop()
            dot.opacity = 1
        }
        if (voiceState === "thinking")
            pulseAnim.start()
        else {
            pulseAnim.stop()
            if (voiceState === "speaking")
                dot.opacity = 0.55
            else
                dot.opacity = 1
        }
    }

    Connections {
        target: typeof VoiceAssistant !== "undefined" ? VoiceAssistant : null
        function onStateChanged() { voiceIndicator.syncFromAssistant() }
        function onLastErrorChanged() { voiceIndicator.syncFromAssistant() }
    }

    Timer {
        interval: 1500
        running: voiceIndicator.visible
        repeat: true
        onTriggered: voiceIndicator.reassertTopmost()
    }

    Rectangle {
        id: banner
        anchors.fill: parent
        radius: height / 2
        color: voiceIndicator.voiceState === "error" ? "#7c2d12" : "transparent"
        visible: voiceIndicator.voiceState === "error" && voiceIndicator.lastError.length > 0

        Text {
            anchors.fill: parent
            anchors.leftMargin: 30
            anchors.rightMargin: 12
            verticalAlignment: Text.AlignVCenter
            color: "#fed7aa"
            font.pixelSize: 12
            font.bold: true
            elide: Text.ElideRight
            text: voiceIndicator.lastError
        }
    }

    Rectangle {
        id: dot
        width: 22
        height: 22
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        radius: width / 2
        color: voiceIndicator.voiceState === "error" ? "#f59e0b"
             : (voiceIndicator.voiceState === "speaking" ? voiceIndicator.dimRed
                                                        : voiceIndicator.solidRed)
        border.color: "#450a0a"
        border.width: 1

        SequentialAnimation {
            id: pulseAnim
            loops: Animation.Infinite
            NumberAnimation { target: dot; property: "opacity"; to: 0.25; duration: 420 }
            NumberAnimation { target: dot; property: "opacity"; to: 1.0; duration: 420 }
        }
    }

    onWidthChanged: if (visible) placeBottomRight()
    onHeightChanged: if (visible) placeBottomRight()

    Component.onCompleted: syncFromAssistant()
}
