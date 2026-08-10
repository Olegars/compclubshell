// Путь: Dashboard.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Window
import Qt5Compat.GraphicalEffects
import QtWebView
import sector0451

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
    property real userBalance: (typeof root !== 'undefined' && root !== null) ? root.sessionBalance : 0.0
    property string timeRemaining: (typeof root !== 'undefined') ? root.sessionTime : "00:00:00"

    // Подсветка баланса, когда деньги прилетели (пополнение, возврат).
    // Первое присвоение после входа не считаем: там баланс приходит с нуля.
    property real balancePrev: 0
    property bool balanceTracked: false
    property real balanceGain: 0
    signal balanceIncreased(real gain)

    onUserBalanceChanged: {
        if (!balanceTracked) {
            balanceTracked = true
            balancePrev = userBalance
            return
        }
        if (userBalance > balancePrev + 0.009) {
            balanceGain = userBalance - balancePrev
            balanceIncreased(balanceGain)
        }
        balancePrev = userBalance
    }

    // Keep display balance bound to Main.sessionBalance (checkout must not break this).
    Binding {
        target: dashboardRoot
        property: "userBalance"
        value: (typeof root !== 'undefined' && root !== null) ? root.sessionBalance : 0.0
        when: typeof root !== 'undefined' && root !== null
    }

    property int termId: (typeof root !== 'undefined' && root !== null) ? root.terminalId : 0
    property string pcType: (typeof root !== 'undefined' && root !== null) ? root.pcTypeFromDatabase : "singl"
    property string zoneNameFromDb: (typeof root !== 'undefined' && root !== null) ? root.zoneNameFromDatabase : ""

    property bool isProBootcamp: {
        var z = pcType.toLowerCase()
        return z === "bootcamp" || z === "bootcamp-pro" || z === "trio" || z === "kvatro" || z === "vip" || z === "pro"
    }

    property string zoneTitle: {
        var name = String(zoneNameFromDb || "").trim()
        if (name.length > 0)
            return name.toUpperCase() + " ZONE"
        var z = String(pcType || "").trim().toLowerCase()
        if (z === "singl" || z === "single" || z === "solo" || z === "standard" || z === "standart")
            return "СИНГЛ ZONE"
        if (z === "duo") return "ДУО ZONE"
        if (z === "trio") return "ТРИО ZONE"
        if (z === "kvatro" || z === "quatro") return "КВАТРО ZONE"
        if (z === "bootcamp" || z === "bootkamp") return "БУТКАМП ZONE"
        if (z === "tv") return "ТВ ZONE"
        if (z.length > 0) return z.toUpperCase() + " ZONE"
        return "ЗОНА"
    }

    // Палитра живёт в Theme (акцент зоны единый для логина, загрузки и дашборда)
    readonly property color accentColor: Theme.accent
    readonly property color darkBg: Theme.bgDeep
    property string currentLanguage: "RU"

    // Выбор личного / клубного аккаунта перед запуском любой игры
    property int pendingGameId: 0
    property string pendingGameTitle: ""
    property string pendingGameExe: ""
    property string pendingGameArgs: ""
    property string pendingGamePlatform: ""

    readonly property string defaultRiotClient: "C:\\Riot Games\\Riot Client\\RiotClientServices.exe"
    readonly property string defaultEpicLauncher: "C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe"
    readonly property string defaultEaDesktop: "C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe"

    readonly property var epicLauncherPaths: [
        "C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe",
        "C:\\Program Files (x86)\\Epic Games\\Launcher\\Portal\\Binaries\\Win32\\EpicGamesLauncher.exe",
        "C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe"
    ]
    readonly property var eaLauncherPaths: [
        "C:\\Program Files\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe",
        "C:\\Program Files\\Electronic Arts\\EA Desktop\\EADesktop.exe"
    ]
    readonly property var battleNetPaths: [
        "C:\\Program Files (x86)\\Battle.net\\Battle.net Launcher.exe",
        "C:\\Program Files (x86)\\Battle.net\\Battle.net.exe",
        "C:\\Program Files\\Battle.net\\Battle.net Launcher.exe",
        "C:\\Program Files\\Battle.net\\Battle.net.exe"
    ]
    readonly property var ubisoftPaths: [
        "C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\UbisoftConnect.exe",
        "C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\upc.exe",
        "C:\\Program Files\\Ubisoft\\Ubisoft Game Launcher\\UbisoftConnect.exe",
        "C:\\Program Files\\Ubisoft\\Ubisoft Game Launcher\\upc.exe"
    ]
    readonly property var lestaPaths: [
        "C:\\Program Files (x86)\\Lesta\\GameCenter\\LestaGameCenter.exe",
        "C:\\Program Files\\Lesta\\GameCenter\\LestaGameCenter.exe",
        "C:\\Games\\Lesta Game Center\\LestaGameCenter.exe",
        "C:\\Users\\Public\\Desktop\\Lesta Game Center.lnk"
    ]
    readonly property var vkPlayPaths: [
        "C:\\Users\\Public\\Desktop\\VK Play.lnk",
        "C:\\Program Files (x86)\\VK Play\\GameCenter\\GameCenter.exe",
        "C:\\Program Files\\VK Play\\GameCenter\\GameCenter.exe",
        "C:\\Program Files (x86)\\MyGames\\GameCenter\\GameCenter.exe",
        "C:\\Program Files\\MyGames\\GameCenter\\GameCenter.exe"
    ]

    function clearGameSearch() {
        // Drop leaked SendInput / Ctrl+V credentials from search; blur so keys miss the field.
        if (typeof gameSearchInput !== "undefined" && gameSearchInput) {
            gameSearchInput.text = ""
            gameSearchInput.focus = false
            gameSearchInput.readOnly = (typeof root !== "undefined"
                                        && root !== null
                                        && (root.isLoggingIn || root.gameLoadingVisible))
        }
        if (typeof gamesModel !== "undefined" && gamesModel)
            gamesModel.setSearchQuery("")
    }

    // "HH:MM:SS" -> секунды; -1 если строку разобрать не удалось
    function sessionSecondsLeft(hhmmss) {
        var parts = String(hhmmss || "").split(":")
        if (parts.length < 2)
            return -1
        var h = parseInt(parts[0], 10)
        var m = parseInt(parts[1], 10)
        var s = (parts.length > 2) ? parseInt(parts[2], 10) : 0
        if (isNaN(h) || isNaN(m) || isNaN(s))
            return -1
        return h * 3600 + m * 60 + s
    }

    function launchQuickClient(paths) {
        clearGameSearch()
        if (typeof Launcher === "undefined") {
            console.warn("[LAUNCH] Launcher недоступен")
            return
        }
        if (typeof Launcher.launchFirstExisting === "function") {
            Launcher.launchFirstExisting(paths, "")
            return
        }
        // fallback: старый API — первый путь
        if (paths && paths.length > 0)
            Launcher.launch(paths[0], "", "", "", "")
    }

    Connections {
        target: typeof Launcher !== "undefined" ? Launcher : null
        function onClearGameSearchRequested() {
            dashboardRoot.clearGameSearch()
        }
    }

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

    function looksLikeEpic(platform, exePath, args) {
        var p = String(platform || "").toLowerCase()
        var e = String(exePath || "").toLowerCase()
        var a = String(args || "").toLowerCase()
        return p === "epic" || p.indexOf("epic") >= 0
            || e.indexOf("epicgameslauncher") >= 0 || e.indexOf("epic games") >= 0
            || a.indexOf("com.epicgames.launcher") >= 0
    }

    function looksLikeEa(platform, exePath, args) {
        var p = String(platform || "").toLowerCase()
        var e = String(exePath || "").toLowerCase()
        var a = String(args || "").toLowerCase()
        return p === "ea" || p === "origin" || p === "eadesktop" || p === "eaapp"
            || p.indexOf("ea ") >= 0 || p.indexOf("electronic arts") >= 0
            || e.indexOf("eadesktop.exe") >= 0 || e.indexOf("ea desktop") >= 0
            || a.indexOf("origin2://") >= 0 || a.indexOf("origin://") >= 0
            || a.indexOf("eadm://") >= 0
    }

    function looksLikeSteam(platform, exePath, args) {
        var p = String(platform || "").toLowerCase()
        var e = String(exePath || "").toLowerCase()
        var a = String(args || "").toLowerCase()
        var fileName = e.split("\\").pop().split("/").pop()
        return p === "steam" || p === "valve"
            || fileName === "steam.exe"
            || a.indexOf("steam://") >= 0 || a.indexOf("-applaunch") >= 0
    }

    function normalizePlatform(platform, exePath, args, title) {
        if (looksLikeRiot(platform, exePath, args, title))
            return "riot"
        if (looksLikeEpic(platform, exePath, args))
            return "epic"
        if (looksLikeEa(platform, exePath, args))
            return "ea"
        if (looksLikeSteam(platform, exePath, args))
            return "steam"
        var p = String(platform || "").toLowerCase().trim()
        return p.length > 0 ? p : "pc"
    }

    function platformBrandTitle(key) {
        switch (String(key || "").toLowerCase()) {
        case "riot": return "RIOT GAMES"
        case "steam": return "STEAM"
        case "epic": return "EPIC GAMES"
        case "ea": return "EA APP"
        default:
            var k = String(key || "").toUpperCase()
            return k.length > 0 ? k : "ИГРА"
        }
    }

    function platformBrandColor(key) {
        switch (String(key || "").toLowerCase()) {
        case "riot": return "#d32f2f"
        case "steam": return "#00adef"
        case "epic": return "#ffffff"
        case "ea": return "#ff5722"
        default: return accentColor
        }
    }

    function platformShortName(key) {
        switch (String(key || "").toLowerCase()) {
        case "riot": return "Riot"
        case "steam": return "Steam"
        case "epic": return "Epic"
        case "ea": return "EA"
        default: return "платформы"
        }
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

    function resolvePersonalExe(platform, exePath) {
        var e = String(exePath || "").trim()
        if (platform === "riot")
            return resolveRiotExe(e)
        if (e.length > 0)
            return e
        if (platform === "epic")
            return defaultEpicLauncher
        if (platform === "ea")
            return defaultEaDesktop
        return e
    }

    function resolvePersonalArgs(platform, args, title, exePath) {
        if (platform === "riot")
            return resolveRiotArgs(args, title, exePath)
        return String(args || "")
    }

    function isLaunchBlocked() {
        if (typeof root !== 'undefined' && root !== null) {
            if (root.isLoggingIn || root.gameLoadingVisible)
                return true
        }
        if (typeof Launcher !== 'undefined' && typeof Launcher.isSessionBusy === 'function'
                && Launcher.isSessionBusy())
            return true
        return false
    }

    function openAccountChoice(gameId, platform, title, exePath, args) {
        if (isLaunchBlocked()) {
            console.log("[LAUNCH] ignore tile click — session busy / overlay visible")
            return
        }
        pendingGameId = parseInt(gameId) || 0
        pendingGameTitle = title || ""
        pendingGameExe = exePath || ""
        pendingGameArgs = args || ""
        pendingGamePlatform = normalizePlatform(platform, exePath, args, title)
        accountChoicePopup.open()
    }

    function launchPersonal() {
        var plat = normalizePlatform(pendingGamePlatform, pendingGameExe, pendingGameArgs, pendingGameTitle)
        var title = pendingGameTitle || platformShortName(plat)
        var exe = resolvePersonalExe(plat, pendingGameExe)
        var args = resolvePersonalArgs(plat, pendingGameArgs, title, pendingGameExe)
        accountChoicePopup.close()
        clubBusyHintPopup.close()
        clearGameSearch()
        if (typeof root !== 'undefined') {
            root.isLoggingIn = true
            root.currentGameId = pendingGameId
            root.showGameLoading(plat, title)
        }
        if (typeof Launcher === 'undefined') {
            console.error("[LAUNCH] Launcher не найден")
            if (typeof root !== 'undefined') {
                root.isLoggingIn = false
                root.hideGameLoading()
            }
            return
        }
        var payload = {
            "platform": plat,
            "platform_source": "personal_account",
            "exe_path": exe,
            "args": args,
            "login": "",
            "password": "",
            "game_id": pendingGameId,
            "game_title": title,
            "terminal_id": parseInt(dashboardRoot.termId),
            "auth": { "mode": "personal" }
        }
        console.log("[LAUNCH] личный аккаунт →", plat, exe, args)
        if (typeof NetworkManager !== 'undefined' && pendingGameId > 0)
            NetworkManager.recordGameLaunch(pendingGameId)
        Launcher.launchPlatformSessionString(JSON.stringify(payload), "")
        // Оверлей держит C++ до hideShell / gameStartedSuccessfully — не scheduleHide рано
    }

    function launchClub() {
        var gameId = pendingGameId
        var plat = normalizePlatform(pendingGamePlatform, pendingGameExe, pendingGameArgs, pendingGameTitle)
        var title = pendingGameTitle || platformShortName(plat)
        accountChoicePopup.close()
        clubBusyHintPopup.close()
        clearGameSearch()
        if (typeof root !== 'undefined') {
            root.isLoggingIn = true
            root.currentGameId = gameId
            root.showGameLoading(plat, title)
        }
        startClubTakeAccount(gameId, plat, title, pendingGameExe, pendingGameArgs)
    }

    // Единственный источник адреса бэкенда — Network/api_ip + Network/api_port
    // из config.ini (нормализуются в NetworkManager::buildServerUrl).
    function apiBase() {
        if (typeof NetworkManager !== 'undefined' && NetworkManager.serverUrl)
            return String(NetworkManager.serverUrl)
        console.warn("[NET] Адрес бэкенда не задан: проверьте Network/api_ip в config.ini")
        return ""
    }

    function startClubTakeAccount(gameId, modelPlatform, modelTitle, modelExe, modelArgs) {
        var baseUrl = dashboardRoot.apiBase()
        if (baseUrl.length === 0)
            return

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
                            if (typeof NetworkManager !== 'undefined' && gameId > 0)
                                NetworkManager.recordGameLaunch(parseInt(gameId))
                            Launcher.launchPlatformSessionString(JSON.stringify(res), String(res.platform_app_id || ""))
                        } else {
                            console.error("[SESSION] Launcher не найден")
                        }
                    } else {
                        console.warn("[SESSION] take-account:", res.message || "ошибка")
                        if (String(res.message || "").indexOf("занят") >= 0) {
                            clubBusyHintPopup.open()
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
            NetworkManager.startClimateControl()
            climateControl.syncFromNetwork()
            // Логин успел положить чек до загрузки Dashboard.
            if (NetworkManager.pendingReceiptUrl && NetworkManager.pendingReceiptUrl.length > 0)
                Qt.callLater(openFiscalReceiptPopup)
        }
    }

    function openFiscalReceiptPopup() {
        if (typeof NetworkManager === 'undefined')
            return
        const url = NetworkManager.pendingReceiptUrl || ""
        if (!url || url.length < 1)
            return
        fiscalReceiptPopup.receiptUrl = url
        fiscalReceiptPopup.receiptAmount = NetworkManager.pendingReceiptAmount || 0
        fiscalReceiptPopup.isStub = !!NetworkManager.pendingReceiptStub
        fiscalReceiptPopup.description = NetworkManager.pendingReceiptDescription || ""
        fiscalReceiptPopup.open()
    }

    function closeFiscalReceiptPopup() {
        fiscalReceiptPopup.close()
        if (typeof NetworkManager !== 'undefined')
            NetworkManager.clearPendingReceipt()
    }

    Connections {
        target: typeof NetworkManager !== 'undefined' ? NetworkManager : null
        function onFiscalReceiptReady(url, amount, isStub, description) {
            fiscalReceiptPopup.receiptUrl = url || ""
            fiscalReceiptPopup.receiptAmount = amount || 0
            fiscalReceiptPopup.isStub = !!isStub
            fiscalReceiptPopup.description = description || ""
            if (fiscalReceiptPopup.receiptUrl.length > 0)
                fiscalReceiptPopup.open()
        }
    }

    // --- Климат-контроль: декор оборотов / кнопка ---
    QtObject {
        id: climateControl
        property bool starting: false
        property bool running: false
        property bool stopping: false
        property int rpm: 0
        property int targetRpm: 1500
        property int pendingFinalRpm: -1
        property bool busy: starting || stopping

        function rpmForSpeed(speed) {
            var s = parseInt(speed)
            if (s >= 3)
                return 3000
            if (s >= 2)
                return 2250
            return 1500
        }

        function rpmForPercent(p) {
            var n = parseInt(p)
            if (n >= 100)
                return 3000
            if (n >= 75)
                return 2250
            return 1500
        }

        function startRamp(toRpm) {
            var rising = toRpm > rpm
            starting = rising && toRpm > 0
            stopping = !rising
            running = false
            if (rising && toRpm > 0)
                blinkAnim.restart()
            else
                blinkAnim.stop()

            rpmRamp.to = toRpm
            rpmRamp.duration = Math.max(600, Math.abs(toRpm - rpm) * 1.2)
            rpmRamp.easing.type = rising ? Easing.OutCubic : Easing.InOutQuad
            rpmRamp.start()
        }

        function rampTo(target) {
            target = Math.max(0, Math.min(3000, parseInt(target) || 0))
            if (pendingFinalRpm < 0
                    && target === targetRpm
                    && (running || starting || stopping)
                    && Math.abs(rpm - target) < 40)
                return

            rpmRamp.stop()
            rpmJitter.stop()
            blinkAnim.stop()
            if (typeof softStepHold !== "undefined")
                softStepHold.stop()
            pendingFinalRpm = -1

            if (target <= 0) {
                targetRpm = 0
                starting = false
                stopping = true
                running = false
                rpmRamp.to = 0
                rpmRamp.duration = Math.max(800, Math.abs(rpm) * 2)
                rpmRamp.easing.type = Easing.InQuad
                rpmRamp.start()
                return
            }

            // 50%↔100%: сначала ~2250 (~2.5с как на реле), потом цель
            var mid = 2250
            var crosses = (rpm <= 1800 && target >= 2700) || (rpm >= 2700 && target <= 1800)
            if (crosses) {
                targetRpm = target
                pendingFinalRpm = target
                startRamp(mid)
                return
            }

            targetRpm = target
            startRamp(target)
        }

        function setRunningSteady() {
            starting = false
            stopping = false
            running = targetRpm > 0
            blinkAnim.stop()
            if (targetRpm > 0 && Math.abs(rpm - targetRpm) > 80)
                rpm = targetRpm
            if (running)
                rpmJitter.restart()
            else
                rpmJitter.stop()
        }

        function syncFromNetwork() {
            if (typeof NetworkManager === "undefined" || !NetworkManager.fanAvailable) {
                rampTo(0)
                return
            }
            // desired speed 1..3 → 1500 / 2250 / 3000
            rampTo(rpmForSpeed(NetworkManager.fanSpeed))
        }

        function isLocked() {
            return typeof NetworkManager !== "undefined"
                   && NetworkManager.fanManualLockSec > 0
        }

        function speedForPercent(p) {
            var n = parseInt(p)
            if (n >= 100)
                return 3
            if (n >= 75)
                return 2
            return 1
        }

        function toggle() {
            if (typeof NetworkManager === "undefined")
                return
            if (isLocked())
                return
            if (NetworkManager.fanMode === "auto") {
                NetworkManager.setFan("100")
            } else if (NetworkManager.fanSpeed >= 3) {
                NetworkManager.setFan("75")
            } else if (NetworkManager.fanSpeed >= 2) {
                NetworkManager.setFan("50")
            } else {
                NetworkManager.setFan("auto")
            }
            // RPM только после реального ответа API (onFanStateChanged)
        }

        function setPercent(p) {
            if (typeof NetworkManager === "undefined")
                return
            if (isLocked())
                return
            // Уже на этой мощности — не дергаем API и не крутим эмуляцию
            if (NetworkManager.fanMode !== "auto"
                    && NetworkManager.fanSpeed === speedForPercent(p))
                return
            NetworkManager.setFan(String(p))
        }

        function setAuto() {
            if (typeof NetworkManager === "undefined")
                return
            if (NetworkManager.fanMode === "auto")
                return
            if (isLocked())
                return
            NetworkManager.setFan("auto")
        }
    }

    NumberAnimation {
        id: rpmRamp
        target: climateControl
        property: "rpm"
        onFinished: {
            if (!climateControl.starting && !climateControl.stopping)
                return

            // Мягкий шаг эмуляции: подержали mid, идём к финалу
            if (climateControl.pendingFinalRpm >= 0
                    && Math.abs(climateControl.rpm - 2250) <= 80) {
                var finalRpm = climateControl.pendingFinalRpm
                climateControl.pendingFinalRpm = -1
                climateControl.targetRpm = 2250
                climateControl.setRunningSteady()
                softStepHold.finalRpm = finalRpm
                softStepHold.restart()
                return
            }

            if (Math.abs(climateControl.rpm - climateControl.targetRpm) <= 30
                    || climateControl.rpm === rpmRamp.to) {
                climateControl.rpm = climateControl.targetRpm
                climateControl.setRunningSteady()
            }
        }
    }

    Timer {
        id: softStepHold
        interval: 2500
        repeat: false
        property int finalRpm: 3000
        onTriggered: climateControl.rampTo(finalRpm)
    }

    Timer {
        id: rpmJitter
        interval: 180
        repeat: true
        running: false
        onTriggered: {
            if (!climateControl.running || climateControl.targetRpm <= 0)
                return
            var base = climateControl.targetRpm
            var wobble = Math.max(25, Math.floor(base * 0.02))
            climateControl.rpm = base - wobble + Math.floor(Math.random() * (wobble * 2 + 1))
        }
    }

    SequentialAnimation {
        id: blinkAnim
        loops: Animation.Infinite
        running: false
        NumberAnimation {
            target: climatePowerPulse
            property: "opacity"
            to: 1.0
            duration: 90
        }
        NumberAnimation {
            target: climatePowerPulse
            property: "opacity"
            to: 0.15
            duration: 90
        }
        onStopped: {
            if (climatePowerPulse)
                climatePowerPulse.opacity = 0
        }
    }

    Connections {
        target: (typeof NetworkManager !== "undefined") ? NetworkManager : null
        function onFanStateChanged() {
            if (typeof NetworkManager === "undefined")
                return
            climateControl.syncFromNetwork()
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

    // Mid-session: game still running under shell — return without ending session
    readonly property bool showReturnToGame: typeof Launcher !== "undefined"
        && Launcher.hasActiveGame
        && !Launcher.shellHiddenForGame
        && !(typeof root !== "undefined" && root !== null && root.gameLoadingVisible)

    Rectangle {
        id: returnToGameBanner
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 16
        height: 52
        radius: 6
        z: 250
        visible: dashboardRoot.showReturnToGame
        color: "#0a1f12"
        border.color: accentColor
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 12
            spacing: 14

            Text {
                text: "▶"
                color: accentColor
                font.pixelSize: 18
                font.bold: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: "ИГРА НА ПАУЗЕ ОВЕРЛЕЯ"
                    color: accentColor
                    font.pixelSize: Theme.fontCaption
                    font.bold: true
                    font.letterSpacing: 1.5
                    opacity: 0.7
                }
                Text {
                    text: (typeof Launcher !== "undefined" && Launcher.gameTitle)
                          ? ("Сессия: " + Launcher.gameTitle)
                          : "Игровая сессия активна"
                    color: Theme.textBody
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            Rectangle {
                Layout.preferredWidth: returnGameLabel.implicitWidth + 28
                Layout.preferredHeight: 36
                radius: 4
                color: returnGameMouse.containsMouse ? accentColor : "#052e16"
                border.color: accentColor
                border.width: 1
                Text {
                    id: returnGameLabel
                    anchors.centerIn: parent
                    text: "ВЕРНУТЬСЯ В ИГРУ"
                    color: returnGameMouse.containsMouse ? "#030704" : accentColor
                    font.pixelSize: 13
                    font.bold: true
                    font.letterSpacing: 1
                }
                MouseArea {
                    id: returnGameMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof Launcher !== "undefined")
                            Launcher.switchToGame()
                    }
                }
            }
        }
    }

    // Active shop order contents — compact neon panel, top-right
    readonly property bool showOrderContents: typeof root !== "undefined" && root !== null
        && root.hasActiveOrder
        && (root.orderStatusCode === "pending" || root.orderStatusCode === "cooking"
            || (root.orderStatusText.indexOf("ВЫПОЛНЕН") < 0
                && root.orderStatusText.indexOf("ОТМЕН") < 0
                && root.orderItems && root.orderItems.length > 0))
        && root.orderItems && root.orderItems.length > 0

    Rectangle {
        id: orderContentsPanel
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: dashboardRoot.showReturnToGame ? 84 : 20
        anchors.rightMargin: 20
        z: 240
        width: Math.min(280, parent.width * 0.22)
        height: orderContentsCol.implicitHeight + 24
        radius: 6
        color: "#0a0c05"
        border.color: (typeof root !== "undefined" && root.orderStatusCode === "cooking")
                      ? Theme.warning : Theme.shop
        border.width: 1
        opacity: dashboardRoot.showOrderContents ? 1.0 : 0.0
        visible: opacity > 0.01
        scale: dashboardRoot.showOrderContents ? 1.0 : 0.96

        Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
        Behavior on border.color { ColorAnimation { duration: 180 } }

        Column {
            id: orderContentsCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 8

            Row {
                spacing: 8
                width: parent.width
                Rectangle {
                    width: 6
                    height: 6
                    radius: 3
                    color: orderContentsPanel.border.color
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: "ВАШ ЗАКАЗ"
                    color: orderContentsPanel.border.color
                    font.pixelSize: Theme.fontCaption
                    font.bold: true
                    font.letterSpacing: 1.5
                    anchors.verticalCenter: parent.verticalCenter
                }
                Item { width: 1; height: 1 }
                Text {
                    text: (typeof root !== "undefined") ? root.orderStatusText : ""
                    color: (typeof root !== "undefined" && root.orderStatusCode === "cooking")
                           ? Theme.warning : "#f59e0b"
                    font.pixelSize: Theme.fontCaption
                    font.bold: true
                    elide: Text.ElideRight
                    width: Math.max(40, orderContentsCol.width - 120)
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignRight
                }
            }

            Repeater {
                model: (typeof root !== "undefined" && root.orderItems) ? root.orderItems : []
                delegate: Row {
                    width: orderContentsCol.width
                    spacing: 6
                    Text {
                        text: (modelData.name || "") + (modelData.qty > 1 ? (" ×" + modelData.qty) : "")
                        color: Theme.textBody
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        width: parent.width - priceLbl.width - 8
                    }
                    Text {
                        id: priceLbl
                        text: (typeof modelData.price === "number"
                               ? modelData.price.toFixed(0)
                               : modelData.price) + " ₽"
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        font.family: "Monospace"
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(0.92, 0.80, 0.08, 0.35)
                visible: typeof root !== "undefined" && root.orderItemsTotal > 0
            }

            Text {
                visible: typeof root !== "undefined" && root.orderItemsTotal > 0
                text: "ИТОГО: " + (typeof root !== "undefined"
                                   ? root.orderItemsTotal.toFixed(0) : "0") + " ₽"
                color: Theme.warning
                font.pixelSize: 12
                font.bold: true
                anchors.right: parent.right
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 40
        anchors.topMargin: dashboardRoot.showReturnToGame ? 84 : 40
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

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        id: topTilesRow
                        Layout.fillWidth: true
                        spacing: 10

                        // Общая высота плиток: климат и SOS одной высоты
                        readonly property int tileSize: 72
                        readonly property int climateH: 88

                        Rectangle {
                            id: climateBox
                            Layout.fillWidth: true
                            Layout.preferredHeight: topTilesRow.climateH
                            Layout.minimumHeight: topTilesRow.climateH
                            Layout.maximumHeight: topTilesRow.climateH
                            Layout.alignment: Qt.AlignVCenter
                            color: "#0a0f0b"
                            border.color: (climateControl.running || climateControl.starting)
                                          ? Qt.rgba(0.13, 0.77, 0.36, 0.55)
                                          : Qt.rgba(0.13, 0.77, 0.36, 0.3)
                            border.width: 1
                            radius: 4
                            Behavior on border.color { ColorAnimation { duration: 200 } }

                            // Обратный отсчёт cooldown в правом верхнем углу
                            Text {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.topMargin: 5
                                anchors.rightMargin: 8
                                z: 5
                                visible: typeof NetworkManager !== "undefined"
                                         && NetworkManager.fanManualLockSec > 0
                                text: NetworkManager.fanManualLockSec + "с"
                                color: Theme.warning
                                font.pixelSize: 14
                                font.bold: true
                                font.family: "Monospace"
                                opacity: 0.95
                            }

                            ColumnLayout {
                                id: climateCol
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 8
                                anchors.topMargin: 4
                                anchors.bottomMargin: 4
                                spacing: 3

                                // Строка 1: питание + RPM
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 6

                                    Rectangle {
                                        id: climatePowerBtn
                                        Layout.preferredWidth: 30
                                        Layout.preferredHeight: 30
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: 4
                                        color: {
                                            if (climateControl.starting)
                                                return "#14532d"
                                            if (climateControl.running)
                                                return Theme.success
                                            return Theme.danger
                                        }
                                        border.color: climatePowerBtnMouse.containsMouse
                                                      ? "white"
                                                      : (climateControl.running || climateControl.starting
                                                         ? Qt.lighter(Theme.success, 1.2)
                                                         : Qt.lighter(Theme.danger, 1.15))
                                        border.width: climatePowerBtnMouse.containsMouse ? 2 : 1
                                        scale: climatePowerBtnMouse.pressed ? 0.94 : (climatePowerBtnMouse.containsMouse ? 1.04 : 1.0)
                                        opacity: NetworkManager.fanManualLockSec > 0 ? 0.7 : 1.0
                                        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
                                        Behavior on color { ColorAnimation { duration: 180 } }

                                        Rectangle {
                                            id: climatePowerPulse
                                            anchors.fill: parent
                                            radius: parent.radius
                                            color: "#4ade80"
                                            opacity: 0
                                            z: 0
                                        }

                                        Canvas {
                                            id: powerGlyph
                                            anchors.centerIn: parent
                                            z: 1
                                            width: 15
                                            height: 15
                                            antialiasing: true

                                            property color strokeColor: climatePowerBtnMouse.pressed ? "#111111" : "white"
                                            onStrokeColorChanged: requestPaint()

                                            onPaint: {
                                                var ctx = getContext("2d")
                                                ctx.reset()
                                                var cx = width / 2
                                                var cy = height / 2
                                                var r = width / 2 - 2.8
                                                ctx.lineWidth = 2.0
                                                ctx.lineCap = "round"
                                                ctx.strokeStyle = strokeColor
                                                ctx.beginPath()
                                                ctx.arc(cx, cy + 1, r, -Math.PI / 3, Math.PI + Math.PI / 3)
                                                ctx.stroke()
                                                ctx.beginPath()
                                                ctx.moveTo(cx, cy - r - 1.2)
                                                ctx.lineTo(cx, cy)
                                                ctx.stroke()
                                            }
                                        }

                                        MouseArea {
                                            id: climatePowerBtnMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: climateControl.toggle()
                                        }
                                    }

                                    Column {
                                        Layout.alignment: Qt.AlignVCenter
                                        spacing: 0
                                        Text {
                                            text: "КЛИМАТ КОНТРОЛЬ"
                                            color: accentColor
                                            font.pixelSize: 9
                                            font.bold: true
                                            font.letterSpacing: 1.4
                                            opacity: 0.7
                                        }
                                        Row {
                                            spacing: 5
                                            Text {
                                                text: climateControl.rpm.toString()
                                                color: climateControl.running || climateControl.starting || climateControl.stopping
                                                       ? Theme.textPrimary
                                                       : Theme.textMuted
                                                font.pixelSize: 16
                                                font.bold: true
                                                font.family: "Monospace"
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                            Text {
                                                text: "RPM"
                                                color: accentColor
                                                font.pixelSize: Theme.fontCaption
                                                font.bold: true
                                                opacity: 0.65
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                            Item {
                                                id: fanGlyph
                                                width: 18
                                                height: 18
                                                anchors.verticalCenter: parent.verticalCenter
                                                opacity: (climateControl.running || climateControl.starting)
                                                         ? 1 : (climateControl.stopping ? 0.7 : 0.35)

                                                Canvas {
                                                    id: fanCanvas
                                                    anchors.fill: parent
                                                    antialiasing: true
                                                    onPaint: {
                                                        var ctx = getContext("2d")
                                                        ctx.reset()
                                                        ctx.translate(width / 2, height / 2)
                                                        ctx.fillStyle = accentColor
                                                        for (var i = 0; i < 3; i++) {
                                                            ctx.rotate(Math.PI * 2 / 3)
                                                            ctx.beginPath()
                                                            ctx.moveTo(0, 0)
                                                            ctx.quadraticCurveTo(6, -2.2, 8.5, -0.9)
                                                            ctx.quadraticCurveTo(4.5, 3.2, 0, 0)
                                                            ctx.fill()
                                                        }
                                                        ctx.beginPath()
                                                        ctx.arc(0, 0, 1.9, 0, Math.PI * 2)
                                                        ctx.fillStyle = Theme.textPrimary
                                                        ctx.fill()
                                                    }
                                                    Component.onCompleted: requestPaint()

                                                    RotationAnimator on rotation {
                                                        from: 0
                                                        to: 360
                                                        duration: climateControl.targetRpm >= 2800 ? 380
                                                                  : (climateControl.targetRpm >= 2000 ? 520
                                                                     : (climateControl.targetRpm > 0 ? 780 : 1600))
                                                        loops: Animation.Infinite
                                                        running: climateControl.running
                                                                 || climateControl.starting
                                                                 || climateControl.stopping
                                                    }
                                                }
                                            }
                                            Text {
                                                visible: typeof NetworkManager !== "undefined" && NetworkManager.cpuTempC > 0
                                                text: NetworkManager.cpuTempC.toFixed(0) + "°"
                                                color: Theme.textMuted
                                                font.pixelSize: 11
                                                font.family: "Monospace"
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                        }
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                // Строка 2: скорости
                                RowLayout {
                                    id: fanSpeedRadios
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 22
                                    spacing: 4
                                    enabled: typeof NetworkManager !== "undefined"
                                             && NetworkManager.fanAvailable
                                             && NetworkManager.fanManualLockSec <= 0
                                    opacity: enabled ? 1 : 0.4

                                    Repeater {
                                        model: [
                                            { label: "50%", action: "50" },
                                            { label: "75%", action: "75" },
                                            { label: "100%", action: "100" },
                                            { label: "AUTO", action: "auto" }
                                        ]
                                        delegate: Rectangle {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            radius: 3
                                            readonly property bool selected: {
                                                if (typeof NetworkManager === "undefined")
                                                    return false
                                                if (modelData.action === "auto")
                                                    return NetworkManager.fanMode === "auto"
                                                if (modelData.action === "50")
                                                    return NetworkManager.fanMode !== "auto"
                                                           && NetworkManager.fanSpeed === 1
                                                if (modelData.action === "75")
                                                    return NetworkManager.fanMode !== "auto"
                                                           && NetworkManager.fanSpeed === 2
                                                return NetworkManager.fanMode !== "auto"
                                                       && NetworkManager.fanSpeed >= 3
                                            }
                                            color: selected ? accentColor : "#111111"
                                            border.color: selected ? Qt.lighter(accentColor, 1.2) : "#333333"
                                            border.width: 1

                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.label
                                                color: selected ? "#111111" : Theme.textPrimary
                                                font.pixelSize: 10
                                                font.bold: true
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (modelData.action === "auto")
                                                        climateControl.setAuto()
                                                    else
                                                        climateControl.setPercent(parseInt(modelData.action))
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: sosBtn
                            Layout.preferredWidth: topTilesRow.climateH
                            Layout.preferredHeight: topTilesRow.climateH
                            Layout.minimumHeight: topTilesRow.climateH
                            Layout.maximumHeight: topTilesRow.climateH
                            Layout.alignment: Qt.AlignVCenter
                            radius: 4
                            // Та же «воздушная» подача, что у климат-блока:
                            // тёмная подложка и приглушённый контур, цвет — на ховере.
                            color: sosMouse.pressed
                                   ? "#2a0b0b"
                                   : (sosMouse.containsMouse ? "#1a0c0c" : "#0f0a0a")
                            border.color: sosMouse.pressed || sosMouse.containsMouse
                                          ? Qt.rgba(0.94, 0.27, 0.27, 0.85)
                                          : Qt.rgba(0.94, 0.27, 0.27, 0.3)
                            border.width: 1
                            scale: sosMouse.pressed ? 0.94 : (sosMouse.containsMouse ? 1.04 : 1.0)
                            opacity: sosMouse.pressed ? 0.9 : 1.0
                            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                            Behavior on opacity { NumberAnimation { duration: 100 } }
                            Behavior on color { ColorAnimation { duration: 120 } }
                            Behavior on border.color { ColorAnimation { duration: 200 } }

                            Column {
                                anchors.centerIn: parent
                                spacing: 2
                                Text { text: "⚠️"; font.pixelSize: 16; anchors.horizontalCenter: parent.horizontalCenter }
                                Text {
                                    text: "SOS"
                                    color: Theme.danger
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.letterSpacing: 2
                                    anchors.horizontalCenter: parent.horizontalCenter
                                }
                            }

                            MouseArea {
                                id: sosMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: sosReasonPopup.open()
                            }
                        }
                    }

                    Row {
                        Layout.alignment: Qt.AlignLeft
                        spacing: 10

                        Row {
                            spacing: 8
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: accentColor
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "LATENCY (EU): "
                                      + (typeof NetworkManager !== 'undefined'
                                         ? NetworkManager.getLatency("162.249.72.1") : 24)
                                      + " MS"
                                color: accentColor
                                font.pixelSize: Theme.fontCaption
                                font.bold: true
                                opacity: 0.85
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        Rectangle {
                            width: 1
                            height: 12
                            color: accentColor
                            opacity: 0.35
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Row {
                            spacing: 6
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                text: "CPU"
                                color: accentColor
                                font.pixelSize: Theme.fontCaption
                                font.bold: true
                                opacity: 0.55
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                readonly property real t: (typeof NetworkManager !== "undefined")
                                                          ? NetworkManager.cpuTempC : -1
                                readonly property bool ok: t > 0
                                text: ok ? (t.toFixed(0) + "°C") : "—"
                                color: !ok ? Theme.textMuted
                                     : (t >= 85 ? Theme.danger
                                        : (t >= 70 ? Theme.warning : accentColor))
                                font.pixelSize: Theme.fontCaption
                                font.bold: true
                                font.family: "Monospace"
                                opacity: ok ? 0.9 : 0.5
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    // Зона + пользователь + баланс
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        implicitHeight: zoneUserCol.implicitHeight + 22
                        radius: 8
                        color: Qt.rgba(accentColor.r, accentColor.g, accentColor.b, 0.06)
                        border.width: 1
                        border.color: Qt.rgba(accentColor.r, accentColor.g, accentColor.b, 0.22)

                        Column {
                            id: zoneUserCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Row {
                                width: parent.width
                                spacing: 8
                                Rectangle {
                                    width: 3
                                    height: zoneBadge.implicitHeight
                                    radius: 1
                                    color: accentColor
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    id: zoneBadge
                                    text: dashboardRoot.zoneTitle
                                    color: accentColor
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.letterSpacing: 1.6
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    width: Math.max(0, parent.width - zoneBadge.width - 19)
                                    text: dashboardRoot.userName
                                    color: Theme.textPrimary
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.letterSpacing: 1.6
                                    elide: Text.ElideRight
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            Item {
                                width: parent.width
                                height: sidebarBalance.implicitHeight

                                Text {
                                    id: sidebarBalance
                                    anchors.left: parent.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "БАЛАНС: " + dashboardRoot.userBalance.toFixed(0) + " ₽"
                                    color: Theme.textSecondary
                                    font.pixelSize: 18
                                    transformOrigin: Item.Left
                                }

                                Text {
                                    id: sidebarBalanceGain
                                    anchors.left: sidebarBalance.right
                                    anchors.leftMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "+" + dashboardRoot.balanceGain.toFixed(0) + " ₽"
                                    color: Theme.success
                                    font.pixelSize: 18
                                    font.bold: true
                                    opacity: 0
                                }

                                Connections {
                                    target: dashboardRoot
                                    function onBalanceIncreased(gain) {
                                        sidebarBalanceFlash.restart()
                                        sidebarGainFloat.restart()
                                    }
                                }

                                SequentialAnimation {
                                    id: sidebarBalanceFlash

                                    ParallelAnimation {
                                        NumberAnimation {
                                            target: sidebarBalance; property: "scale"
                                            to: 1.16; duration: 320; easing.type: Easing.OutBack
                                        }
                                        ColorAnimation {
                                            target: sidebarBalance; property: "color"
                                            to: Theme.success; duration: 320
                                        }
                                    }

                                    SequentialAnimation {
                                        loops: 2
                                        ColorAnimation {
                                            target: sidebarBalance; property: "color"
                                            to: Theme.textPrimary; duration: 260
                                        }
                                        ColorAnimation {
                                            target: sidebarBalance; property: "color"
                                            to: Theme.success; duration: 260
                                        }
                                    }

                                    ParallelAnimation {
                                        NumberAnimation {
                                            target: sidebarBalance; property: "scale"
                                            to: 1.0; duration: 520; easing.type: Easing.OutCubic
                                        }
                                        ColorAnimation {
                                            target: sidebarBalance; property: "color"
                                            to: Theme.textSecondary; duration: 640
                                        }
                                    }
                                }

                                SequentialAnimation {
                                    id: sidebarGainFloat

                                    PropertyAction {
                                        target: sidebarBalanceGain
                                        property: "anchors.verticalCenterOffset"; value: 0
                                    }
                                    ParallelAnimation {
                                        NumberAnimation {
                                            target: sidebarBalanceGain; property: "opacity"
                                            to: 1; duration: 300
                                        }
                                        NumberAnimation {
                                            target: sidebarBalanceGain
                                            property: "anchors.verticalCenterOffset"
                                            to: -10; duration: 300; easing.type: Easing.OutCubic
                                        }
                                    }
                                    PauseAnimation { duration: 1800 }
                                    ParallelAnimation {
                                        NumberAnimation {
                                            target: sidebarBalanceGain; property: "opacity"
                                            to: 0; duration: 800
                                        }
                                        NumberAnimation {
                                            target: sidebarBalanceGain
                                            property: "anchors.verticalCenterOffset"
                                            to: -26; duration: 800; easing.type: Easing.OutCubic
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Column {
                        Layout.fillWidth: true
                        spacing: 5
                        Item { height: 4; width: 1 }
                        Text { text: "ОСТАЛОСЬ ВРЕМЕНИ"; color: accentColor; font.pixelSize: Theme.fontCaption; opacity: 0.7 }
                        Text {
                            text: dashboardRoot.timeRemaining
                            font.pixelSize: Theme.fontHero
                            font.family: "Monospace"
                            font.bold: true
                            // < 5 мин — красный, < 15 мин — жёлтый
                            readonly property int secondsLeft: dashboardRoot.sessionSecondsLeft(dashboardRoot.timeRemaining)
                            // secondsLeft <= 0 — данных ещё нет / сессия закончилась
                            color: (secondsLeft <= 0) ? Theme.textPrimary
                                   : (secondsLeft < 300 ? Theme.danger
                                      : (secondsLeft < 900 ? Theme.warning : Theme.textPrimary))
                            Behavior on color { ColorAnimation { duration: 250 } }
                        }
                    }

                    Item { Layout.fillHeight: true }
                    Text { text: "БЫСТРЫЙ ЗАПУСК ПЛАТФОРМ"; color: accentColor; font.pixelSize: Theme.fontCaption; font.bold: true; font.letterSpacing: 2; opacity: 0.6; Layout.alignment: Qt.AlignHCenter }

                    GridLayout {
                        columns: 3
                        rows: 3
                        columnSpacing: 8
                        rowSpacing: 8
                        Layout.fillWidth: true
                        PlatformSquareBtn {
                            btnText: "STEAM"
                            iconSource: Qt.resolvedUrl("images/launchers/steam.png")
                            brandColor: "#00adef"
                            onClicked: {
                                // Личный Steam: оверлей держим до gameStartedSuccessfully из C++
                                // (не hideShell сразу — иначе оверлей не видно).
                                if (typeof root !== 'undefined') {
                                    root.isLoggingIn = true
                                    root.showGameLoading("steam", "Steam")
                                }
                                var mockAuth = {
                                    "platform": "steam",
                                    "platform_source": "personal_account",
                                    "login": "",
                                    "password": "",
                                    "args": "",
                                    "auth": { "mode": "personal" }
                                }
                                if (typeof Launcher !== 'undefined') {
                                    console.log("[QML-CLICK] Быстрый запуск чистого Steam...")
                                    Launcher.launchPlatformSession(mockAuth, "")
                                }
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "EPIC"
                            iconSource: Qt.resolvedUrl("images/launchers/epic.png")
                            brandColor: "#ffffff"
                            onClicked: {
                                console.log("[QML-CLICK] Epic Games Launcher...")
                                dashboardRoot.launchQuickClient(dashboardRoot.epicLauncherPaths)
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "EA APP"
                            iconSource: Qt.resolvedUrl("images/launchers/ea.png")
                            brandColor: "#ff5722"
                            onClicked: {
                                console.log("[QML-CLICK] EA App...")
                                dashboardRoot.launchQuickClient(dashboardRoot.eaLauncherPaths)
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "RIOT"
                            iconSource: Qt.resolvedUrl("images/launchers/riot.png")
                            brandColor: "#d32f2f"
                            onClicked: {
                                if (typeof root !== 'undefined') {
                                    root.isLoggingIn = true
                                    root.showGameLoading("riot", "Riot Client")
                                }
                                var mockAuth = {
                                    "platform": "riot",
                                    "platform_source": "personal_account",
                                    "exe_path": "C:\\Riot Games\\Riot Client\\RiotClientServices.exe",
                                    "args": "--launch-product=league_of_legends --launch-patchline=live",
                                    "login": "",
                                    "password": "",
                                    "auth": { "mode": "personal" }
                                }
                                if (typeof Launcher !== 'undefined') {
                                    console.log("[QML-CLICK] Личный Riot Client...")
                                    Launcher.launchPlatformSession(mockAuth, "")
                                }
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "BATTLE.NET"
                            iconSource: Qt.resolvedUrl("images/launchers/battlenet.png")
                            brandColor: "#00aeff"
                            onClicked: {
                                console.log("[QML-CLICK] Battle.net...")
                                dashboardRoot.launchQuickClient(dashboardRoot.battleNetPaths)
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "UBISOFT"
                            iconSource: Qt.resolvedUrl("images/launchers/ubisoft.png")
                            brandColor: "#ffffff"
                            onClicked: {
                                console.log("[QML-CLICK] Ubisoft Connect...")
                                dashboardRoot.launchQuickClient(dashboardRoot.ubisoftPaths)
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "LESTA"
                            iconSource: Qt.resolvedUrl("images/launchers/lesta.png")
                            brandColor: "#ff6a00"
                            onClicked: {
                                console.log("[QML-CLICK] Lesta Game Center...")
                                dashboardRoot.launchQuickClient(dashboardRoot.lestaPaths)
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "VK PLAY"
                            iconSource: Qt.resolvedUrl("images/launchers/vkplay.png")
                            brandColor: "#ff3347"
                            onClicked: {
                                console.log("[QML-CLICK] VK Play...")
                                dashboardRoot.launchQuickClient(dashboardRoot.vkPlayPaths)
                            }
                        }
                        PlatformSquareBtn {
                            btnText: "ROBLOX"
                            iconSource: Qt.resolvedUrl("images/launchers/roblox.png")
                            brandColor: "#e11d48"
                            onClicked: {
                                console.log("[QML-CLICK] Roblox...")
                                dashboardRoot.launchQuickClient([
                                    "C:\\Users\\Public\\Desktop\\Roblox Player.lnk",
                                    "C:\\Program Files (x86)\\Roblox\\Versions\\RobloxPlayerBeta.exe"
                                ])
                            }
                        }
                    }

                    Item { height: 5; width: 1 }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ActionBtn {
                                id: storeActionBtn
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                compact: true
                                text: "МАГАЗИН"
                                icon: "🛒"
                                baseColor: Theme.shop
                                isActiveStatus: (typeof root !== 'undefined') ? root.hasActiveOrder : false
                                orderIsFinished: (typeof root !== 'undefined'
                                                  && (root.orderStatusText.indexOf("ВЫПОЛНЕН") >= 0
                                                      || root.orderStatusText.indexOf("ОТМЕН") >= 0))
                                orderIsCooking: (typeof root !== 'undefined'
                                                 && (root.orderStatusCode === "cooking"
                                                     || root.orderStatusText.indexOf("В РАБОТЕ") >= 0
                                                     || root.orderStatusText.indexOf("ГОТОВИТ") >= 0))
                                statusText: (typeof root !== 'undefined' && root.hasActiveOrder) ? root.orderStatusText : ""
                                onClicked: {
                                    console.log("[SHOP] open, termId=", dashboardRoot.termId,
                                                "balance=", dashboardRoot.userBalance)
                                    if (typeof NetworkManager !== 'undefined')
                                        NetworkManager.fetchProducts()
                                    storePopup.open()
                                }
                            }
                            ActionBtn {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                compact: true
                                text: "ПОПОЛНИТЬ"
                                icon: "💳"
                                baseColor: Theme.shop
                                onClicked: depositPopup.open()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ActionBtn {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                compact: true
                                text: "ПРОДЛИТЬ"
                                icon: "⏱"
                                baseColor: accentColor
                                onClicked: {
                                    console.log("[SESSION] ПРОДЛИТЬ ВРЕМЯ — stub")
                                    if (typeof SessionAlert !== "undefined")
                                        SessionAlert.requestExtendTime()
                                }
                            }
                            ActionBtn {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                compact: true
                                text: "ПЕРЕСЕСТЬ"
                                icon: "↔"
                                baseColor: "#06b6d4"
                                onClicked: transferPopup.open()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ActionBtn {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                compact: true
                                text: "ПАУЗА"
                                icon: "⏳"
                                baseColor: "#3b82f6"
                                onClicked: {
                                    var baseUrl = dashboardRoot.apiBase()
                                    if (baseUrl.length === 0)
                                        return
                                    var pcId = parseInt(dashboardRoot.termId)
                                    if (!pcId) {
                                        console.error("[PAUSE] terminalId пуст")
                                        return
                                    }
                                    var xhr = new XMLHttpRequest()
                                    xhr.open("POST", baseUrl + "/api/shell/game/pause")
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
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.minimumWidth: 0
                                compact: true
                                text: "ВЫЙТИ"
                                icon: "🚪"
                                baseColor: "#525252"
                                onClicked: {
                                    if (typeof HidMonitor !== "undefined") HidMonitor.stopWatch()
                                    if (typeof NetworkManager !== "undefined") NetworkManager.logoutTerminal(dashboardRoot.termId)
                                    if (typeof root !== 'undefined') root.sessionUser = ""
                                    dashboardRoot.visible = false
                                }
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
                                    Rectangle {
                                        id: handleItem
                                        x: (customSlider.value / 100) * (parent.width - width)
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 16
                                        height: 16
                                        radius: 8
                                        color: "white"
                                        border.width: (volumeArea.containsMouse || volumeArea.isDragging) ? 2 : 0
                                        border.color: accentColor
                                        scale: volumeArea.isDragging ? 1.15 : (volumeArea.containsMouse ? 1.08 : 1.0)
                                        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                                        Behavior on border.width { NumberAnimation { duration: 100 } }
                                    }
                                    MouseArea {
                                        id: volumeArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
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
                                width: 44
                                height: 32
                                color: langMouse.containsMouse ? "#1a1a1a" : "#111"
                                border.color: langMouse.containsMouse ? accentColor : "#333"
                                radius: 4
                                Behavior on color { ColorAnimation { duration: 120 } }
                                Behavior on border.color { ColorAnimation { duration: 120 } }
                                Text { anchors.centerIn: parent; text: dashboardRoot.currentLanguage; color: "white"; font.pixelSize: 12; font.bold: true }
                                MouseArea { id: langMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { dashboardRoot.currentLanguage = (dashboardRoot.currentLanguage === "RU") ? "EN" : "RU"; if (typeof Launcher !== 'undefined') Launcher.toggleSystemLanguage(); } }
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

                    ActionBtn {
                        text: "ПЕРЕЗАГРУЗКА"
                        icon: "↻"
                        baseColor: "#b91c1c"
                        onClicked: rebootConfirmPopup.requestPinAndOpen()
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 25
            Text { text: "БИБЛИОТЕКА ИГР"; color: "white"; font.pixelSize: 24; font.bold: true; font.letterSpacing: 2 }
            RowLayout {
                id: filterRow
                Layout.fillWidth: true
                spacing: 30
                property string activeTab: "ВСЕ ИГРЫ"

                Row {
                    spacing: 30
                    Layout.alignment: Qt.AlignVCenter
                    Repeater {
                        model: ["ВСЕ ИГРЫ", "STEAM", "EPIC", "EA", "RIOT", "БРАУЗЕРЫ", "УТИЛИТЫ"]
                        delegate: Text {
                            text: modelData
                            color: filterRow.activeTab === modelData
                                   ? accentColor
                                   : (filterTabMouse.containsMouse ? "#cccccc" : Theme.textMuted)
                            font.pixelSize: 16
                            font.bold: true
                            font.letterSpacing: 1
                            Behavior on color { ColorAnimation { duration: 120 } }
                            MouseArea {
                                id: filterTabMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    filterRow.activeTab = modelData
                                    if (typeof gamesModel !== 'undefined')
                                        gamesModel.setFilter(modelData)
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    id: gameSearchBox
                    Layout.preferredWidth: 220
                    Layout.preferredHeight: 40
                    Layout.alignment: Qt.AlignVCenter
                    color: gameSearchInput.activeFocus ? Theme.accentSurface : Theme.bgPanel
                    border.color: gameSearchInput.activeFocus ? accentColor : "#1a1a1a"
                    border.width: gameSearchInput.activeFocus ? 2 : 1
                    radius: 4

                    TextField {
                        id: gameSearchInput
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: gameSearchClear.visible ? 28 : 10
                        placeholderText: "Поиск игр…"
                        placeholderTextColor: "#555555"
                        color: "white"
                        font.pixelSize: 13
                        selectByMouse: true
                        verticalAlignment: TextInput.AlignVCenter
                        background: Item {}
                        // Block focus/keys while overlay/login — prevents SendInput password leak
                        enabled: !(typeof root !== "undefined" && root !== null
                                   && (root.isLoggingIn || root.gameLoadingVisible))
                        readOnly: typeof root !== "undefined" && root !== null
                                  && (root.isLoggingIn || root.gameLoadingVisible)
                        onEnabledChanged: {
                            if (!enabled) {
                                text = ""
                                focus = false
                                if (typeof gamesModel !== "undefined" && gamesModel)
                                    gamesModel.setSearchQuery("")
                            }
                        }
                        onTextChanged: {
                            if (typeof gamesModel !== 'undefined')
                                gamesModel.setSearchQuery(text)
                        }
                    }

                    Text {
                        id: gameSearchClear
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"
                        color: gameSearchClearArea.containsMouse ? accentColor : Theme.textMuted
                        font.pixelSize: 12
                        visible: gameSearchInput.text.length > 0
                        MouseArea {
                            id: gameSearchClearArea
                            anchors.fill: parent
                            anchors.margins: -4
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onClicked: gameSearchInput.text = ""
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            // One continuous library: first N cards = featured (white outline + caption),
            // rest continue same size/Y via the same Row then GridView.
            ColumnLayout {
                id: gamesGridHost
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                readonly property int cardW: 230
                // Высота подобрана так, чтобы область постера была 2:3 (вертикальные
                // обложки влезают целиком без обрезки и без искажения пропорций)
                readonly property int cardH: 380
                readonly property int cardTitleH: 45
                // Реальный размер области постера — для sourceSize (экономия памяти)
                readonly property int posterW: cardW - 20
                readonly property int posterH: cardH - 20 - cardTitleH
                readonly property int featuredCaptionH: 22
                readonly property int featuredBorderPad: 6
                // Extra px so white contour AA is not clipped by the row bounds
                readonly property int featuredOutlineBleed: 2
                readonly property bool featuredStripVisible:
                    (typeof gamesModel !== 'undefined' && gamesModel
                     && gamesModel.featuredCount > 0)
                    && filterRow.activeTab === "ВСЕ ИГРЫ"
                    && gameSearchInput.text.length === 0
                readonly property int featuredCount:
                    featuredStripVisible && gamesModel ? gamesModel.featuredCount : 0
                readonly property int columnsPerRow:
                    Math.max(1, Math.floor(width / cardW))
                // First visual row fills columns: featured (0..N-1) then normal games
                readonly property int firstRowCount:
                    featuredStripVisible
                    ? Math.min(columnsPerRow,
                               (typeof gamesModel !== 'undefined' && gamesModel)
                               ? gamesModel.count : 0)
                    : 0

                function posterUrl(pUrl) {
                    if (!pUrl || pUrl === "")
                        return ""
                    if (pUrl.indexOf("http") === 0 || pUrl.indexOf("file") === 0)
                        return pUrl
                    var baseUrl = dashboardRoot.apiBase()
                    if (baseUrl.length === 0)
                        return ""
                    return pUrl.indexOf("/") === 0 ? baseUrl + pUrl : baseUrl + "/" + pUrl
                }

                // Single source: gamesModel.get() — featured already prepended in C++ from catalog
                function syncFirstRow() {
                    firstRowModel.clear()
                    if (typeof gamesModel === 'undefined' || !gamesModel)
                        return
                    var n = firstRowCount
                    for (var i = 0; i < n; ++i) {
                        var g = gamesModel.get(i)
                        if (!g || g.gameId === undefined || g.gameId === null)
                            break
                        firstRowModel.append({
                            "gameId": g.gameId,
                            "title": g.title || "",
                            "poster": g.poster || "",
                            "platform": g.platform || "",
                            "exePath": g.exePath || "",
                            "args": g.args || ""
                        })
                    }
                    if (typeof gamesModel.setGridSkip === 'function')
                        gamesModel.setGridSkip(featuredStripVisible ? firstRowModel.count : 0)
                }

                onFirstRowCountChanged: syncFirstRow()
                onFeaturedStripVisibleChanged: syncFirstRow()
                onWidthChanged: syncFirstRow()
                Component.onCompleted: syncFirstRow()

                Connections {
                    target: (typeof gamesModel !== 'undefined') ? gamesModel : null
                    function onCountChanged() { gamesGridHost.syncFirstRow() }
                    function onFeaturedCountChanged() { gamesGridHost.syncFirstRow() }
                }

                ListModel { id: firstRowModel }

                // Identical card for first-row Row and GridView (model.* only — no required props)
                Component {
                    id: gameCardDelegate
                    Item {
                        id: cardRoot
                        width: gamesGridHost.cardW
                        height: gamesGridHost.cardH

                        // Bind from model roles so title and poster stay paired
                        readonly property int cardGameId: model.gameId
                        readonly property string cardTitle: model.title || ""
                        readonly property string cardPoster: model.poster || ""
                        readonly property string cardPlatform: model.platform || ""
                        readonly property string cardExePath: model.exePath || ""
                        readonly property string cardArgs: model.args || ""

                        // Frame holds poster + title; outline lives on the title button only
                        Rectangle {
                            id: cardFrame
                            anchors.fill: parent
                            anchors.margins: 10
                            color: Theme.bgPanel
                            radius: 6
                            border.width: 0
                            // Заблокированный запуск (идёт вход / оверлей) — плитка гаснет
                            opacity: cardArea.enabled ? 1.0 : 0.45
                            Behavior on opacity { NumberAnimation { duration: 150 } }

                            Item {
                                id: cardBody
                                anchors.fill: parent
                                clip: true

                                Column {
                                    anchors.fill: parent

                                    Image {
                                        id: posterImg
                                        width: parent.width
                                        height: parent.height - gamesGridHost.cardTitleH
                                        // Unique URL per gameId keeps poster tied to title
                                        source: cardRoot.cardPoster.length
                                                ? (gamesGridHost.posterUrl(cardRoot.cardPoster)
                                                   + (cardRoot.cardPoster.indexOf("?") >= 0 ? "&" : "?")
                                                   + "gid=" + cardRoot.cardGameId)
                                                : ""
                                        // Постер вписывается целиком: без обрезки и без искажения
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: true
                                        // Декодируем под физический размер плитки: не полное
                                        // разрешение, но и не мыло на 1440p/4K
                                        sourceSize.width: Theme.px(gamesGridHost.posterW)
                                        sourceSize.height: Theme.px(gamesGridHost.posterH)
                                        opacity: (status === Image.Ready)
                                                 ? (cardArea.containsMouse ? 1.0 : 0.7) : 0.0
                                    }

                                    Rectangle {
                                        id: titleBar
                                        width: parent.width
                                        height: gamesGridHost.cardTitleH
                                        color: cardArea.containsMouse ? accentColor : "#050505"
                                        border.width: cardArea.containsMouse ? 2 : 1
                                        border.color: cardArea.containsMouse ? accentColor : "#1a1a1a"

                                        Text {
                                            id: cardTitleText
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            elide: Text.ElideRight
                                            horizontalAlignment: Text.AlignHCenter
                                            text: cardRoot.cardTitle
                                            color: cardArea.containsMouse ? "black" : "white"
                                            font.bold: true
                                            // Полное название по ховеру, если оно не влезло
                                            ToolTip.text: cardRoot.cardTitle
                                            ToolTip.delay: 400
                                            ToolTip.visible: truncated && cardArea.containsMouse
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                id: cardArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                enabled: !dashboardRoot.isLaunchBlocked()
                                onClicked: {
                                    if (dashboardRoot.isLaunchBlocked())
                                        return
                                    dashboardRoot.openAccountChoice(
                                        cardRoot.cardGameId,
                                        cardRoot.cardPlatform,
                                        cardRoot.cardTitle,
                                        cardRoot.cardExePath,
                                        cardRoot.cardArgs)
                                }
                            }
                        }
                    }
                }

                // First row: caption + white outline around featured only; one Row of identical cards
                Item {
                    id: firstLibraryRow
                    visible: gamesGridHost.featuredStripVisible
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible
                        ? (gamesGridHost.featuredCaptionH + 6
                           + gamesGridHost.cardH + gamesGridHost.featuredBorderPad * 2
                           + gamesGridHost.featuredOutlineBleed * 2)
                        : 0

                    Text {
                        id: featuredCaption
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        height: gamesGridHost.featuredCaptionH
                        text: (typeof NetworkManager !== 'undefined' && NetworkManager.featuredLabel)
                              ? NetworkManager.featuredLabel
                              : "Вы часто играете"
                        color: "white"
                        font.pixelSize: 13
                        font.bold: true
                        font.letterSpacing: 1.2
                        elide: Text.ElideRight
                    }

                    // Decorative white contour around featured cards only (not wrapping delegates)
                    Rectangle {
                        id: featuredOutline
                        anchors.top: featuredCaption.bottom
                        anchors.topMargin: 6 + gamesGridHost.featuredOutlineBleed
                        anchors.left: parent.left
                        anchors.leftMargin: gamesGridHost.featuredOutlineBleed
                        width: gamesGridHost.featuredCount * gamesGridHost.cardW
                               + gamesGridHost.featuredBorderPad * 2
                        height: gamesGridHost.cardH + gamesGridHost.featuredBorderPad * 2
                        z: 0
                        color: "transparent"
                        border.width: 1
                        border.color: "white"
                        radius: 8
                        // Don't clip — stroke must paint fully at rounded corners
                        clip: false
                    }

                    Row {
                        id: firstRowCards
                        anchors.top: featuredCaption.bottom
                        anchors.topMargin: 6 + gamesGridHost.featuredOutlineBleed
                                           + gamesGridHost.featuredBorderPad
                        anchors.left: parent.left
                        anchors.leftMargin: gamesGridHost.featuredOutlineBleed
                                            + gamesGridHost.featuredBorderPad
                        height: gamesGridHost.cardH
                        spacing: 0
                        z: 1

                        Repeater {
                            model: firstRowModel
                            delegate: gameCardDelegate
                        }
                    }
                }

                GridView {
                    id: gamesGrid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    cellWidth: gamesGridHost.cardW
                    cellHeight: gamesGridHost.cardH
                    clip: true
                    model: gamesModel
                    boundsBehavior: Flickable.StopAtBounds
                    reuseItems: true
                    visible: !gamesEmptyState.visible
                    delegate: gameCardDelegate
                }

                // Пустое состояние библиотеки: разделяем «поиск/фильтр без результата» и «ничего не загрузилось»
                Item {
                    id: gamesEmptyState
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: (typeof gamesModel !== 'undefined' && gamesModel)
                             ? gamesModel.count === 0 : false

                    readonly property bool isFiltered:
                        gameSearchInput.text.length > 0 || filterRow.activeTab !== "ВСЕ ИГРЫ"

                    Column {
                        anchors.centerIn: parent
                        spacing: 10

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: gamesEmptyState.isFiltered ? "🔍" : "🎮"
                            font.pixelSize: 44
                            opacity: 0.35
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: gamesEmptyState.isFiltered ? "Игры не найдены" : "Библиотека пуста"
                            color: "#8a8a8a"
                            font.pixelSize: 20
                            font.bold: true
                            font.letterSpacing: 1
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: gamesEmptyState.isFiltered
                                  ? (gameSearchInput.text.length > 0
                                     ? "Измените запрос"
                                     : "Выберите другую категорию")
                                  : "Проверьте соединение"
                            color: "#555555"
                            font.pixelSize: 14
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
            border.color: Theme.shop
            border.width: 2
            radius: Theme.radiusSm
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
                        color: Theme.shop
                        font.pixelSize: 36
                        font.bold: true
                        font.italic: true
                    }
                    Text {
                        text: "СНАРЯЖЕНИЕ И ПРОВИЗИЯ ДЛЯ РЕЙДА"
                        color: Theme.shop
                        opacity: 0.5
                        font.pixelSize: Theme.fontCaption
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
                        color: filterCatRow.activeCat === modelData.name ? Theme.shop : "#111"

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
                        color: Theme.bgPanel
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
                                    // Декодируем под видимую область картинки товара, а не в полном разрешении
                                    sourceSize.width: Theme.px(storeGrid.cellWidth - 55)
                                    sourceSize.height: Theme.px(130)
                                    source: {
                                        var imgUrl = model.image || ""
                                        if (imgUrl === "")
                                            return ""
                                        if (imgUrl.indexOf("http") === 0 || imgUrl.indexOf("file") === 0)
                                            return imgUrl
                                        var baseUrl = dashboardRoot.apiBase()
                                        if (baseUrl.length === 0)
                                            return ""
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
                                    color: Theme.shop
                                    font.pixelSize: 22
                                    font.bold: true
                                }

                                Item { Layout.fillWidth: true }

                                Rectangle {
                                    visible: model.stock > 0
                                    width: 44
                                    height: 44
                                    radius: 8
                                    color: itemMouse.containsMouse ? "#ffffff" : Theme.shop

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

                    // Пустая витрина. Элемент лежит в contentItem, поэтому центрируем
                    // вручную по видимой области (скроллить всё равно нечего).
                    Column {
                        visible: storeGrid.count === 0
                        x: (storeGrid.width - width) / 2
                        y: (storeGrid.height - height) / 2
                        spacing: 8

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "📦"
                            font.pixelSize: 40
                            opacity: 0.35
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Товары не найдены"
                            color: "#8a8a8a"
                            font.pixelSize: 18
                            font.bold: true
                        }
                    }
                }

                Rectangle {
                    id: cartBoxContainer
                    Layout.preferredWidth: 400
                    Layout.fillHeight: true
                    color: Theme.bgPanel
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
                                            color: Theme.shop
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
                            color: cartModel.count > 0 ? Theme.shop : "#222"

                            Text {
                                anchors.centerIn: parent
                                text: "ОФОРМИТЬ ЗАКАЗ"
                                color: "black"
                                font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: cartModel.count > 0
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    console.log("[SHOP] checkout click, cart=", cartModel.count,
                                                "termId=", dashboardRoot.termId,
                                                "balance=", dashboardRoot.userBalance)

                                    var baseUrl = dashboardRoot.apiBase()
                                    if (baseUrl.length === 0)
                                        return

                                    // Snapshot cart before async clears
                                    var lines = []
                                    for (var i = 0; i < cartModel.count; i++) {
                                        var item = cartModel.get(i)
                                        lines.push({
                                            productId: item.productId,
                                            name: item.name,
                                            price: item.price,
                                            quantity: item.quantity
                                        })
                                    }
                                    if (lines.length === 0) {
                                        console.warn("[SHOP] checkout aborted: empty cart snapshot")
                                        return
                                    }

                                    var pending = 0
                                    var failed = 0
                                    var lastError = ""
                                    var lastBalance = dashboardRoot.userBalance
                                    var lastOrderId = 0
                                    var lastStatusLabel = ""
                                    var lastFiscalReceipt = null

                                    function finishCheckout() {
                                        console.log("[SHOP] checkout done remaining=", pending, "failed=", failed,
                                                    "balance=", lastBalance, "order_id=", lastOrderId,
                                                    "status=", lastStatusLabel, "err=", lastError)
                                        if (failed === 0) {
                                            cartModel.clear()
                                            storePopup.close()
                                            if (typeof root !== 'undefined' && root !== null) {
                                                root.sessionBalance = lastBalance
                                                if (lastOrderId > 0) {
                                                    root.trackedOrderId = lastOrderId
                                                    root.hasActiveOrder = true
                                                    root.orderStatusCode = "pending"
                                                    root.orderStatusText = lastStatusLabel.length > 0
                                                            ? String(lastStatusLabel).toUpperCase()
                                                            : "ЗАКАЗ ПРИНЯТ"
                                                    // Seed contents panel from cart snapshot until poll arrives
                                                    var seeded = []
                                                    var seededTotal = 0
                                                    for (var si = 0; si < lines.length; si++) {
                                                        var sl = lines[si]
                                                        seeded.push({
                                                            name: sl.name,
                                                            qty: sl.quantity,
                                                            price: sl.price * sl.quantity
                                                        })
                                                        seededTotal += sl.price * sl.quantity
                                                    }
                                                    root.orderItems = seeded
                                                    root.orderItemsTotal = seededTotal
                                                } else {
                                                    root.hasActiveOrder = true
                                                    root.orderStatusCode = "pending"
                                                    root.orderStatusText = "ЗАКАЗ ПРИНЯТ"
                                                }
                                                if (typeof NetworkManager !== 'undefined')
                                                    NetworkManager.checkOrderStatus(
                                                                parseInt(dashboardRoot.termId) || root.terminalId,
                                                                root.trackedOrderId)
                                            }
                                            if (lastFiscalReceipt
                                                    && lastFiscalReceipt.fiscal_receipt_url
                                                    && String(lastFiscalReceipt.fiscal_receipt_url).length > 0) {
                                                fiscalReceiptPopup.receiptUrl = String(lastFiscalReceipt.fiscal_receipt_url)
                                                fiscalReceiptPopup.receiptAmount = Number(lastFiscalReceipt.amount || 0)
                                                fiscalReceiptPopup.isStub = !!lastFiscalReceipt.is_stub
                                                fiscalReceiptPopup.description = String(lastFiscalReceipt.description || "")
                                                Qt.callLater(function () { fiscalReceiptPopup.open() })
                                            }
                                        } else {
                                            console.error("[SHOP] checkout failed:", lastError)
                                            storeToast.show("Не удалось оформить заказ"
                                                            + (String(lastError).length > 0
                                                               ? ": " + lastError : ""))
                                        }
                                    }

                                    // One checkout = one order with all cart lines
                                    var apiItems = []
                                    for (var li = 0; li < lines.length; li++) {
                                        apiItems.push({
                                            product_id: lines[li].productId,
                                            qty: lines[li].quantity
                                        })
                                    }

                                    pending = 1
                                    var xhr = new XMLHttpRequest()
                                    var url = baseUrl + "/api/shell/store/checkout"
                                    var body = JSON.stringify({
                                        "terminal_id": parseInt(dashboardRoot.termId) || 0,
                                        "items": apiItems
                                    })
                                    console.log("[SHOP] POST", url, body, "lines=", apiItems.length)
                                    xhr.open("POST", url)
                                    xhr.setRequestHeader("Content-Type", "application/json")
                                    xhr.setRequestHeader("Accept", "application/json")
                                    xhr.onreadystatechange = function() {
                                        if (xhr.readyState !== XMLHttpRequest.DONE)
                                            return
                                        console.log("[SHOP] response HTTP", xhr.status, xhr.responseText)
                                        try {
                                            var res = xhr.responseText ? JSON.parse(xhr.responseText) : {}
                                            if (xhr.status >= 200 && xhr.status < 300
                                                    && res.status === "success") {
                                                if (typeof res.balance === "number")
                                                    lastBalance = res.balance
                                                else if (typeof res.deposit_balance === "number")
                                                    lastBalance = res.deposit_balance
                                                if (typeof res.order_id === "number" && res.order_id > 0)
                                                    lastOrderId = res.order_id
                                                else if (res.order_id)
                                                    lastOrderId = parseInt(res.order_id) || lastOrderId
                                                if (res.status_label)
                                                    lastStatusLabel = res.status_label
                                                if (res.fiscal_receipt
                                                        && res.fiscal_receipt.fiscal_receipt_url)
                                                    lastFiscalReceipt = res.fiscal_receipt
                                            } else {
                                                failed++
                                                lastError = (res && res.message)
                                                             ? res.message
                                                             : ("HTTP " + xhr.status)
                                                console.error("[SHOP] error:", lastError)
                                            }
                                        } catch (e) {
                                            failed++
                                            lastError = e.toString()
                                            console.error("[SHOP] parse error:", e)
                                        }
                                        pending = 0
                                        finishCheckout()
                                    }
                                    xhr.onerror = function() {
                                        failed++
                                        lastError = "нет связи с сервером"
                                        console.error("[SHOP] network error")
                                        pending = 0
                                        finishCheckout()
                                    }
                                    xhr.send(body)
                                }
                            }
                        }
                    }
                }
            }
        }

        // Тост ошибок магазина (внутри попапа — иначе его перекроет модальный слой)
        Rectangle {
            id: storeToast
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            width: Math.min(parent.width - 40, storeToastText.implicitWidth + 40)
            height: 44
            radius: 8
            z: 1000
            visible: opacity > 0
            opacity: 0
            color: "#0a0505"
            border.color: Theme.dangerStrong
            border.width: 1

            property string message: ""

            function show(msg) {
                storeToast.message = msg
                storeToast.opacity = 1
                storeToastHide.restart()
            }

            Behavior on opacity { NumberAnimation { duration: 220 } }

            Text {
                id: storeToastText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, storePopup.width - 80)
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                text: storeToast.message
                color: "#f5f5f5"
                font.pixelSize: 14
                font.bold: true
            }

            Timer {
                id: storeToastHide
                interval: 3000
                onTriggered: storeToast.opacity = 0
            }
        }
    }

    Popup {
        id: fiscalReceiptPopup
        width: Math.min(440, parent.width * 0.9)
        height: Math.min(620, parent.height * 0.92)
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 0
        z: 9500

        property string receiptUrl: ""
        property real receiptAmount: 0
        property bool isStub: false
        property string description: ""

        readonly property string qrImageUrl: receiptUrl.length > 0
            ? ("https://api.qrserver.com/v1/create-qr-code/?size=280x280&data="
               + encodeURIComponent(receiptUrl))
            : ""

        readonly property string amountText: {
            const n = Number(receiptAmount || 0)
            if (!isFinite(n) || Math.abs(n) < 0.0001)
                return ""
            const abs = Math.round(Math.abs(n))
            return (n < 0 ? "−" : (n > 0 ? "+" : "")) + abs + " ₽"
        }

        background: Rectangle {
            color: Theme.bgPanel
            radius: Theme.radiusSm
            border.width: 1
            border.color: Qt.rgba(Theme.shop.r, Theme.shop.g, Theme.shop.b, 0.4)
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 14

            Text {
                Layout.fillWidth: true
                text: "КАССОВЫЙ ЧЕК"
                color: Theme.shop
                font.pixelSize: 22
                font.bold: true
                font.italic: true
                font.letterSpacing: 1.2
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                visible: fiscalReceiptPopup.amountText.length > 0
                Layout.fillWidth: true
                text: fiscalReceiptPopup.amountText
                color: "#ffffff"
                font.pixelSize: 34
                font.bold: true
                font.italic: true
                font.family: "Consolas"
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                visible: fiscalReceiptPopup.description.length > 0
                Layout.fillWidth: true
                text: fiscalReceiptPopup.description
                color: Theme.textSecondary
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                visible: fiscalReceiptPopup.isStub
                Layout.fillWidth: true
                text: "Демо · касса на сервере выключена"
                color: "#fbbf24"
                font.pixelSize: 11
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 240
                Layout.alignment: Qt.AlignHCenter

                Rectangle {
                    anchors.centerIn: parent
                    width: 220
                    height: 220
                    radius: 16
                    color: "#ffffff"

                    Image {
                        anchors.fill: parent
                        anchors.margins: 12
                        source: fiscalReceiptPopup.qrImageUrl
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: paperHint.implicitHeight + 20
                radius: 12
                color: Qt.rgba(1, 1, 1, 0.04)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.08)

                Text {
                    id: paperHint
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 12
                    text: "Если нужен бумажный чек — обратитесь к администратору"
                    color: Theme.textMuted
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            Item { Layout.fillHeight: true }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                radius: Theme.radiusSm
                color: closeReceiptTap.pressed ? Qt.darker(Theme.shop, 1.2)
                     : (closeReceiptHover.hovered ? Theme.warning : Theme.shop)

                HoverHandler {
                    id: closeReceiptHover
                    cursorShape: Qt.PointingHandCursor
                }

                Text {
                    anchors.centerIn: parent
                    text: "ЗАКРЫТЬ"
                    color: "#0a0a0a"
                    font.pixelSize: 15
                    font.bold: true
                    font.italic: true
                    font.letterSpacing: 1.4
                }

                TapHandler {
                    id: closeReceiptTap
                    onTapped: closeFiscalReceiptPopup()
                }
            }
        }
    }

    Popup {
        id: transferPopup
        width: Math.min(480, parent.width * 0.9)
        height: Math.min(560, parent.height * 0.88)
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0
        property var targets: []
        property int selectedId: 0
        property string warning: ""
        property string errorText: ""
        property real charge: 0
        property bool busy: false
        property bool loading: false
        property string donePin: ""
        property string donePcName: ""

        onOpened: {
            transferPopup.donePin = ""
            transferPopup.donePcName = ""
            loadTransferTargets()
        }

        function finishAndLeave() {
            transferPopup.close()
            if (typeof HidMonitor !== "undefined") HidMonitor.stopWatch()
            if (typeof NetworkManager !== "undefined") {
                NetworkManager.stopClimateControl()
                NetworkManager.setFan("auto")
                NetworkManager.clearSessionUser()
            }
            if (typeof root !== "undefined") root.sessionUser = ""
            dashboardRoot.visible = false
        }

        function loadTransferTargets() {
            transferPopup.loading = true
            transferPopup.errorText = ""
            transferPopup.warning = ""
            transferPopup.selectedId = 0
            transferPopup.targets = []
            var baseUrl = dashboardRoot.apiBase()
            var pcId = parseInt(dashboardRoot.termId)
            if (!baseUrl.length || !pcId) {
                transferPopup.loading = false
                transferPopup.errorText = "Нет terminal_id"
                return
            }
            var xhr = new XMLHttpRequest()
            xhr.open("GET", baseUrl + "/api/shell/transfer/targets?terminal_id=" + pcId)
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE)
                    return
                transferPopup.loading = false
                if (xhr.status !== 200) {
                    transferPopup.errorText = "Не удалось загрузить ПК"
                    return
                }
                try {
                    var res = JSON.parse(xhr.responseText)
                    transferPopup.targets = res.targets || []
                    if (!transferPopup.targets.length)
                        transferPopup.errorText = "Нет свободных ПК"
                } catch (e) {
                    transferPopup.errorText = "Ошибка ответа"
                }
            }
            xhr.send()
        }

        function previewTarget(id) {
            if (transferPopup.donePin.length)
                return
            transferPopup.selectedId = id
            transferPopup.busy = true
            transferPopup.errorText = ""
            var baseUrl = dashboardRoot.apiBase()
            var pcId = parseInt(dashboardRoot.termId)
            var xhr = new XMLHttpRequest()
            xhr.open("POST", baseUrl + "/api/shell/transfer/preview")
            xhr.setRequestHeader("Content-Type", "application/json")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE)
                    return
                transferPopup.busy = false
                if (xhr.status !== 200) {
                    try {
                        var err = JSON.parse(xhr.responseText)
                        transferPopup.errorText = err.message || "Ошибка расчёта"
                    } catch (e) {
                        transferPopup.errorText = "Ошибка расчёта"
                    }
                    transferPopup.warning = ""
                    return
                }
                try {
                    var res = JSON.parse(xhr.responseText)
                    transferPopup.warning = (res.preview && res.preview.warning) ? res.preview.warning : ""
                    transferPopup.charge = (res.preview && res.preview.charge) ? Number(res.preview.charge) : 0
                } catch (e) {
                    transferPopup.errorText = "Ошибка ответа"
                }
            }
            xhr.send(JSON.stringify({
                "terminal_id": pcId,
                "target_computer_id": id
            }))
        }

        function confirmTransfer() {
            if (!transferPopup.selectedId || transferPopup.busy || transferPopup.donePin.length)
                return
            transferPopup.busy = true
            transferPopup.errorText = ""
            var baseUrl = dashboardRoot.apiBase()
            var pcId = parseInt(dashboardRoot.termId)
            var xhr = new XMLHttpRequest()
            xhr.open("POST", baseUrl + "/api/shell/transfer/confirm")
            xhr.setRequestHeader("Content-Type", "application/json")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE)
                    return
                transferPopup.busy = false
                if (xhr.status !== 200) {
                    try {
                        var err = JSON.parse(xhr.responseText)
                        transferPopup.errorText = err.message || "Не удалось пересесть"
                    } catch (e) {
                        transferPopup.errorText = "Не удалось пересесть"
                    }
                    return
                }
                try {
                    var res = JSON.parse(xhr.responseText)
                    transferPopup.donePin = String(res.pin_code || (res.result && res.result.pin_code) || "")
                    transferPopup.donePcName = String((res.to && res.to.name) || (res.result && res.result.to && res.result.to.name) || "")
                    transferPopup.warning = ""
                } catch (e) {
                    transferPopup.errorText = "Ошибка ответа"
                }
            }
            xhr.send(JSON.stringify({
                "terminal_id": pcId,
                "target_computer_id": transferPopup.selectedId
            }))
        }

        background: Rectangle {
            color: Theme.bgPanel
            radius: Theme.radiusSm
            border.width: 1
            border.color: "#0e7490"
        }

        contentItem: Column {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12

            Text {
                text: transferPopup.donePin.length ? "ГОТОВО" : "ПЕРЕСАДКА"
                color: "#22d3ee"
                font.pixelSize: 22
                font.bold: true
                font.italic: true
            }

            // Success: new PIN
            Column {
                width: parent.width
                spacing: 10
                visible: transferPopup.donePin.length > 0
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "Войдите PIN на " + (transferPopup.donePcName.length ? transferPopup.donePcName : "новом ПК")
                    color: "#888"
                    font.pixelSize: 12
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: transferPopup.donePin
                    color: "#22c55e"
                    font.pixelSize: 48
                    font.bold: true
                    font.family: "Consolas"
                }
                Rectangle {
                    width: parent.width
                    height: 48
                    radius: 8
                    color: "#06b6d4"
                    Text { anchors.centerIn: parent; text: "ПОНЯТНО"; color: "#0a0a0a"; font.bold: true; font.pixelSize: 14 }
                    MouseArea { anchors.fill: parent; onClicked: transferPopup.finishAndLeave() }
                }
            }

            Text {
                width: parent.width
                visible: transferPopup.donePin.length === 0
                wrapMode: Text.WordWrap
                text: transferPopup.loading ? "Загрузка…" : (transferPopup.errorText.length ? transferPopup.errorText : "Выберите свободный ПК")
                color: transferPopup.errorText.length ? "#f87171" : "#888"
                font.pixelSize: 12
            }

            ListView {
                width: parent.width
                height: 220
                clip: true
                visible: transferPopup.donePin.length === 0
                model: transferPopup.targets
                spacing: 6
                delegate: Rectangle {
                    width: ListView.view.width
                    height: 52
                    radius: 8
                    color: transferPopup.selectedId === modelData.id ? "#083344" : "#111"
                    border.color: transferPopup.selectedId === modelData.id ? "#22d3ee" : "#333"
                    border.width: 1
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        Text {
                            text: modelData.name
                            color: "white"
                            font.bold: true
                            font.pixelSize: 14
                        }
                        Text {
                            text: (modelData.zone || "зона —") + " · " + Math.round(modelData.hourly_rate) + " ₽/ч"
                            color: "#888"
                            font.pixelSize: 11
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: transferPopup.previewTarget(modelData.id)
                    }
                }
            }

            Text {
                width: parent.width
                visible: transferPopup.donePin.length === 0 && transferPopup.warning.length > 0
                wrapMode: Text.WordWrap
                text: transferPopup.warning
                color: "#fbbf24"
                font.pixelSize: 13
            }
            Text {
                visible: transferPopup.donePin.length === 0 && transferPopup.warning.length > 0
                text: "Доплата: " + Math.round(transferPopup.charge) + " ₽"
                color: "#94a3b8"
                font.pixelSize: 11
            }

            Row {
                spacing: 10
                width: parent.width
                visible: transferPopup.donePin.length === 0
                Rectangle {
                    width: parent.width * 0.35 - 5
                    height: 44
                    radius: 8
                    color: "#222"
                    Text { anchors.centerIn: parent; text: "ОТМЕНА"; color: "#aaa"; font.bold: true; font.pixelSize: 12 }
                    MouseArea { anchors.fill: parent; onClicked: transferPopup.close() }
                }
                Rectangle {
                    width: parent.width * 0.65 - 5
                    height: 44
                    radius: 8
                    color: (transferPopup.selectedId && !transferPopup.busy) ? "#06b6d4" : "#333"
                    Text {
                        anchors.centerIn: parent
                        text: transferPopup.busy ? "…" : "ПОДТВЕРДИТЬ"
                        color: (transferPopup.selectedId && !transferPopup.busy) ? "#0a0a0a" : "#666"
                        font.bold: true
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: transferPopup.selectedId > 0 && !transferPopup.busy
                        onClicked: transferPopup.confirmTransfer()
                    }
                }
            }
        }
    }


    Popup {
        id: depositPopup
        width: Math.min(560, parent.width * 0.9)
        height: Math.min(620, parent.height * 0.9)
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 0

        background: Rectangle {
            color: Theme.bgPanel
            radius: Theme.radiusSm
            border.width: 1
            border.color: Qt.rgba(Theme.shop.r, Theme.shop.g, Theme.shop.b, 0.35)

            // Акцентный блик по верхней кромке — как на карточках магазина.
            Rectangle {
                anchors.top: parent.top
                anchors.topMargin: 1
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width * 0.55
                height: 2
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 0.5; color: Theme.shop }
                    GradientStop { position: 1.0; color: "transparent" }
                }
            }
        }

        property int selectedAmount: 500
        property bool waitingPayment: false
        property bool creating: false
        property bool sendReceipt: false
        property string statusText: ""
        property string lastError: ""
        property string widgetUrl: ""
        property string paymentId: ""

        onOpened: {
            depositPopup.waitingPayment = false
            depositPopup.creating = false
            depositPopup.sendReceipt = false
            depositPopup.statusText = ""
            depositPopup.lastError = ""
            depositPopup.widgetUrl = ""
            depositPopup.paymentId = ""
        }

        onClosed: {
            depositWaitTimer.stop()
            if (!payWindow.visible) {
                depositPopup.waitingPayment = false
                depositPopup.widgetUrl = ""
                if (typeof NetworkManager !== "undefined")
                    NetworkManager.refreshBalance()
            }
        }

        Connections {
            target: typeof NetworkManager !== "undefined" ? NetworkManager : null
            function onTopUpReady(widgetUrl, paymentId, amount) {
                depositPopup.creating = false
                depositCreateTimeout.stop()
                depositPopup.waitingPayment = true
                depositPopup.widgetUrl = widgetUrl
                depositPopup.paymentId = paymentId
                depositPopup.statusText = "Открываем форму оплаты…"
                depositPopup.lastError = ""
                payWindow.amount = amount > 0 ? amount : depositPopup.selectedAmount
                payWindow.openWithUrl(widgetUrl)
                if (typeof NetworkManager !== "undefined")
                    NetworkManager.refreshBalance()
                // Дальше всё происходит в окне оплаты — попап только мешает,
                // висит позади и дублирует статус.
                depositPopup.close()
            }
            function onTopUpFailed(message) {
                depositPopup.creating = false
                depositCreateTimeout.stop()
                depositPopup.waitingPayment = false
                depositPopup.widgetUrl = ""
                depositPopup.statusText = ""
                depositPopup.lastError = message || "Не удалось создать платёж"
                depositWaitTimer.stop()
                payWindow.closePayment()
            }
            function onBalanceUpdated(balance) {
                if (!depositPopup.waitingPayment)
                    return
                depositPopup.statusText = "Баланс обновлён: " + Number(balance).toFixed(0) + " ₽"
                payWindow.statusBanner = depositPopup.statusText
                depositWaitTimer.stop()
                depositWaitClose.start()
            }
        }

        // Если сервер не ответил, кнопка иначе осталась бы навсегда заблокированной.
        Timer {
            id: depositCreateTimeout
            interval: 25000
            onTriggered: {
                if (!depositPopup.creating)
                    return
                depositPopup.creating = false
                depositPopup.statusText = ""
                depositPopup.lastError = "Сервер не ответил. Попробуйте ещё раз."
            }
        }

        Timer {
            id: depositWaitTimer
            interval: 4000
            repeat: true
            onTriggered: {
                if (typeof NetworkManager !== "undefined")
                    NetworkManager.refreshBalance()
            }
        }

        Timer {
            id: depositWaitClose
            interval: 1600
            onTriggered: {
                payWindow.closePayment()
                depositPopup.close()
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 26
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    spacing: 5
                    Layout.fillWidth: true

                    Text {
                        text: "ЮKASSA · ТЕСТОВЫЙ РЕЖИМ · ТОЛЬКО КАРТА"
                        color: Theme.textMuted
                        font.pixelSize: 9
                        font.bold: true
                        font.letterSpacing: 1.8
                    }
                    Text {
                        text: "ПОПОЛНЕНИЕ БАЛАНСА"
                        color: Theme.shop
                        font.pixelSize: 21
                        font.bold: true
                        font.italic: true
                        font.letterSpacing: 1.5
                    }
                }

                // Место под крестик: сам он живёт в углу попапа, вне колонок.
                Item {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                radius: Theme.radiusSm
                color: "#101010"
                border.width: 1
                border.color: "#1d1d1d"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18

                    Text {
                        text: "ТЕКУЩИЙ БАЛАНС"
                        color: Theme.textMuted
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 1.4
                        Layout.fillWidth: true
                    }
                    Text {
                        id: depositBalanceValue
                        text: Number(dashboardRoot.userBalance).toFixed(0) + " ₽"
                        color: Theme.textPrimary
                        font.pixelSize: 22
                        font.bold: true
                        font.italic: true
                        transformOrigin: Item.Right
                    }
                }

                Connections {
                    target: dashboardRoot
                    function onBalanceIncreased(gain) {
                        depositBalanceFlash.restart()
                    }
                }

                SequentialAnimation {
                    id: depositBalanceFlash

                    ParallelAnimation {
                        NumberAnimation {
                            target: depositBalanceValue; property: "scale"
                            to: 1.18; duration: 320; easing.type: Easing.OutBack
                        }
                        ColorAnimation {
                            target: depositBalanceValue; property: "color"
                            to: Theme.success; duration: 320
                        }
                    }

                    SequentialAnimation {
                        loops: 2
                        ColorAnimation {
                            target: depositBalanceValue; property: "color"
                            to: Theme.textPrimary; duration: 260
                        }
                        ColorAnimation {
                            target: depositBalanceValue; property: "color"
                            to: Theme.success; duration: 260
                        }
                    }

                    ParallelAnimation {
                        NumberAnimation {
                            target: depositBalanceValue; property: "scale"
                            to: 1.0; duration: 520; easing.type: Easing.OutCubic
                        }
                        ColorAnimation {
                            target: depositBalanceValue; property: "color"
                            to: Theme.textPrimary; duration: 640
                        }
                    }
                }
            }

            ColumnLayout {
                visible: !depositPopup.waitingPayment
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                Text {
                    text: "СУММА ПОПОЛНЕНИЯ"
                    color: Theme.textMuted
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.4
                }

                RowLayout {
                    spacing: 10
                    Layout.fillWidth: true

                    Repeater {
                        model: [100, 300, 500, 1000]

                        delegate: Rectangle {
                            id: amountTile

                            readonly property bool selected: depositPopup.selectedAmount === modelData

                            Layout.fillWidth: true
                            Layout.preferredHeight: 54
                            radius: Theme.radiusSm
                            color: selected ? Theme.shop
                                            : (tileHover.hovered ? "#181818" : "#101010")
                            border.width: 1
                            border.color: selected ? Theme.shop
                                                   : (tileHover.hovered ? "#3a3a3a" : "#1d1d1d")

                            Behavior on color { ColorAnimation { duration: 120 } }
                            Behavior on border.color { ColorAnimation { duration: 120 } }

                            HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }

                            Text {
                                anchors.centerIn: parent
                                text: modelData + " ₽"
                                color: amountTile.selected ? "#0a0a0a" : Theme.textBody
                                font.pixelSize: 15
                                font.bold: true
                                font.italic: amountTile.selected
                            }

                            TapHandler {
                                enabled: !depositPopup.creating
                                onTapped: depositPopup.selectedAmount = modelData
                            }
                        }
                    }
                }

                Rectangle {
                    visible: depositPopup.lastError.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: errorText.implicitHeight + 22
                    radius: Theme.radiusSm
                    color: "#1a0c0c"
                    border.width: 1
                    border.color: "#4d1f1f"

                    Text {
                        id: errorText
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: depositPopup.lastError
                        color: "#fca5a5"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }

                // Кнопка прижата к низу карточки.
                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        width: 22
                        height: 22
                        radius: 4
                        color: depositPopup.sendReceipt ? Theme.shop : "transparent"
                        border.width: 1
                        border.color: depositPopup.sendReceipt ? Theme.shop : "#333"
                        Text {
                            anchors.centerIn: parent
                            text: "✓"
                            color: "#0a0a0a"
                            font.bold: true
                            font.pixelSize: 14
                            visible: depositPopup.sendReceipt
                        }
                        TapHandler {
                            enabled: !depositPopup.creating
                            onTapped: depositPopup.sendReceipt = !depositPopup.sendReceipt
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Отправить чек на Email/SMS"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        TapHandler {
                            enabled: !depositPopup.creating
                            onTapped: depositPopup.sendReceipt = !depositPopup.sendReceipt
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Нажимая «Оплатить», вы соглашаетесь получить чек в виде QR-кода на экране"
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    id: payButton

                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    radius: Theme.radiusSm
                    color: depositPopup.creating
                           ? "#3d3208"
                           : (payTap.pressed ? Qt.darker(Theme.shop, 1.25)
                                             : (payHover.hovered ? Theme.warning : Theme.shop))

                    Behavior on color { ColorAnimation { duration: 120 } }

                    HoverHandler {
                        id: payHover
                        enabled: !depositPopup.creating
                        cursorShape: Qt.PointingHandCursor
                    }

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 10

                        BusyIndicator {
                            visible: depositPopup.creating
                            running: visible
                            implicitWidth: 20
                            implicitHeight: 20
                        }

                        Text {
                            text: depositPopup.creating
                                  ? "СОЗДАЁМ ПЛАТЁЖ…"
                                  : "ОПЛАТИТЬ КАРТОЙ · " + depositPopup.selectedAmount + " ₽"
                            color: depositPopup.creating ? Theme.shop : "#0a0a0a"
                            font.pixelSize: 15
                            font.bold: true
                            font.italic: true
                            font.letterSpacing: 1.2
                        }
                    }

                    TapHandler {
                        id: payTap
                        enabled: !depositPopup.creating
                        onTapped: {
                            depositPopup.lastError = ""
                            depositPopup.statusText = "Создаём платёж…"
                            if (typeof NetworkManager !== "undefined") {
                                depositPopup.creating = true
                                depositCreateTimeout.restart()
                                NetworkManager.createTopUp(depositPopup.selectedAmount, depositPopup.sendReceipt)
                            } else {
                                depositPopup.lastError = "NetworkManager недоступен"
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Данные карты вводятся в защищённой форме ЮKassa"
                    color: Theme.textMuted
                    font.pixelSize: 10
                    font.letterSpacing: 0.6
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            ColumnLayout {
                visible: depositPopup.waitingPayment
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                Text {
                    text: depositPopup.statusText.length > 0
                          ? depositPopup.statusText
                          : "Форма карты открыта в окне оплаты. Не закрывайте его, пока не завершите платёж."
                    color: Theme.textBody
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    font.pixelSize: 13
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    radius: Theme.radiusSm
                    color: showPayHover.hovered ? "#181818" : "#101010"
                    border.width: 1
                    border.color: showPayHover.hovered ? "#3a3a3a" : "#1d1d1d"

                    HoverHandler { id: showPayHover; cursorShape: Qt.PointingHandCursor }

                    Text {
                        anchors.centerIn: parent
                        text: "ПОКАЗАТЬ ОКНО ОПЛАТЫ"
                        color: Theme.textBody
                        font.pixelSize: 13
                        font.bold: true
                        font.letterSpacing: 1
                    }

                    TapHandler {
                        onTapped: {
                            if (depositPopup.widgetUrl.length > 0)
                                payWindow.openWithUrl(depositPopup.widgetUrl)
                        }
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Отменить"
                    color: cancelHover.hovered ? Theme.textBody : Theme.textMuted
                    font.pixelSize: 12
                    font.bold: true

                    HoverHandler { id: cancelHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            payWindow.closePayment()
                            depositPopup.close()
                        }
                    }
                }
            }
        }

        // Крестик в самом углу карточки, поверх содержимого.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            width: 34
            height: 34
            z: 10
            radius: Theme.radiusSm
            color: depositCloseHover.hovered ? "#171717" : "transparent"
            border.width: 1
            border.color: depositCloseHover.hovered ? "#4d4d4d" : "#232323"

            HoverHandler { id: depositCloseHover; cursorShape: Qt.PointingHandCursor }

            Text {
                anchors.centerIn: parent
                text: "✕"
                color: depositCloseHover.hovered ? "#ffffff" : "#6b6b6b"
                font.pixelSize: 14
                font.bold: true
            }

            TapHandler { onTapped: depositPopup.close() }
        }
    }

    // Затемнение дашборда, пока открыто окно оплаты. Popup гасит фон своим
    // модальным слоем, а payWindow — отдельное нативное окно, и без этого
    // слоя дашборд остаётся ярким прямо под формой оплаты.
    Rectangle {
        anchors.fill: parent
        z: 9000
        color: "#000000"
        opacity: payWindow.visible ? 0.72 : 0
        visible: opacity > 0.01

        Behavior on opacity { NumberAnimation { duration: 180 } }

        // Гасим клики по дашборду, пока идёт оплата.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: function(wheel) { wheel.accepted = true }
        }
    }

    // Отдельное нативное окно: WebView2 внутри QML Popup даёт «дыру»
    // (сквозь неё видно рабочий стол / Cursor), потому что HWND не вписывается в оверлей.
    Window {
        id: payWindow
        title: "Оплата · ЮKassa"
        width: 560
        height: 780
        visible: false
        color: Theme.bgPanel
        // Без системного заголовка: шапку с суммой и крестиком рисуем сами,
        // рамку — тонкой зелёной линией.
        flags: Qt.Dialog | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint

        property real amount: 0
        property string pendingUrl: ""
        property string statusBanner: ""
        property string loadError: ""
        property bool paymentCompleted: false

        property string widgetPageUrl: ""
        property string widgetOrigin: ""

        function originOf(u) {
            var s = (u || "").toString()
            var i = s.indexOf("://")
            if (i < 0)
                return ""
            var j = s.indexOf("/", i + 3)
            return j < 0 ? s : s.substring(0, j)
        }

        // Виджет ЮKassa после оплаты уводит окно на return_url, а тот редиректит
        // дальше по сайту. WebView сообщает только конечный адрес (например
        // /login), поэтому ловим любой уход со страницы виджета на наш сервер.
        // Домены ЮKassa и банков (3-D Secure) под это не подпадают.
        function isPostPaymentUrl(u) {
            var s = (u || "").toString()
            if (widgetOrigin.length === 0 || s.length === 0)
                return false
            if (s.indexOf(widgetOrigin) !== 0)
                return false
            return s.indexOf(widgetPageUrl) !== 0
        }

        // Ручное закрытие (Escape, кнопка на заставке, крестик страницы):
        // синхронизируем платёж на всякий случай и закрываем окно.
        function userClose() {
            if (!paymentCompleted && depositPopup.paymentId.length > 0
                    && typeof NetworkManager !== "undefined") {
                NetworkManager.syncTopUpPayment(depositPopup.paymentId)
            }
            closePayment()
            depositPopup.waitingPayment = false
            depositWaitTimer.stop()
            depositPopup.close()
        }

        function completePayment(reason) {
            if (paymentCompleted)
                return
            paymentCompleted = true
            console.log("[PAY] оплата завершена:", reason)
            // Закрываемся не сразу: сюда попадаем из колбэка WebView, а скрытие
            // окна уничтожает сам WebView вместе с его нативным HWND. Синхронно
            // это оставляло пустое окно на экране.
            payFinishDelay.restart()
        }

        function finishPaymentNow() {
            const paymentId = depositPopup.paymentId
            closePayment()

            depositWaitTimer.stop()
            depositPopup.waitingPayment = false
            depositPopup.statusText = ""
            depositPopup.lastError = ""
            depositPopup.close()

            // Как «Вернуться» на сайте: дергаем ЮKassa и зачисляем баланс,
            // если вебхук ещё не успел. refreshBalance внутри sync.
            if (typeof NetworkManager !== "undefined") {
                if (paymentId && paymentId.length > 0)
                    NetworkManager.syncTopUpPayment(paymentId)
                else
                    NetworkManager.refreshBalance()
            }
            payBalanceRecheck.restart()
        }

        function isBlankUrl(u) {
            var s = (u || "").toString()
            return s.length === 0 || s === "about:blank"
        }

        function webView() {
            return payViewLoader.item
        }

        function openWithUrl(url) {
            pendingUrl = (url || "").toString()
            widgetPageUrl = pendingUrl
            widgetOrigin = originOf(pendingUrl)
            statusBanner = "Загрузка формы ЮKassa…"
            loadError = ""
            paymentCompleted = false
            // Окно без системной рамки — центрируем сами.
            if (typeof Screen !== "undefined" && Screen.width > 0) {
                x = Math.round((Screen.width - width) / 2)
                y = Math.round((Screen.height - height) / 2)
            }
            visible = true
            raise()
            requestActivate()
            // WebView создаётся Loader'ом уже в видимом окне (см. payViewLoader),
            // навигация уходит из onLoaded / по таймеру.
            payNavigateDelay.restart()
        }

        function closePayment() {
            payNavigateDelay.stop()
            payNavigateRetry.stop()
            payDiagLater.stop()
            payDiagFinal.stop()
            payDonePoll.stop()
            payFinishDelay.stop()
            pendingUrl = ""
            statusBanner = ""
            loadError = ""
            visible = false
        }

        function applyPendingUrl() {
            if (isBlankUrl(pendingUrl)) {
                loadError = "Пустой URL виджета от сервера"
                console.log("[PAY] applyPendingUrl: пустой URL")
                return
            }
            var wv = webView()
            if (!wv) {
                console.log("[PAY] applyPendingUrl: WebView ещё не создан, ждём")
                payNavigateDelay.restart()
                return
            }
            statusBanner = "Открываем: " + pendingUrl
            console.log("[PAY] navigate ->", pendingUrl)
            wv.url = pendingUrl
        }

        function diagnose(tag) {
            var wv = webView()
            if (!wv)
                return
            wv.runJavaScript(
                "(function(){try{" +
                "var f=document.getElementById('payment-form');" +
                "var fr=f?f.querySelector('iframe'):null;" +
                "var r=fr?fr.getBoundingClientRect():null;" +
                "var s=document.getElementById('status');" +
                "return JSON.stringify({" +
                "href:location.href," +
                "secure:window.isSecureContext," +
                "title:document.title," +
                "bodyLen:(document.body?document.body.innerText.length:-1)," +
                "hasForm:!!f," +
                "iframes:document.querySelectorAll('iframe').length," +
                "iframeSrc:(fr?fr.src.slice(0,90):'none')," +
                "iframeW:(r?Math.round(r.width):-1)," +
                "iframeH:(r?Math.round(r.height):-1)," +
                "ymLoaded:!!window.YooMoneyCheckoutWidget," +
                "status:(s?s.innerText:'')," +
                "errors:(window.__payErrors||[])," +
                "iframeStyle:(fr?(fr.getAttribute('style')||'').slice(0,160):'')," +
                "formHTML:(f?f.innerHTML.slice(0,300):'')," +
                "vis:document.visibilityState," +
                "hasFocus:document.hasFocus()" +
                "});}catch(e){return 'JSERR:'+e;}})()",
                function(res) {
                    console.log("[PAY] diag(" + tag + "):", res)
                    if (res && res.indexOf('"secure":false') >= 0) {
                        payWindow.loadError =
                            "Форма не отрисуется: страница открыта по http (не secure context)."
                    }
                })
        }

        onClosing: function(close) {
            close.accepted = true
            // Ручное закрытие (крестик): тоже синхронизируем платёж.
            if (!paymentCompleted && depositPopup.paymentId.length > 0
                    && typeof NetworkManager !== "undefined") {
                NetworkManager.syncTopUpPayment(depositPopup.paymentId)
            }
            payNavigateDelay.stop()
            payDiagLater.stop()
            payDiagFinal.stop()
            payDonePoll.stop()
            payFinishDelay.stop()
            pendingUrl = ""
            depositPopup.waitingPayment = false
            depositWaitTimer.stop()
            depositPopup.close()
        }

        Timer {
            id: payNavigateDelay
            interval: 250
            repeat: false
            onTriggered: payWindow.applyPendingUrl()
        }

        // Виджет ЮKassa монтируется асинхронно — снимаем состояние ещё раз позже.
        Timer {
            id: payDiagLater
            interval: 4000
            repeat: false
            onTriggered: {
                payWindow.diagnose("after4s")
                payDiagFinal.restart()
            }
        }

        Timer {
            id: payDiagFinal
            interval: 6000
            repeat: false
            onTriggered: payWindow.diagnose("after10s")
        }

        Timer {
            id: payFinishDelay
            interval: 60
            repeat: false
            onTriggered: payWindow.finishPaymentNow()
        }

        Timer {
            id: payBalanceRecheck
            interval: 5000
            repeat: false
            onTriggered: {
                if (typeof NetworkManager !== "undefined")
                    NetworkManager.refreshBalance()
            }
        }

        // Страница выставляет window.__payDone в колбэке виджета (оплата прошла)
        // и window.__payClose по клику на крестик в своей шапке. Опрашиваем оба.
        Timer {
            id: payDonePoll
            interval: 500
            repeat: true
            running: payWindow.visible && !payWindow.paymentCompleted
            onTriggered: {
                var wv = payWindow.webView()
                if (!wv)
                    return
                wv.runJavaScript(
                    "(function(){try{return (window.__payDone===true?1:0)+(window.__payClose===true?2:0)}catch(e){return 0}})()",
                    function(flags) {
                        if (flags & 1)
                            payWindow.completePayment("колбэк виджета")
                        else if (flags & 2)
                            payWindow.userClose()
                    })
            }
        }

        // Повтор, если после Navigate всё ещё blank (контроллер WebView2 не успел).
        Timer {
            id: payNavigateRetry
            interval: 700
            repeat: false
            onTriggered: {
                if (!payWindow.visible || payWindow.isBlankUrl(payWindow.pendingUrl))
                    return
                var wv = payWindow.webView()
                var cur = wv ? (wv.url || "").toString() : ""
                if (payWindow.isBlankUrl(cur) || cur !== payWindow.pendingUrl)
                    payWindow.applyPendingUrl()
            }
        }

        // Тонкая зелёная рамка вокруг всего окна — вся «шапка» уже на странице.
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: accentColor
            border.width: 1
            radius: Theme.radiusSm
            z: 10
        }

        Item {
            id: payContent
            anchors.fill: parent
            anchors.margins: 1

            // Закрытие по Escape — системной кнопки окна больше нет.
            focus: true
            Keys.onEscapePressed: payWindow.userClose()

            // Шапка окна: живёт над WebView, а не поверх него — нативный HWND
            // WebView2 перекрывает любые QML-элементы, положенные сверху.
            Rectangle {
                id: payHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 58
                color: Theme.bgPanel

                MouseArea {
                    anchors.fill: parent
                    onPressed: payWindow.startSystemMove()
                }

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Text {
                        text: "REACTOR PAY"
                        color: accentColor
                        font.pixelSize: 10
                        font.bold: true
                        font.italic: true
                        font.letterSpacing: 2.2
                    }
                    Text {
                        text: Math.round(payWindow.amount).toLocaleString(Qt.locale("ru_RU"), 'f', 0) + " ₽"
                        color: "#ffffff"
                        font.pixelSize: 20
                        font.bold: true
                        font.italic: true
                    }
                }

                Rectangle {
                    id: payCloseBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                    height: 36
                    radius: Theme.radiusSm
                    color: closeHover.hovered ? "#161616" : "transparent"
                    border.width: 1
                    border.color: closeHover.hovered ? "#4d4d4d" : "#1f1f1f"

                    HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: closeHover.hovered ? "#ffffff" : "#6b6b6b"
                        font.pixelSize: 15
                        font.bold: true
                    }

                    TapHandler { onTapped: payWindow.userClose() }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: "#1a1a1a"
                }
            }

            // WebView2 фиксирует видимость в момент создания контроллера.
            // Если создать его в скрытом окне, страница навсегда остаётся
            // document.visibilityState = "hidden", и виджет ЮKassa прячет форму
            // (height: 0). Поэтому создаём WebView только после показа окна.
            Loader {
                id: payViewLoader
                anchors.top: payHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                active: payWindow.visible
                sourceComponent: payViewComponent

                onLoaded: {
                    console.log("[PAY] WebView создан в видимом окне")
                    payNavigateDelay.restart()
                }
            }

            Component {
                id: payViewComponent

                WebView {
                    // Не задаём url здесь — только через applyPendingUrl(), иначе
                    // LoadSucceeded на about:blank помечает оплату как «готовую».

                    onLoadingChanged: function(loadRequest) {
                        var u = (loadRequest.url || url || "").toString()
                        console.log("[PAY] loadingChanged status =", loadRequest.status,
                                    "url =", u, "err =", loadRequest.errorString)
                        if (payWindow.isBlankUrl(u))
                            return

                        // Страховка на случай, если колбэк не сработал: после оплаты
                        // виджет уводит окно на сайт — там уже нечего показывать.
                        if (payWindow.isPostPaymentUrl(u)) {
                            payWindow.completePayment("уход со страницы виджета: " + u)
                            return
                        }

                        if (loadRequest.status === WebView.LoadStartedStatus) {
                            payWindow.statusBanner = "Загрузка формы ЮKassa…"
                            payWindow.loadError = ""
                        } else if (loadRequest.status === WebView.LoadSucceededStatus) {
                            payWindow.statusBanner = ""
                            payWindow.loadError = ""
                            payWindow.diagnose("load")
                            payDiagLater.restart()
                        } else if (loadRequest.status === WebView.LoadFailedStatus) {
                            payWindow.loadError = "Не удалось открыть форму оплаты.\n"
                                    + (loadRequest.errorString || "ошибка загрузки")
                            depositPopup.lastError = payWindow.loadError
                            payNavigateRetry.restart()
                        }
                    }
                }
            }

            // Заставка поверх WebView, пока страница ЮKassa грузится, — чтобы не
            // мигало пустое чёрное окно до появления формы. Скрывается по загрузке.
            Rectangle {
                id: payLoadingCover
                anchors.top: payHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: Theme.bgPanel
                visible: payWindow.loadError.length > 0
                         || (payWindow.statusBanner.length > 0 && !payWindow.paymentCompleted)

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    width: parent.width - 80

                    BusyIndicator {
                        visible: payWindow.loadError.length === 0
                        running: visible
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    Text {
                        text: payWindow.loadError.length > 0
                              ? payWindow.loadError
                              : "ЗАГРУЗКА ФОРМЫ"
                        color: payWindow.loadError.length > 0 ? "#f87171" : accentColor
                        font.pixelSize: payWindow.loadError.length > 0 ? 13 : 10
                        font.bold: true
                        font.italic: true
                        font.letterSpacing: payWindow.loadError.length > 0 ? 0 : 2
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                    Button {
                        visible: payWindow.loadError.length > 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Закрыть"
                        onClicked: payWindow.userClose()
                    }
                }
            }
        }
    }

    Popup {
        id: steamLimitAlertPopup; width: 600; height: 380; anchors.centerIn: parent; modal: true
        background: Rectangle { color: "#0a0505"; border.color: accentColor; radius: Theme.radiusSm }
        property string targetExe: ""; property string targetArgs: ""
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 30; spacing: 15
            Text { text: "⚙️ СИСТЕМА КОНТРОЛЯ ТРАФИКА"; color: accentColor; font.bold: true; Layout.alignment: Qt.AlignHCenter }
            Button { text: "ПОНЯТНО, ЗАПУСТИТЬ"; Layout.alignment: Qt.AlignHCenter; onClicked: { if (typeof Launcher !== 'undefined') Launcher.launch(steamLimitAlertPopup.targetExe, steamLimitAlertPopup.targetArgs, "", "", ""); steamLimitAlertPopup.close() } }
        }
    }

    Connections {
        target: typeof root !== 'undefined' ? root : null
        function onIsLoggingInChanged() {
            if (root && root.isLoggingIn) {
                accountChoicePopup.close()
                clubBusyHintPopup.close()
            }
        }
        function onGameLoadingVisibleChanged() {
            if (root && root.gameLoadingVisible) {
                accountChoicePopup.close()
                clubBusyHintPopup.close()
            }
        }
    }

    Popup {
        id: rebootConfirmPopup
        width: Math.min(560, parent.width - 40)
        height: 380
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property string rebootPin: ""
        property bool pinReady: false
        property bool pinLoading: false
        property string pinError: ""

        function requestPinAndOpen() {
            rebootPin = ""
            pinReady = false
            pinLoading = true
            pinError = ""
            open()

            var baseUrl = dashboardRoot.apiBase()
            if (baseUrl.length === 0) {
                pinLoading = false
                pinError = "Адрес сервера не задан в config.ini"
                return
            }
            var pcId = parseInt(dashboardRoot.termId)
            if (!pcId) {
                pinLoading = false
                pinError = "Не удалось определить ПК"
                console.error("[REBOOT] terminalId пуст")
                return
            }
            var xhr = new XMLHttpRequest()
            xhr.open("POST", baseUrl + "/api/shell/games/pause")
            xhr.setRequestHeader("Content-Type", "application/json")
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE)
                    return
                pinLoading = false
                if (xhr.status !== 200) {
                    pinError = "Не удалось сохранить PIN"
                    console.error("[REBOOT] pause HTTP", xhr.status, xhr.responseText)
                    return
                }
                try {
                    var res = JSON.parse(xhr.responseText)
                    if (res.status === "success" && res.pin_code) {
                        rebootPin = String(res.pin_code)
                        pinReady = true
                        if (typeof root !== 'undefined')
                            root.temporaryPausePin = rebootPin
                        console.log("[REBOOT] PIN сохранён в БД (pause API)")
                    } else {
                        pinError = res.message || "Не удалось сохранить PIN"
                        console.error("[REBOOT] отказ:", res.message || xhr.responseText)
                    }
                } catch (e) {
                    pinError = "Ошибка ответа сервера"
                    console.error("[REBOOT] parse:", e)
                }
            }
            xhr.send(JSON.stringify({
                "computer_id": pcId,
                "booking_id": (typeof NetworkManager !== 'undefined') ? NetworkManager.lastBookingId : 0
            }))
        }

        background: Rectangle {
            color: "#0a0505"
            border.color: accentColor
            radius: Theme.radiusSm
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 14
            Text {
                text: "Перезагрузка"
                color: accentColor
                font.pixelSize: 22
                font.bold: true
                font.letterSpacing: 2
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textBody
                font.pixelSize: 15
                lineHeight: 1.35
                text: "Сохраните пин код для входа после перезагрузки"
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                color: rebootConfirmPopup.pinError !== "" ? Theme.danger
                       : (rebootConfirmPopup.pinReady ? accentColor : Theme.textSecondary)
                font.pixelSize: rebootConfirmPopup.pinReady ? 42 : 16
                font.bold: true
                font.letterSpacing: rebootConfirmPopup.pinReady ? 10 : 1
                text: rebootConfirmPopup.pinLoading ? "Получение PIN…"
                      : (rebootConfirmPopup.pinReady ? rebootConfirmPopup.rebootPin
                         : (rebootConfirmPopup.pinError !== "" ? rebootConfirmPopup.pinError : "— — — —"))
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textSecondary
                font.pixelSize: 14
                lineHeight: 1.3
                text: "Перезагрузить компьютер?"
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 6
                    enabled: rebootConfirmPopup.pinReady
                    opacity: rebootConfirmPopup.pinReady
                             ? (rebootConfirmMouse.pressed ? 0.9 : 1.0)
                             : 0.45
                    color: !rebootConfirmPopup.pinReady
                           ? "#7f1d1d"
                           : (rebootConfirmMouse.pressed
                              ? Qt.darker("#b91c1c", 1.25)
                              : (rebootConfirmMouse.containsMouse
                                 ? Qt.lighter("#b91c1c", 1.12)
                                 : "#b91c1c"))
                    scale: rebootConfirmMouse.pressed && rebootConfirmPopup.pinReady
                           ? 0.96
                           : (rebootConfirmMouse.containsMouse && rebootConfirmPopup.pinReady ? 1.02 : 1.0)
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Text {
                        anchors.centerIn: parent
                        text: "Перезагрузить"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 14
                    }
                    MouseArea {
                        id: rebootConfirmMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: rebootConfirmPopup.pinReady
                        cursorShape: rebootConfirmPopup.pinReady ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            if (!rebootConfirmPopup.pinReady)
                                return
                            rebootConfirmPopup.close()
                            if (typeof Launcher !== "undefined")
                                Launcher.rebootPC()
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 6
                    color: rebootCancelMouse.pressed
                           ? Qt.rgba(0.06, 0.12, 0.1, 0.85)
                           : (rebootCancelMouse.containsMouse
                              ? Qt.rgba(0.08, 0.14, 0.11, 0.55)
                              : "transparent")
                    border.color: rebootCancelMouse.containsMouse || rebootCancelMouse.pressed
                                  ? Qt.lighter(accentColor, 1.2)
                                  : accentColor
                    border.width: 2
                    scale: rebootCancelMouse.pressed ? 0.96 : (rebootCancelMouse.containsMouse ? 1.02 : 1.0)
                    opacity: rebootCancelMouse.pressed ? 0.9 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Text {
                        anchors.centerIn: parent
                        text: "Отмена"
                        color: rebootCancelMouse.containsMouse || rebootCancelMouse.pressed
                               ? Qt.lighter(accentColor, 1.15)
                               : accentColor
                        font.bold: true
                        font.pixelSize: 14
                        Behavior on color { ColorAnimation { duration: 100 } }
                    }
                    MouseArea {
                        id: rebootCancelMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: rebootConfirmPopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: sosReasonPopup
        width: Math.min(420, parent.width - 40)
        height: 360
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#0a0505"
            border.color: Theme.dangerStrong
            radius: Theme.radiusSm
        }

        function sendReason(code, label) {
            sosReasonPopup.close()
            if (typeof NetworkManager !== 'undefined') {
                NetworkManager.sendSos(code, label)
            } else {
                console.warn("[QML-SOS] NetworkManager unavailable")
            }
            sosToast.show()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 14

            Text {
                text: "SOS"
                color: Theme.dangerStrong
                font.pixelSize: 22
                font.bold: true
                font.letterSpacing: 3
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: "Что случилось?"
                color: Theme.textBody
                font.pixelSize: 15
                Layout.alignment: Qt.AlignHCenter
            }

            Repeater {
                model: [
                    { code: "peripherals", label: "Не работают наушники / мышь / клавиатура" },
                    { code: "auth_help", label: "Помочь с авторизацией" },
                    { code: "other", label: "Другая проблема" }
                ]
                delegate: Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 6
                    color: reasonMouse.pressed
                           ? "#991b1b"
                           : (reasonMouse.containsMouse ? Theme.dangerStrong : "#1a0a0a")
                    border.color: Theme.dangerStrong
                    border.width: 1
                    scale: reasonMouse.pressed ? 0.96 : (reasonMouse.containsMouse ? 1.02 : 1.0)
                    opacity: reasonMouse.pressed ? 0.9 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.fill: parent
                        anchors.margins: 10
                        text: modelData.label
                        color: reasonMouse.containsMouse || reasonMouse.pressed ? "black" : "#f5f5f5"
                        font.pixelSize: 13
                        font.bold: true
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                    }
                    MouseArea {
                        id: reasonMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sosReasonPopup.sendReason(modelData.code, modelData.label)
                    }
                }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Отмена"
                color: cancelSosMouse.pressed
                       ? "#aaaaaa"
                       : (cancelSosMouse.containsMouse ? "#999999" : Theme.textMuted)
                font.pixelSize: 13
                scale: cancelSosMouse.pressed ? 0.96 : 1.0
                opacity: cancelSosMouse.pressed ? 0.85 : 1.0
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                Behavior on opacity { NumberAnimation { duration: 100 } }
                Behavior on color { ColorAnimation { duration: 100 } }
                MouseArea {
                    id: cancelSosMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sosReasonPopup.close()
                }
            }
        }
    }

    Rectangle {
        id: sosToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 48
        width: 220
        height: 44
        radius: 8
        z: 1000
        visible: opacity > 0
        opacity: 0
        color: "#0a0505"
        border.color: Theme.dangerStrong
        border.width: 1

        function show() {
            sosToast.opacity = 1
            sosToastHide.restart()
        }

        Behavior on opacity { NumberAnimation { duration: 220 } }

        Text {
            anchors.centerIn: parent
            text: "Запрос отправлен"
            color: "#f5f5f5"
            font.pixelSize: 14
            font.bold: true
        }

        Timer {
            id: sosToastHide
            interval: 1800
            onTriggered: sosToast.opacity = 0
        }
    }

    Popup {
        id: accountChoicePopup
        width: Math.min(640, parent.width - 40)
        height: 420
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        readonly property string brandKey: dashboardRoot.pendingGamePlatform
        readonly property color brandColor: dashboardRoot.platformBrandColor(brandKey)
        readonly property string brandTitle: dashboardRoot.platformBrandTitle(brandKey)
        readonly property string shortName: dashboardRoot.platformShortName(brandKey)
        background: Rectangle {
            color: "#0a0505"
            border.color: accountChoicePopup.brandColor
            radius: Theme.radiusSm
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 16

            Text {
                text: accountChoicePopup.brandTitle
                color: accountChoicePopup.brandColor
                font.pixelSize: 22
                font.bold: true
                font.letterSpacing: 2
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: dashboardRoot.pendingGameTitle.length > 0
                      ? dashboardRoot.pendingGameTitle
                      : ("Игра " + accountChoicePopup.shortName)
                color: "#aaaaaa"
                font.pixelSize: 14
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textBody
                font.pixelSize: 16
                lineHeight: 1.35
                text: "У вас есть личный аккаунт " + accountChoicePopup.shortName + "?\n\n"
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
                    color: ownAccMouse.pressed
                           ? Qt.darker(accountChoicePopup.brandColor, 1.25)
                           : (ownAccMouse.containsMouse
                              ? Qt.lighter(accountChoicePopup.brandColor, 1.12)
                              : accountChoicePopup.brandColor)
                    scale: ownAccMouse.pressed ? 0.96 : (ownAccMouse.containsMouse ? 1.02 : 1.0)
                    opacity: ownAccMouse.pressed ? 0.9 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Text {
                        anchors.centerIn: parent
                        text: "СВОЙ АККАУНТ"
                        color: accountChoicePopup.brandKey === "epic" ? "black" : "white"
                        font.bold: true
                        font.pixelSize: 14
                    }
                    MouseArea {
                        id: ownAccMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dashboardRoot.launchPersonal()
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 6
                    color: clubAccMouse.pressed
                           ? Qt.rgba(0.06, 0.12, 0.1, 0.85)
                           : (clubAccMouse.containsMouse
                              ? Qt.rgba(0.08, 0.14, 0.11, 0.55)
                              : "transparent")
                    border.color: clubAccMouse.containsMouse || clubAccMouse.pressed
                                  ? Qt.lighter(accentColor, 1.2)
                                  : accentColor
                    border.width: 2
                    scale: clubAccMouse.pressed ? 0.96 : (clubAccMouse.containsMouse ? 1.02 : 1.0)
                    opacity: clubAccMouse.pressed ? 0.9 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Text {
                        anchors.centerIn: parent
                        text: "КЛУБНЫЙ АККАУНТ"
                        color: clubAccMouse.containsMouse || clubAccMouse.pressed
                               ? Qt.lighter(accentColor, 1.15)
                               : accentColor
                        font.bold: true
                        font.pixelSize: 14
                        Behavior on color { ColorAnimation { duration: 100 } }
                    }
                    MouseArea {
                        id: clubAccMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dashboardRoot.launchClub()
                    }
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Отмена"
                color: cancelAccMouse.pressed
                       ? "#aaaaaa"
                       : (cancelAccMouse.containsMouse ? "#999999" : Theme.textMuted)
                font.pixelSize: 13
                scale: cancelAccMouse.pressed ? 0.96 : 1.0
                opacity: cancelAccMouse.pressed ? 0.85 : 1.0
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                Behavior on opacity { NumberAnimation { duration: 100 } }
                Behavior on color { ColorAnimation { duration: 100 } }
                MouseArea {
                    id: cancelAccMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: accountChoicePopup.close()
                }
            }
        }
    }

    Popup {
        id: clubBusyHintPopup
        width: Math.min(560, parent.width - 40)
        height: 280
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        readonly property string brandKey: dashboardRoot.pendingGamePlatform
        readonly property color brandColor: dashboardRoot.platformBrandColor(brandKey)
        readonly property string shortName: dashboardRoot.platformShortName(brandKey)
        background: Rectangle {
            color: "#0a0505"
            border.color: clubBusyHintPopup.brandColor
            radius: Theme.radiusSm
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 18
            Text {
                text: "КЛУБНЫЕ АККАУНТЫ ЗАНЯТЫ"
                color: clubBusyHintPopup.brandColor
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
                text: "Сейчас нет свободного клубного аккаунта "
                      + clubBusyHintPopup.shortName
                      + ".\nМожно войти под своим — прогресс и скины останутся у вас."
            }
            Item { Layout.fillHeight: true }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                radius: 6
                color: busyOwnMouse.pressed
                       ? Qt.darker(clubBusyHintPopup.brandColor, 1.25)
                       : (busyOwnMouse.containsMouse
                          ? Qt.lighter(clubBusyHintPopup.brandColor, 1.12)
                          : clubBusyHintPopup.brandColor)
                scale: busyOwnMouse.pressed ? 0.96 : (busyOwnMouse.containsMouse ? 1.02 : 1.0)
                opacity: busyOwnMouse.pressed ? 0.9 : 1.0
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                Behavior on opacity { NumberAnimation { duration: 100 } }
                Behavior on color { ColorAnimation { duration: 100 } }
                Text {
                    anchors.centerIn: parent
                    text: "ВОЙТИ ПОД СВОИМ"
                    color: clubBusyHintPopup.brandKey === "epic" ? "black" : "white"
                    font.bold: true
                }
                MouseArea {
                    id: busyOwnMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        clubBusyHintPopup.close()
                        dashboardRoot.launchPersonal()
                    }
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Закрыть"
                color: busyCloseMouse.pressed
                       ? "#aaaaaa"
                       : (busyCloseMouse.containsMouse ? "#999999" : Theme.textMuted)
                scale: busyCloseMouse.pressed ? 0.96 : 1.0
                opacity: busyCloseMouse.pressed ? 0.85 : 1.0
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                Behavior on opacity { NumberAnimation { duration: 100 } }
                Behavior on color { ColorAnimation { duration: 100 } }
                MouseArea {
                    id: busyCloseMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: clubBusyHintPopup.close()
                }
            }
        }
    }

    component ActionBtn : Rectangle {
        id: controlRoot
        property string text: "BUTTON"
        property string icon: ""
        property color baseColor: accentColor
        property bool isActiveStatus: false
        property bool orderIsFinished: false
        property bool orderIsCooking: false
        property string statusText: ""
        // Половинная кнопка: контент по центру, статус второй строкой.
        property bool compact: false
        readonly property color statusAccent: orderIsFinished ? Theme.success
                                              : (orderIsCooking ? Theme.warning : Theme.danger)
        signal clicked()

        // Не даём тексту/статусу раздувать ширину в RowLayout — колонки строго 50/50.
        implicitWidth: 0
        clip: true
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.minimumWidth: 0
        Layout.preferredHeight: compact ? 44 : 50
        // Hover scale искажает «размер» соседних кнопок — только opacity/border.
        scale: 1.0
        radius: 4
        color: actionMouse.pressed
               ? (isActiveStatus ? Qt.rgba(0.12, 0.1, 0.02, 1) : Qt.rgba(0.06, 0.12, 0.1, 1))
               : (actionMouse.containsMouse ? Qt.rgba(0.08, 0.14, 0.11, 1) : "transparent")
        border.color: actionMouse.containsMouse || actionMouse.pressed || isActiveStatus
                      ? (isActiveStatus && !orderIsFinished ? statusAccent : baseColor)
                      : Qt.darker(baseColor, 1.25)
        border.width: actionMouse.containsMouse || actionMouse.pressed || isActiveStatus ? 2 : 1
        opacity: actionMouse.containsMouse || isActiveStatus ? 1 : 0.94

        Behavior on opacity { NumberAnimation { duration: 100 } }
        Behavior on border.width { NumberAnimation { duration: 90 } }
        Behavior on color { ColorAnimation { duration: 100 } }
        Behavior on border.color { ColorAnimation { duration: 120 } }

        Row {
            visible: !controlRoot.compact
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 14
            spacing: 6
            Text {
                text: icon
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: controlRoot.text
                color: actionMouse.pressed ? "black" : (actionMouse.containsMouse || isActiveStatus ? "white" : baseColor)
                font.bold: true
                font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: " (" + controlRoot.statusText + ")"
                visible: controlRoot.isActiveStatus && controlRoot.statusText !== ""
                color: controlRoot.statusAccent
                font.bold: true
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Column {
            visible: controlRoot.compact
            anchors.centerIn: parent
            width: parent.width - 12
            spacing: 1

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 5
                Text {
                    text: icon
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: controlRoot.text
                    color: actionMouse.pressed ? "black" : (actionMouse.containsMouse || isActiveStatus ? "white" : baseColor)
                    font.bold: true
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                visible: controlRoot.isActiveStatus && controlRoot.statusText !== ""
                text: controlRoot.statusText
                color: controlRoot.statusAccent
                font.bold: true
                font.pixelSize: 9
            }
        }

        // Spinner while order is in progress
        Item {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 14
            width: 16
            height: 16
            visible: controlRoot.isActiveStatus && !controlRoot.orderIsFinished && !controlRoot.compact

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: controlRoot.statusAccent
                border.width: 2
                radius: 8

                Rectangle {
                    width: 9
                    height: 9
                    color: darkBg
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: -2
                }

                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 1000
                    loops: Animation.Infinite
                    running: controlRoot.isActiveStatus && !controlRoot.orderIsFinished
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 14
            text: "✓"
            color: Theme.success
            font.bold: true
            font.pixelSize: 16
            visible: controlRoot.isActiveStatus && controlRoot.orderIsFinished && !controlRoot.compact
        }

        MouseArea {
            id: actionMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: controlRoot.clicked()
        }
    }

    component PlatformSquareBtn : Rectangle {
        id: platBtn
        property string btnText: "LAUNCH"
        property string iconText: "🎮"
        property string iconSource: ""   // optional qrc:/… png/svg; falls back to iconText
        property color brandColor: accentColor
        signal clicked()

        Layout.fillWidth: true
        Layout.preferredHeight: 58
        radius: 4
        color: {
            if (platBtnMouse.pressed)
                return Qt.rgba(0.06, 0.12, 0.1, 1)
            if (platBtnMouse.containsMouse)
                return Qt.rgba(0.08, 0.14, 0.11, 1)
            return "#0a0d0b"
        }
        border.color: platBtnMouse.containsMouse || platBtnMouse.pressed
                      ? brandColor
                      : Qt.darker(brandColor, 1.35)
        border.width: platBtnMouse.containsMouse || platBtnMouse.pressed ? 2 : 1
        scale: platBtnMouse.pressed ? 0.96 : (platBtnMouse.containsMouse ? 1.03 : 1.0)
        opacity: platBtnMouse.containsMouse ? 1 : 0.92

        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 120 } }
        Behavior on border.width { NumberAnimation { duration: 100 } }
        Behavior on color { ColorAnimation { duration: 120 } }

        Column {
            anchors.centerIn: parent
            spacing: 3
            Item {
                width: 22
                height: 22
                anchors.horizontalCenter: parent.horizontalCenter
                scale: platBtnMouse.containsMouse ? 1.1 : 1.0
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

                Image {
                    anchors.fill: parent
                    visible: platBtn.iconSource.length > 0
                    source: platBtn.iconSource
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    smooth: true
                    mipmap: true
                }
                Text {
                    anchors.centerIn: parent
                    visible: platBtn.iconSource.length === 0
                    text: iconText
                    color: brandColor
                    font.pixelSize: 18
                }
            }
            Text {
                text: btnText
                color: platBtnMouse.containsMouse ? brandColor : "white"
                font.pixelSize: Theme.fontCaption
                font.bold: platBtnMouse.containsMouse
                anchors.horizontalCenter: parent.horizontalCenter
                Behavior on color { ColorAnimation { duration: 120 } }
            }
        }

        MouseArea {
            id: platBtnMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: platBtn.clicked()
        }
    }
}
