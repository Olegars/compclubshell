import QtQuick

// Фон из регистрационных аватаров: случайный порядок на запуск,
// низкая непрозрачность — как водяные знаки. Без MultiEffect/layer.
Item {
    id: root
    clip: true

    readonly property var avatarSources: [
        Qt.resolvedUrl("images/avatars/avatar_1.png"),
        Qt.resolvedUrl("images/avatars/avatar_2.png"),
        Qt.resolvedUrl("images/avatars/avatar_3.png"),
        Qt.resolvedUrl("images/avatars/avatar_4.png"),
        Qt.resolvedUrl("images/avatars/avatar_5.png"),
        Qt.resolvedUrl("images/avatars/avatar_6.png"),
        Qt.resolvedUrl("images/avatars/avatar_7.png"),
        Qt.resolvedUrl("images/avatars/avatar_8.png"),
        Qt.resolvedUrl("images/avatars/avatar_9.png"),
        Qt.resolvedUrl("images/avatars/avatar_10.png")
    ]

    property var order: []
    property var jitter: []

    readonly property int cellW: Math.max(240, Math.round(width / 5.1))
    readonly property int cellH: Math.max(280, Math.round(height / 3.15))
    readonly property int cols: Math.max(4, Math.ceil(width / cellW) + 1)
    readonly property int rows: Math.max(3, Math.ceil(height / cellH) + 1)

    function shuffleCopy(arr) {
        var a = arr.slice()
        for (var i = a.length - 1; i > 0; i--) {
            var j = Math.floor(Math.random() * (i + 1))
            var t = a[i]
            a[i] = a[j]
            a[j] = t
        }
        return a
    }

    Component.onCompleted: {
        var idxs = []
        for (var i = 0; i < avatarSources.length; i++)
            idxs.push(i)
        var jits = []
        for (var k = 0; k < 72; k++) {
            jits.push({
                dx: Math.random() - 0.5,
                dy: Math.random() - 0.5,
                rot: (Math.random() - 0.5) * 16,
                sc: 0.84 + Math.random() * 0.24,
                op: 0.09 + Math.random() * 0.07
            })
        }
        jitter = jits
        order = shuffleCopy(idxs)
    }

    Repeater {
        model: (root.width > 80 && root.order.length > 0 && root.jitter.length > 0)
               ? (root.cols * root.rows) : 0

        Image {
            readonly property var j: root.jitter[index % root.jitter.length]
            readonly property int col: index % root.cols
            readonly property int row: Math.floor(index / root.cols)
            readonly property int srcIndex: root.order[index % root.order.length]

            x: col * root.cellW + j.dx * root.cellW * 0.18 - root.cellW * 0.1
            y: row * root.cellH + j.dy * root.cellH * 0.18 - root.cellH * 0.1
            width: root.cellW * j.sc
            height: root.cellH * j.sc
            rotation: j.rot
            opacity: j.op
            source: root.avatarSources[srcIndex]
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 360
            sourceSize.height: 420
        }
    }
}
