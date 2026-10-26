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
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                width: parent.width - 16
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

    onVideoSourceUrlChanged: {
        if (wantPlayer && !playerLoader.active)
            activatePlayerTimer.restart()
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
                property bool triedHttp: false
                property bool httpFailed: false
                property bool localFailed: false
                property bool usingFallback: false

                function httpUrl() {
                    if (overlayBlockRoot.videoSourceUrl === "")
                        return ""
                    if (typeof NetworkManager !== "undefined"
                            && typeof NetworkManager.resolveOverlayUrl === "function")
                        return NetworkManager.resolveOverlayUrl(overlayBlockRoot.videoSourceUrl)
                    return overlayBlockRoot.videoSourceUrl
                }

                function resolvePath() {
                    if (typeof NetworkManager === "undefined")
                        return ""
                    return NetworkManager.getLocalPath(
                                overlayBlockRoot.videoSourceUrl,
                                overlayBlockRoot.blockUniqueId)
                }

                function fileReady(url) {
                    if (url === "")
                        return false
                    if (typeof NetworkManager === "undefined"
                            || typeof NetworkManager.isLocalMediaLight !== "function")
                        return true
                    return NetworkManager.isLocalMediaLight(url, 512 * 1024 * 1024)
                }

                function setSourceAndPlay(src) {
                    if (src === "")
                        return false
                    usingFallback = (src === overlayBlockRoot.fallbackVideo)
                    if (player.source.toString() === src) {
                        if (player.playbackState !== MediaPlayer.PlayingState
                                && overlayBlockRoot.wantPlayer)
                            player.play()
                        return true
                    }
                    console.log("[PLAYER]", overlayBlockRoot.blockUniqueId,
                                usingFallback ? "fallback:" : "open:", src)
                    player.stop()
                    player.source = src
                    return true
                }

                // Пока ролик качается или кодек его отверг — крутим fallback_bg.mp4.
                function playFallback() {
                    var fb = overlayBlockRoot.fallbackVideo
                    if (!fileReady(fb)) {
                        console.log("[PLAYER]", overlayBlockRoot.blockUniqueId,
                                    "fallback missing:", fb)
                        return false
                    }
                    return setSourceAndPlay(fb)
                }

                function openWhenReady() {
                    var path = resolvedPath !== "" ? resolvedPath : resolvePath()
                    resolvedPath = path
                    if (path !== "" && !localFailed) {
                        setSourceAndPlay(path)
                        return
                    }
                    // Не ждём 100+ МБ кэша: стримим с облака, кэш докачается в фоне.
                    var remote = httpUrl()
                    if (remote !== "" && !httpFailed) {
                        triedHttp = true
                        setSourceAndPlay(remote)
                        return
                    }
                    console.log("[PLAYER]", overlayBlockRoot.blockUniqueId, "wait download")
                    playFallback()
                }

                function onMediaRejected() {
                    var current = player.source.toString()
                    console.log("[PLAYER-ERROR]", overlayBlockRoot.blockUniqueId,
                                "invalid", current)
                    if (usingFallback || current === overlayBlockRoot.fallbackVideo) {
                        player.stop()
                        return
                    }
                    if (resolvedPath !== "" && current === resolvedPath)
                        localFailed = true
                    if (triedHttp && current !== resolvedPath)
                        httpFailed = true

                    if (resolvedPath !== "" && !localFailed) {
                        setSourceAndPlay(resolvedPath)
                        return
                    }
                    var remote = httpUrl()
                    if (remote !== "" && !httpFailed && current !== remote) {
                        triedHttp = true
                        setSourceAndPlay(remote)
                        return
                    }
                    // HTTP без moov в начале или битый кэш — запасной ролик, пока нет файла.
                    playFallback()
                }

                Component.onCompleted: {
                    resolvedPath = resolvePath()
                    // Ещё один yield после создания плеера.
                    Qt.callLater(openWhenReady)
                }

                Connections {
                    target: overlayBlockRoot
                    function onVideoSourceUrlChanged() {
                        videoInner.resolvedPath = ""
                        videoInner.triedHttp = false
                        videoInner.httpFailed = false
                        videoInner.localFailed = false
                        videoInner.usingFallback = false
                        Qt.callLater(videoInner.openWhenReady)
                    }
                }

                Connections {
                    target: NetworkManager
                    function onFileDownloaded(remoteUrl, localPath, target) {
                        if (target !== overlayBlockRoot.blockUniqueId)
                            return
                        if (overlayBlockRoot.videoSourceUrl !== remoteUrl)
                            return
                        resolvedPath = localPath
                        localFailed = false
                        Qt.callLater(openWhenReady)
                    }
                }

                MediaPlayer {
                    id: player
                    videoOutput: vout
                    audioOutput: AudioOutput { muted: true }
                    loops: MediaPlayer.Infinite
                    autoPlay: overlayBlockRoot.wantPlayer

                    onMediaStatusChanged: {
                        if (mediaStatus === MediaPlayer.LoadedMedia
                                || mediaStatus === MediaPlayer.BufferedMedia) {
                            if (overlayBlockRoot.wantPlayer)
                                play()
                        } else if (mediaStatus === MediaPlayer.InvalidMedia) {
                            videoInner.onMediaRejected()
                        }
                    }

                    onErrorOccurred: function(error, errorString) {
                        if (error === MediaPlayer.NoError)
                            return
                        console.log("[PLAYER-ERROR]", overlayBlockRoot.blockUniqueId,
                                    errorString)
                        if (mediaStatus !== MediaPlayer.InvalidMedia)
                            videoInner.onMediaRejected()
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
