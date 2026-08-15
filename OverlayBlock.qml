// Путь: C:/Qt/compclubshell/OverlayBlock.qml
import QtQuick
import QtMultimedia
import sector0451

Rectangle {
    id: overlayBlockRoot

    property string title: "BLOCK"
    property string blockUniqueId: ""
    property bool isActive: true
    property var content: null
    property string videoSourceUrl: ""
    property string fallbackVideo: (typeof PathResolver !== "undefined" && PathResolver.fallbackVideoUrl)
                                   ? PathResolver.fallbackVideoUrl
                                   : "file:///C:/ShellVideo/Cache/fallback_bg.mp4"
    property bool playbackAllowed: true
    // Сдвиг активации плеера между слотами (мс) — не открывать 6 файлов в один тик.
    property int openDelayMs: 0

    readonly property bool wantPlayer: playbackAllowed
                                       && isActive
                                       && videoSourceUrl !== ""

    color: Theme.bgPanel
    border.color: isActive ? Theme.accentBorder : "#050505"
    border.width: 1
    clip: true
    opacity: isActive ? 1.0 : 0.05

    Behavior on opacity {
        NumberAnimation { duration: 500 }
    }

    Text {
        text: title
        color: Theme.accent
        font.pixelSize: Theme.fontCaption
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

    // Создаём MediaPlayer только когда слот реально должен играть (один из шести).
    // Активацию Loader откладываем — не в том же тике, что overlaysReady.
    Timer {
        id: activatePlayerTimer
        interval: 750 + Math.max(0, overlayBlockRoot.openDelayMs)
        repeat: false
        onTriggered: {
            if (overlayBlockRoot.wantPlayer)
                playerLoader.active = true
        }
    }

    onWantPlayerChanged: {
        if (wantPlayer) {
            activatePlayerTimer.restart()
        } else {
            activatePlayerTimer.stop()
            playerLoader.active = false
        }
    }

    Component.onCompleted: {
        if (wantPlayer)
            activatePlayerTimer.restart()
    }

    Loader {
        id: playerLoader
        anchors.fill: parent
        z: 1
        active: false

        sourceComponent: Component {
            Item {
                id: videoInner
                anchors.fill: parent
                property string resolvedPath: ""

                function resolvePath() {
                    if (typeof NetworkManager === "undefined")
                        return ""
                    return NetworkManager.getLocalPath(
                                overlayBlockRoot.videoSourceUrl,
                                overlayBlockRoot.blockUniqueId)
                }

                function openWhenReady() {
                    var path = resolvedPath !== "" ? resolvedPath : resolvePath()
                    resolvedPath = path
                    if (path === "") {
                        console.log("[PLAYER]", overlayBlockRoot.blockUniqueId, "wait download")
                        return
                    }
                    if (player.source.toString() === path) {
                        if (player.playbackState !== MediaPlayer.PlayingState)
                            player.play()
                        return
                    }
                    console.log("[PLAYER]", overlayBlockRoot.blockUniqueId, "open:", path)
                    player.stop()
                    player.source = path
                    // play после LoadedMedia
                }

                Component.onCompleted: {
                    resolvedPath = resolvePath()
                    // Ещё один yield после создания плеера.
                    Qt.callLater(openWhenReady)
                }

                Connections {
                    target: NetworkManager
                    function onFileDownloaded(remoteUrl, localPath, target) {
                        if (target !== overlayBlockRoot.blockUniqueId)
                            return
                        if (overlayBlockRoot.videoSourceUrl !== remoteUrl)
                            return
                        resolvedPath = localPath
                        Qt.callLater(openWhenReady)
                    }
                }

                MediaPlayer {
                    id: player
                    videoOutput: vout
                    audioOutput: AudioOutput { muted: true }
                    loops: MediaPlayer.Infinite

                    onMediaStatusChanged: {
                        if (mediaStatus === MediaPlayer.LoadedMedia
                                || mediaStatus === MediaPlayer.BufferedMedia) {
                            if (overlayBlockRoot.wantPlayer)
                                play()
                        } else if (mediaStatus === MediaPlayer.InvalidMedia) {
                            console.log("[PLAYER-ERROR]", overlayBlockRoot.blockUniqueId,
                                        "invalid", source)
                            stop()
                        }
                    }
                }

                VideoOutput {
                    id: vout
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectCrop
                    layer.enabled: false
                }
            }
        }
    }
}
