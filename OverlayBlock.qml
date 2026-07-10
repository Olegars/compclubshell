// Путь: C:/Qt/compclubshell/OverlayBlock.qml
import QtQuick
import QtMultimedia

Rectangle {
    id: overlayBlockRoot

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

    Behavior on opacity {
        NumberAnimation { duration: 500 }
    }

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
        active: overlayBlockRoot.videoSourceUrl !== ""
        anchors.fill: parent
        z: 1

        sourceComponent: Component {
            Item {
                id: videoInnerItem
                anchors.fill: parent
                property string currentPlayingPath: ""

                function updateSource() {
                    var rawUrl = overlayVideoLoader.parent.videoSourceUrl
                    var blockId = overlayVideoLoader.parent.blockUniqueId
                    if (rawUrl !== "" && typeof NetworkManager !== "undefined") {
                        var path = NetworkManager.getLocalPath(rawUrl, blockId)
                        if (overlayBgPlayer.source.toString() === path && path !== "") {
                            return
                        }

                        console.log("[PLAYER-OPTIMIZED]", blockId, "-> Переключение источника на:", path)
                        if (path !== "") {
                            overlayBgPlayer.stop()
                            overlayBgPlayer.source = path
                            overlayBgPlayer.play()
                        } else {
                            if (overlayBgPlayer.source.toString() !== root.fallbackVideo) {
                                overlayBgPlayer.stop()
                                overlayBgPlayer.source = root.fallbackVideo
                                overlayBgPlayer.play()
                            }
                        }
                    }
                }

                Connections {
                    target: overlayVideoLoader.parent
                    function onVideoSourceUrlChanged() {
                        console.log("[PLAYER-SIGNAL]", overlayVideoLoader.parent.blockUniqueId, "-> Смена URL бэкенда:", overlayVideoLoader.parent.videoSourceUrl)
                        videoInnerItem.updateSource()
                    }
                }

                Connections {
                    target: NetworkManager
                    function onFileDownloaded(remoteUrl, localPath, target) {
                        var blockId = overlayVideoLoader.parent.blockUniqueId
                        var currentUrl = overlayVideoLoader.parent.videoSourceUrl
                        if (target === blockId && currentUrl === remoteUrl) {
                            if (overlayBgPlayer.source.toString() !== localPath) {
                                console.log("[OVERLAY-CACHE] Слот", blockId, "считал скачанный файл:", localPath)
                                overlayBgPlayer.stop()
                                overlayBgPlayer.source = localPath
                                overlayBgPlayer.play()
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
                        videoInnerItem.updateSource()
                    }

                    onMediaStatusChanged: {
                        var blockId = overlayVideoLoader.parent.blockUniqueId
                        if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia) {
                            if (root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE") {
                                overlayBgPlayer.play()
                            } else {
                                overlayBgPlayer.stop()
                            }
                        } else if (mediaStatus === MediaPlayer.InvalidMedia) {
                            console.log("[PLAYER-ERROR]", blockId, "-> Ошибка кодека. Уход на fallback.")
                            overlayBgPlayer.stop()
                            if (overlayBgPlayer.source.toString() !== root.fallbackVideo) {
                                overlayBgPlayer.source = root.fallbackVideo
                                if (root.sessionUser === "GUEST" || root.sessionUser === "" || root.sessionUser === "PAUSE") {
                                    overlayBgPlayer.play()
                                }
                            }
                        }
                    }
                }

                VideoOutput {
                    id: vOutOverlayBg
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectCrop
                    layer.enabled: false
                }
            }
        }
    }
}