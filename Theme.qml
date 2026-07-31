// Путь: Theme.qml
// Единая тема оболочки: палитра, шкала шрифтов, радиусы и коэффициент
// масштабирования интерфейса. Базовый макет — 1920x1080.
pragma Singleton
import QtQuick

QtObject {
    id: theme

    // ---------- Масштаб ----------
    // Screen недоступен внутри QtObject, поэтому реальные размеры окна
    // присваивает Main.qml. Базовый макет — Full HD 16:9.
    readonly property real baseWidth: 1920
    readonly property real baseHeight: 1080

    property real viewportWidth: baseWidth
    property real viewportHeight: baseHeight

    // 1920x1080 -> 1.0, 2560x1440 -> 1.33, 3840x2160 -> 2.0.
    // Нижняя граница защищает мелкие экраны от нечитаемого интерфейса.
    readonly property real scale: Math.max(0.75, Math.min(viewportWidth / baseWidth,
                                                         viewportHeight / baseHeight))

    // Растровые ресурсы (постеры, иконки) декодируем под физический размер,
    // иначе на 1440p/4K масштабированная текстура выглядит мылом.
    function px(value) { return Math.ceil(value * theme.scale) }

    // ---------- Зона ПК и акцент ----------
    property string zoneType: "standard"

    readonly property bool isPremiumZone: {
        var z = String(theme.zoneType).toLowerCase()
        return z === "pro" || z === "bootcamp" || z === "trio" || z === "vip"
    }

    readonly property color accent:        isPremiumZone ? "#a855f7" : "#22c55e"
    readonly property color accentBright:  isPremiumZone ? "#c084fc" : "#4ade80"
    readonly property color accentDeep:    isPremiumZone ? "#7e22ce" : "#16a34a"
    readonly property color accentPressed: isPremiumZone ? "#6b21a8" : "#15803d"
    readonly property color accentBorder:  isPremiumZone ? "#3b1a5c" : "#1a4d29"
    readonly property color accentSurface: isPremiumZone ? "#100818" : "#08120a"
    readonly property color accentSurfaceIdle: isPremiumZone ? "#0d0710" : "#0d130e"
    readonly property color accentPanel:   isPremiumZone ? "#0a0510" : "#050a06"

    // ---------- Фоны ----------
    readonly property color bgRoot: "#020202"
    readonly property color bgDeep: "#030704"
    readonly property color bgPanel: "#0a0a0a"

    // ---------- Текст ----------
    readonly property color textPrimary: "white"
    readonly property color textBody: "#e5e5e5"
    readonly property color textSecondary: "#a3a3a3"
    readonly property color textMuted: "#666666"

    // ---------- Семантика ----------
    readonly property color success: "#22c55e"
    readonly property color danger: "#ef4444"
    readonly property color dangerStrong: "#dc2626"
    readonly property color warning: "#facc15"
    readonly property color shop: "#eab308"
    readonly property color info: "#3b82f6"
    readonly property color infoDeep: "#1d4ed8"
    readonly property color infoSurface: "#08162a"
    readonly property color infoSurfaceIdle: "#0d1117"

    // ---------- Типографика ----------
    // Значения в единицах базового макета: интерфейс масштабируется целиком
    // (uiRoot.scale), поэтому dp() здесь применять нельзя — получится двойной масштаб.
    // 12 — нижняя граница читаемости за клубным монитором
    readonly property int fontCaption: 12
    readonly property int fontBody: 13
    readonly property int fontLabel: 16
    readonly property int fontTitle: 22
    readonly property int fontHeading: 28
    readonly property int fontHero: 52

    // ---------- Радиусы ----------
    readonly property int radiusXs: 3
    readonly property int radiusSm: 4
    readonly property int radiusMd: 6
    readonly property int radiusLg: 10
}
