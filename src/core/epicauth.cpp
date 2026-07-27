#include "epicauth.h"
#include "processmanager.h"
#include "networkmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <UIAutomation.h>
#endif

static bool writeTextFile(const QString &path, const QString &content)
{
    if (content.isEmpty())
        return false;
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[EPIC] write fail:" << path << file.errorString();
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << content;
    file.close();
    return true;
}

static QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    return in.readAll();
}

static QString epicLocalAppData()
{
    QString local = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
    if (local.isEmpty())
        local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir::toNativeSeparators(local);
}

// Корень Portal из exe: .../Portal/Binaries/Win64/EpicGamesLauncher.exe → .../Portal
static QString epicPortalRootFromExe(const QString &launcherExe)
{
    if (launcherExe.isEmpty())
        return QString();
    QDir d = QFileInfo(launcherExe).absoluteDir(); // Win64
    if (d.dirName().compare(QStringLiteral("Win64"), Qt::CaseInsensitive) == 0
        || d.dirName().compare(QStringLiteral("Win32"), Qt::CaseInsensitive) == 0)
        d.cdUp(); // Binaries
    if (d.dirName().compare(QStringLiteral("Binaries"), Qt::CaseInsensitive) == 0)
        d.cdUp(); // Portal
    return d.absolutePath();
}

static QStringList epicSearchRoots(const QString &launcherExe)
{
    QStringList roots;
    const QString local = epicLocalAppData();
    roots << (local + QStringLiteral("/EpicGamesLauncher"));
    roots << (local + QStringLiteral("/EpicGames"));

    const QString portal = epicPortalRootFromExe(launcherExe);
    if (!portal.isEmpty()) {
        roots << (portal + QStringLiteral("/Saved"));
        roots << portal;
        QDir up(portal);
        if (up.cdUp())
            roots << up.absolutePath(); // C:\Launcher
    }

    // Частый кастомный путь клуба
    roots << QStringLiteral("C:/Launcher/Portal/Saved");
    roots << QStringLiteral("C:/Launcher");

    roots.removeDuplicates();
    return roots;
}

static QStringList epicGameUserSettingsCandidates(const QString &launcherExe)
{
    QStringList paths;
    const QString local = epicLocalAppData();
    const QString portal = epicPortalRootFromExe(launcherExe);

    const QStringList suffixes = {
        QStringLiteral("/Saved/Config/Windows/GameUserSettings.ini"),
        QStringLiteral("/Saved/Config/WindowsClient/GameUserSettings.ini"),
        QStringLiteral("/Saved/Config/WindowsNoEditor/GameUserSettings.ini"),
        QStringLiteral("/Config/Windows/GameUserSettings.ini"),
    };

    for (const QString &suffix : suffixes) {
        paths << (local + QStringLiteral("/EpicGamesLauncher") + suffix);
        if (!portal.isEmpty())
            paths << (portal + suffix);
    }
    return paths;
}

// Рекурсивный поиск GameUserSettings.ini / файлов с [RememberMe]
static QString findEpicRememberFile(const QString &launcherExe)
{
    for (const QString &p : epicGameUserSettingsCandidates(launcherExe)) {
        const QString body = readTextFile(p);
        if (!body.trimmed().isEmpty()) {
            qWarning() << "[EPIC] cache hit (direct):" << p << "chars:" << body.size();
            return p;
        }
    }

    for (const QString &root : epicSearchRoots(launcherExe)) {
        QDir dir(root);
        if (!dir.exists()) {
            qWarning() << "[EPIC] cache root отсутствует:" << root;
            continue;
        }
        qWarning() << "[EPIC] cache scan:" << root;

        QDirIterator it(root,
                        QStringList{QStringLiteral("GameUserSettings.ini"),
                                    QStringLiteral("*.ini")},
                        QDir::Files,
                        QDirIterator::Subdirectories);
        int checked = 0;
        while (it.hasNext() && checked < 80) {
            const QString path = it.next();
            ++checked;
            const QString name = QFileInfo(path).fileName();
            const QString body = readTextFile(path);
            if (body.trimmed().isEmpty())
                continue;
            const bool remember = body.contains(QStringLiteral("[RememberMe]"), Qt::CaseInsensitive)
                                  || body.contains(QStringLiteral("RememberMe"), Qt::CaseInsensitive);
            if (name.compare(QStringLiteral("GameUserSettings.ini"), Qt::CaseInsensitive) == 0
                || remember) {
                qWarning() << "[EPIC] cache hit (scan):" << path << "chars:" << body.size()
                           << "RememberMe:" << remember;
                return path;
            }
        }
    }
    return QString();
}

static QString epicGameUserSettingsPath(const QString &launcherExe)
{
    const QString found = findEpicRememberFile(launcherExe);
    if (!found.isEmpty())
        return found;
    // путь для записи по умолчанию — рядом с Portal или LocalAppData
    const QString portal = epicPortalRootFromExe(launcherExe);
    if (!portal.isEmpty())
        return portal + QStringLiteral("/Saved/Config/Windows/GameUserSettings.ini");
    return epicLocalAppData()
           + QStringLiteral("/EpicGamesLauncher/Saved/Config/Windows/GameUserSettings.ini");
}

static QString readEpicGameUserSettings(const QString &launcherExe)
{
    const QString path = findEpicRememberFile(launcherExe);
    if (path.isEmpty()) {
        qWarning() << "[EPIC] cache: файл сессии не найден. roots:"
                   << epicSearchRoots(launcherExe);
        return QString();
    }
    return readTextFile(path);
}

static QJsonObject extractVdfFiles(const QJsonObject &authData)
{
    QJsonObject vdf = authData.value(QStringLiteral("vdf_files")).toObject();
    if (vdf.isEmpty()) {
        const QJsonObject auth = authData.value(QStringLiteral("auth")).toObject();
        const QJsonObject cache = auth.value(QStringLiteral("cache")).toObject();
        if (cache.contains(QStringLiteral("vdf_files")))
            vdf = cache.value(QStringLiteral("vdf_files")).toObject();
        else
            vdf = cache;
    }
    return vdf;
}

#ifdef Q_OS_WIN
static QSet<DWORD> collectEpicPids()
{
    QSet<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const QString name = QString::fromWCharArray(pe.szExeFile);
            if (name.compare(QStringLiteral("EpicGamesLauncher.exe"), Qt::CaseInsensitive) == 0
                || name.compare(QStringLiteral("EpicWebHelper.exe"), Qt::CaseInsensitive) == 0
                || name.contains(QStringLiteral("EpicGames"), Qt::CaseInsensitive)) {
                pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static QString processImageForPid(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return QString();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    QString name;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                name = QString::fromWCharArray(pe.szExeFile);
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return name;
}

static void setShellTopmostFrom(QObject *auth, bool enabled)
{
    if (!auth)
        return;
    if (auto *pm = qobject_cast<ProcessManager *>(auth->parent()))
        pm->setShellTopmost(enabled);
}

// Login on-screen под TOPMOST-оверлеем (как EA). Off-screen park ломает CEF Submit.
static void placeWindowForInput(HWND hwnd, QObject *auth)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    DWORD pid = 0;
    const DWORD epicTid = GetWindowThreadProcessId(hwnd, &pid);
    AllowSetForegroundWindow(pid);

    // Без HWND_TOPMOST / SW_RESTORE — иначе login всплывает поверх loading overlay.
    // После краткого foreground сразу поднимаем shell TOPMOST.
    const DWORD ourTid = GetCurrentThreadId();
    const DWORD foreTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, TRUE);
    if (epicTid && epicTid != ourTid)
        AttachThreadInput(ourTid, epicTid, TRUE);

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (epicTid && epicTid != ourTid)
        AttachThreadInput(ourTid, epicTid, FALSE);
    if (foreTid && foreTid != ourTid)
        AttachThreadInput(ourTid, foreTid, FALSE);

    if (auth)
        setShellTopmostFrom(auth, true);
}

static void sendVk(WORD vk)
{
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void sendReturnKey() { sendVk(VK_RETURN); }

static void sendCtrlA()
{
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = 'A';
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = 'A';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static void sendTabs(int count, const char *why)
{
    qWarning() << "[EPIC] Tab x" << count << why;
    for (int i = 0; i < count; ++i) {
        sendVk(VK_TAB);
        Sleep(140);
    }
}

// Exact Tab order (user click/Tab map):
// Email window:  2 = email, 4 = «Продолжить»
// Password window: 3 = password, 6 = «Войти»
// Prefer: Tab×2 → type → Tab×2 → Enter; then Tab×3 → type → Tab×3 → Enter.
// SetForeground once per phase — never between Tabs.

static void typeUnicode(const QString &text)
{
    for (const QChar ch : text) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = 0;
        in[0].ki.wScan = ch.unicode();
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(12);
    }
}

// Caller must already have focused the Epic window. Ctrl+A then replace selection.
static void clearAndType(const QString &text)
{
    sendCtrlA();
    Sleep(50);

    bool pasted = false;
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        const std::wstring wstr = text.toStdWString();
        const size_t bytes = (wstr.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            memcpy(GlobalLock(hMem), wstr.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
            CloseClipboard();
            Sleep(40);
            INPUT in[4] = {};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_CONTROL;
            in[1].type = INPUT_KEYBOARD;
            in[1].ki.wVk = 'V';
            in[2].type = INPUT_KEYBOARD;
            in[2].ki.wVk = 'V';
            in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[3].type = INPUT_KEYBOARD;
            in[3].ki.wVk = VK_CONTROL;
            in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, in, sizeof(INPUT));
            pasted = true;
            qWarning() << "[EPIC] input Ctrl+A + paste, len" << text.size();
        } else {
            CloseClipboard();
        }
    }
    if (!pasted) {
        qWarning() << "[EPIC] input Ctrl+A + unicode type, len" << text.size();
        typeUnicode(text);
    }
}

static QString bstrToQ(BSTR b)
{
    if (!b)
        return QString();
    return QString::fromWCharArray(b);
}

static QString uiaTypeName(CONTROLTYPEID id)
{
    switch (id) {
    case UIA_ButtonControlTypeId: return QStringLiteral("Button");
    case UIA_EditControlTypeId: return QStringLiteral("Edit");
    case UIA_HyperlinkControlTypeId: return QStringLiteral("Hyperlink");
    case UIA_TextControlTypeId: return QStringLiteral("Text");
    case UIA_ImageControlTypeId: return QStringLiteral("Image");
    case UIA_CustomControlTypeId: return QStringLiteral("Custom");
    case UIA_GroupControlTypeId: return QStringLiteral("Group");
    case UIA_PaneControlTypeId: return QStringLiteral("Pane");
    case UIA_WindowControlTypeId: return QStringLiteral("Window");
    case UIA_DocumentControlTypeId: return QStringLiteral("Document");
    case UIA_ListControlTypeId: return QStringLiteral("List");
    case UIA_ListItemControlTypeId: return QStringLiteral("ListItem");
    case UIA_CheckBoxControlTypeId: return QStringLiteral("CheckBox");
    case UIA_ComboBoxControlTypeId: return QStringLiteral("ComboBox");
    case UIA_TabControlTypeId: return QStringLiteral("Tab");
    case UIA_TabItemControlTypeId: return QStringLiteral("TabItem");
    default: return QStringLiteral("Type_%1").arg(id);
    }
}

static void dumpUiaElement(IUIAutomationElement *el, int index, const QString &tag)
{
    if (!el)
        return;

    BSTR name = nullptr, autoId = nullptr, cls = nullptr, help = nullptr, loc = nullptr;
    CONTROLTYPEID ctype = 0;
    BOOL enabled = FALSE, offscreen = FALSE, keyboard = FALSE;
    RECT rect{};

    el->get_CurrentName(&name);
    el->get_CurrentAutomationId(&autoId);
    el->get_CurrentClassName(&cls);
    el->get_CurrentHelpText(&help);
    el->get_CurrentLocalizedControlType(&loc);
    el->get_CurrentControlType(&ctype);
    el->get_CurrentIsEnabled(&enabled);
    el->get_CurrentIsOffscreen(&offscreen);
    el->get_CurrentIsKeyboardFocusable(&keyboard);
    el->get_CurrentBoundingRectangle(&rect);

    qWarning().noquote() << QStringLiteral("[EPIC][%1] #%2 type=%3 (%4)")
                                .arg(tag)
                                .arg(index)
                                .arg(uiaTypeName(ctype), bstrToQ(loc));
    qWarning().noquote() << QStringLiteral("[EPIC][%1]   name=\"%2\" automationId=\"%3\" class=\"%4\"")
                                .arg(tag, bstrToQ(name), bstrToQ(autoId), bstrToQ(cls));
    qWarning().noquote() << QStringLiteral("[EPIC][%1]   enabled=%2 offscreen=%3 keyboardFocusable=%4 help=\"%5\"")
                                .arg(tag)
                                .arg(enabled ? QStringLiteral("true") : QStringLiteral("false"))
                                .arg(offscreen ? QStringLiteral("true") : QStringLiteral("false"))
                                .arg(keyboard ? QStringLiteral("true") : QStringLiteral("false"))
                                .arg(bstrToQ(help));
    qWarning().noquote() << QStringLiteral("[EPIC][%1]   rect=(%2,%3)-(%4,%5) size=%6x%7")
                                .arg(tag)
                                .arg(rect.left).arg(rect.top).arg(rect.right).arg(rect.bottom)
                                .arg(rect.right - rect.left).arg(rect.bottom - rect.top);

    if (name) SysFreeString(name);
    if (autoId) SysFreeString(autoId);
    if (cls) SysFreeString(cls);
    if (help) SysFreeString(help);
    if (loc) SysFreeString(loc);
}

// Win32 children часто пустые у Epic — дополнительно UI Automation (кнопки/поля).
static void dumpAccessibleControls(HWND hwnd, const QString &tag)
{
    qWarning().noquote() << "[EPIC] ===== CONTROLS DUMP:" << tag << "=====";

    // 1) Все Win32 child HWND (в т.ч. невидимые)
    struct ChildDump { int n = 0; } cd;
    EnumChildWindows(hwnd, [](HWND ch, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<ChildDump *>(lp);
        if (c->n >= 80)
            return FALSE;
        wchar_t ct[256] = {}, cc[128] = {};
        GetWindowTextW(ch, ct, 255);
        GetClassNameW(ch, cc, 127);
        RECT r{};
        GetWindowRect(ch, &r);
        const LONG style = GetWindowLongW(ch, GWL_STYLE);
        const QString cname = QString::fromWCharArray(cc);
        const bool isBtn = cname.contains(QStringLiteral("Button"), Qt::CaseInsensitive);
        qWarning().noquote()
            << QStringLiteral("[EPIC][WIN32] #%1 hwnd=0x%2 class=\"%3\" title=\"%4\" "
                              "visible=%5 enabled=%6 style=0x%7 buttonish=%8 rect=%9x%10")
                   .arg(c->n)
                   .arg(reinterpret_cast<quintptr>(ch), 0, 16)
                   .arg(cname)
                   .arg(ct[0] ? QString::fromWCharArray(ct) : QStringLiteral("(empty)"))
                   .arg(IsWindowVisible(ch) ? QStringLiteral("true") : QStringLiteral("false"))
                   .arg(IsWindowEnabled(ch) ? QStringLiteral("true") : QStringLiteral("false"))
                   .arg(quint32(style), 0, 16)
                   .arg(isBtn ? QStringLiteral("true") : QStringLiteral("false"))
                   .arg(r.right - r.left)
                   .arg(r.bottom - r.top);
        ++c->n;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&cd));
    qWarning().noquote() << "[EPIC][WIN32] total children:" << cd.n;

    // 2) UI Automation — Button / Edit / Hyperlink / Image (соцкнопки)
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool didInit = (hrInit == S_OK);

    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        qWarning() << "[EPIC][UIA] CoCreateInstance failed" << Qt::hex << quint32(hr);
        if (didInit)
            CoUninitialize();
        qWarning().noquote() << "[EPIC] ===== CONTROLS DUMP END =====";
        return;
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        qWarning() << "[EPIC][UIA] ElementFromHandle failed" << Qt::hex << quint32(hr);
        automation->Release();
        if (didInit)
            CoUninitialize();
        qWarning().noquote() << "[EPIC] ===== CONTROLS DUMP END =====";
        return;
    }

    BSTR rootName = nullptr;
    root->get_CurrentName(&rootName);
    qWarning().noquote() << "[EPIC][UIA] root name:" << bstrToQ(rootName);
    if (rootName)
        SysFreeString(rootName);

    // Сначала — ВСЁ дерево без фильтра (если тут 0/1, accessibility у Epic выключен)
    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    int rawCount = 0;
    if (trueCond) {
        IUIAutomationElementArray *all = nullptr;
        hr = root->FindAll(TreeScope_Descendants, trueCond, &all);
        trueCond->Release();
        if (SUCCEEDED(hr) && all) {
            int len = 0;
            all->get_Length(&len);
            qWarning().noquote() << "[EPIC][UIA] ALL descendants (any type):" << len;
            for (int i = 0; i < len && i < 80; ++i) {
                IUIAutomationElement *el = nullptr;
                if (FAILED(all->GetElement(i, &el)) || !el)
                    continue;
                dumpUiaElement(el, rawCount++, QStringLiteral("ALL"));
                el->Release();
            }
            if (len > 80)
                qWarning().noquote() << "[EPIC][UIA] ... truncated, total" << len;
            all->Release();
        }
    }

    const CONTROLTYPEID interesting[] = {
        UIA_ButtonControlTypeId,
        UIA_EditControlTypeId,
        UIA_HyperlinkControlTypeId,
        UIA_ImageControlTypeId,
        UIA_CustomControlTypeId,
        UIA_CheckBoxControlTypeId,
        UIA_ComboBoxControlTypeId,
        UIA_TextControlTypeId,
    };

    int total = 0;
    for (CONTROLTYPEID typeId : interesting) {
        VARIANT var{};
        var.vt = VT_I4;
        var.lVal = typeId;

        IUIAutomationCondition *cond = nullptr;
        hr = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond);
        if (FAILED(hr) || !cond)
            continue;

        IUIAutomationElementArray *arr = nullptr;
        hr = root->FindAll(TreeScope_Descendants, cond, &arr);
        cond->Release();
        if (FAILED(hr) || !arr)
            continue;

        int len = 0;
        arr->get_Length(&len);
        qWarning().noquote() << "[EPIC][UIA] found" << len << uiaTypeName(typeId);
        for (int i = 0; i < len && i < 60; ++i) {
            IUIAutomationElement *el = nullptr;
            if (FAILED(arr->GetElement(i, &el)) || !el)
                continue;
            dumpUiaElement(el, total++, uiaTypeName(typeId));
            el->Release();
        }
        arr->Release();
    }

    if (rawCount == 0 && total == 0) {
        qWarning().noquote() << "[EPIC][UIA] пусто: Epic рисует логин сам (Unreal/CEF),";
        qWarning().noquote() << "[EPIC][UIA] это НЕ Qt и не наш процесс — findChildren<QPushButton*> тут не работает.";
    }

    root->Release();
    automation->Release();
    if (didInit)
        CoUninitialize();

    qWarning().noquote() << "[EPIC] ===== CONTROLS DUMP END (raw=" << rawCount
                         << " interesting=" << total << ") =====";
}

static QString styleFlags(LONG style)
{
    QStringList f;
    if (style & WS_VISIBLE) f << QStringLiteral("VISIBLE");
    if (style & WS_DISABLED) f << QStringLiteral("DISABLED");
    if (style & WS_POPUP) f << QStringLiteral("POPUP");
    if (style & WS_CHILD) f << QStringLiteral("CHILD");
    if (style & WS_OVERLAPPEDWINDOW) f << QStringLiteral("OVERLAPPEDWINDOW");
    if (style & WS_CAPTION) f << QStringLiteral("CAPTION");
    if (style & WS_SYSMENU) f << QStringLiteral("SYSMENU");
    if (style & WS_MINIMIZEBOX) f << QStringLiteral("MINIMIZEBOX");
    if (style & WS_MAXIMIZEBOX) f << QStringLiteral("MAXIMIZEBOX");
    if (style & WS_THICKFRAME) f << QStringLiteral("THICKFRAME");
    if (style & WS_CLIPSIBLINGS) f << QStringLiteral("CLIPSIBLINGS");
    if (style & WS_CLIPCHILDREN) f << QStringLiteral("CLIPCHILDREN");
    return f.join(QLatin1Char('|'));
}

static QString exStyleFlags(LONG ex)
{
    QStringList f;
    if (ex & WS_EX_TOPMOST) f << QStringLiteral("TOPMOST");
    if (ex & WS_EX_TOOLWINDOW) f << QStringLiteral("TOOLWINDOW");
    if (ex & WS_EX_APPWINDOW) f << QStringLiteral("APPWINDOW");
    if (ex & WS_EX_LAYERED) f << QStringLiteral("LAYERED");
    if (ex & WS_EX_TRANSPARENT) f << QStringLiteral("TRANSPARENT");
    if (ex & WS_EX_NOACTIVATE) f << QStringLiteral("NOACTIVATE");
    if (ex & WS_EX_WINDOWEDGE) f << QStringLiteral("WINDOWEDGE");
    if (ex & WS_EX_CLIENTEDGE) f << QStringLiteral("CLIENTEDGE");
    if (ex & WS_EX_COMPOSITED) f << QStringLiteral("COMPOSITED");
    if (ex & WS_EX_NOREDIRECTIONBITMAP) f << QStringLiteral("NOREDIRECTIONBITMAP");
    return f.join(QLatin1Char('|'));
}

static void dumpWindowFull(HWND hwnd, const QString &tag, int score = -1)
{
    if (!hwnd || !IsWindow(hwnd)) {
        qWarning().noquote() << "[EPIC]" << tag << "HWND invalid";
        return;
    }

    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);

    wchar_t wtitle[512] = {};
    GetWindowTextW(hwnd, wtitle, 511);
    const QString title = QString::fromWCharArray(wtitle);

    wchar_t wcls[256] = {};
    GetClassNameW(hwnd, wcls, 255);
    const QString cls = QString::fromWCharArray(wcls);

    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    const HWND owner = GetWindow(hwnd, GW_OWNER);
    const HWND parent = GetParent(hwnd);
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    const HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
    const HWND fg = GetForegroundWindow();

    wchar_t ownerTitle[256] = {};
    wchar_t parentTitle[256] = {};
    wchar_t fgTitle[256] = {};
    wchar_t ownerCls[128] = {};
    wchar_t parentCls[128] = {};
    if (owner) {
        GetWindowTextW(owner, ownerTitle, 255);
        GetClassNameW(owner, ownerCls, 127);
    }
    if (parent) {
        GetWindowTextW(parent, parentTitle, 255);
        GetClassNameW(parent, parentCls, 127);
    }
    if (fg)
        GetWindowTextW(fg, fgTitle, 255);

    DWORD ownerPid = 0, parentPid = 0;
    if (owner) GetWindowThreadProcessId(owner, &ownerPid);
    if (parent) GetWindowThreadProcessId(parent, &parentPid);

    const BOOL visible = IsWindowVisible(hwnd);
    const BOOL enabled = IsWindowEnabled(hwnd);
    const BOOL iconic = IsIconic(hwnd);
    const BOOL zoomed = IsZoomed(hwnd);
    const BOOL unicode = IsWindowUnicode(hwnd);

    BYTE alpha = 255;
    DWORD layerFlags = 0;
    COLORREF layerColor = 0;
    if (exStyle & WS_EX_LAYERED)
        GetLayeredWindowAttributes(hwnd, &layerColor, &alpha, &layerFlags);

    qWarning().noquote() << "[EPIC] -------- WINDOW DUMP:" << tag << "--------";
    qWarning().noquote() << "[EPIC] hwnd:" << QString::number(reinterpret_cast<quintptr>(hwnd), 16).prepend(QStringLiteral("0x"));
    qWarning().noquote() << "[EPIC] title:" << (title.isEmpty() ? QStringLiteral("(empty)") : title);
    qWarning().noquote() << "[EPIC] class:" << cls;
    qWarning().noquote() << "[EPIC] score:" << score;
    qWarning().noquote() << "[EPIC] pid:" << pid << processImageForPid(pid) << "| tid:" << tid;
    qWarning().noquote() << "[EPIC] windowRect:" << wr.left << wr.top << wr.right << wr.bottom
                         << QStringLiteral("(%1x%2)").arg(wr.right - wr.left).arg(wr.bottom - wr.top);
    qWarning().noquote() << "[EPIC] clientRect:" << cr.left << cr.top << cr.right << cr.bottom
                         << QStringLiteral("(%1x%2)").arg(cr.right - cr.left).arg(cr.bottom - cr.top);
    qWarning().noquote() << "[EPIC] style: 0x" + QString::number(quint32(style), 16) << styleFlags(style);
    qWarning().noquote() << "[EPIC] exStyle: 0x" + QString::number(quint32(exStyle), 16) << exStyleFlags(exStyle);
    qWarning().noquote() << "[EPIC] visible:" << visible << "| enabled:" << enabled
                         << "| iconic:" << iconic << "| zoomed:" << zoomed << "| unicode:" << unicode;
    if (exStyle & WS_EX_LAYERED)
        qWarning().noquote() << "[EPIC] layered alpha:" << int(alpha)
                             << "| flags:" << layerFlags << "| color:" << layerColor;
    qWarning().noquote() << "[EPIC] owner:" << QString::number(reinterpret_cast<quintptr>(owner), 16).prepend(QStringLiteral("0x"))
                         << "| pid:" << ownerPid
                         << "| class:" << QString::fromWCharArray(ownerCls)
                         << "| title:" << QString::fromWCharArray(ownerTitle);
    qWarning().noquote() << "[EPIC] parent:" << QString::number(reinterpret_cast<quintptr>(parent), 16).prepend(QStringLiteral("0x"))
                         << "| pid:" << parentPid
                         << "| class:" << QString::fromWCharArray(parentCls)
                         << "| title:" << QString::fromWCharArray(parentTitle);
    qWarning().noquote() << "[EPIC] root:" << QString::number(reinterpret_cast<quintptr>(root), 16).prepend(QStringLiteral("0x"))
                         << "| rootOwner:" << QString::number(reinterpret_cast<quintptr>(rootOwner), 16).prepend(QStringLiteral("0x"));
    qWarning().noquote() << "[EPIC] foreground:" << QString::number(reinterpret_cast<quintptr>(fg), 16).prepend(QStringLiteral("0x"))
                         << "| title:" << QString::fromWCharArray(fgTitle)
                         << "| isSelf:" << (fg == hwnd);

    qWarning().noquote() << "[EPIC] ------------------------------------";
}

struct EpicLoginEnumCtx {
    HWND best = nullptr;
    QString title;
    int score = -1;
    QSet<DWORD> epicPids;
    QStringList dump;
    bool doDump = false;
};

static bool isEpicLauncherMainTitle(const QString &t)
{
    return t.contains(QStringLiteral("Программа запуска"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Epic Games Launcher"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("EpicGamesLauncher"), Qt::CaseInsensitive);
}

static bool isEpicLoginTitle(const QString &t)
{
    if (t.isEmpty())
        return false;
    const QString low = t.toLower();
    // CEF / portal: "Sign in", "Sign in to your Epic Games account", "Войти", …
    if (low.contains(QStringLiteral("sign in"))
        || low.contains(QStringLiteral("log in"))
        || low.contains(QStringLiteral("login"))
        || low.contains(QStringLiteral("войти"))
        || low.contains(QStringLiteral("учётн"))
        || low.contains(QStringLiteral("учетн"))
        || low.contains(QStringLiteral("epic games account"))
        || low.contains(QStringLiteral("your epic")))
        return true;
    // "Account" alone is weak; require sign/log context
    if (low.contains(QStringLiteral("account"))
        && (low.contains(QStringLiteral("sign"))
            || low.contains(QStringLiteral("log"))
            || low.contains(QStringLiteral("войти"))))
        return true;
    return false;
}

static void considerLoginCandidate(EpicLoginEnumCtx *c, HWND h, const QString &t,
                                   const QString &cls, int w, int hgt, DWORD pid, bool fromEpic)
{
    const bool chrome = cls.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("Chrome_RenderWidget"), Qt::CaseInsensitive)
                        || cls.contains(QStringLiteral("Intermediate D3D"), Qt::CaseInsensitive);
    // В новых Epic форма «Войти» рисуется ВНУТРИ UnrealWindow лаунчера — отдельного HWND нет
    const bool unrealLauncher = fromEpic
                                && cls.contains(QStringLiteral("UnrealWindow"), Qt::CaseInsensitive)
                                && (isEpicLauncherMainTitle(t)
                                    || t.contains(QStringLiteral("Epic Games"), Qt::CaseInsensitive));

    const bool titleOk = isEpicLoginTitle(t);
    if (!fromEpic && !titleOk)
        return;

    int score = 0;
    if (fromEpic) score += 20;
    if (titleOk) score += 60;
    if (t.contains(QStringLiteral("Войти"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("Sign"), Qt::CaseInsensitive))
        score += 40;
    if (chrome) score += 50;
    if (unrealLauncher) score += 70; // единственное реальное окно логина на многих ПК
    if (w >= 360 && w <= 920 && hgt >= 420 && hgt <= 1000)
        score += 35;
    else if (w >= 1000 && hgt >= 600 && unrealLauncher)
        score += 15; // большой лаунчер с формой по центру
    else if (w >= 280 && hgt >= 280)
        score += 5;

    if (!chrome && !titleOk && !unrealLauncher)
        return;
    if (score < 60)
        return;

    if (score > c->score) {
        c->score = score;
        c->best = h;
        c->title = (t.isEmpty() ? QStringLiteral("(no title)") : t)
                   + QStringLiteral(" [") + cls + QLatin1Char(' ')
                   + QString::number(w) + QLatin1Char('x') + QString::number(hgt)
                   + QStringLiteral(" pid=") + QString::number(pid)
                   + QStringLiteral(" score=") + QString::number(score)
                   + (unrealLauncher ? QStringLiteral(" UNREAL-LOGIN") : QString())
                   + QLatin1Char(']');
    }
}

static BOOL CALLBACK enumEpicChildProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<EpicLoginEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    const bool fromEpic = c->epicPids.contains(pid);

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString cls = QString::fromWCharArray(wcls);

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    if (w < 200 || hgt < 200)
        return TRUE;

    if (c->doDump && fromEpic) {
        c->dump << QStringLiteral("CHILD pid=%1 %2 | \"%3\" | %4 %5x%6")
                       .arg(pid)
                       .arg(processImageForPid(pid))
                       .arg(t.isEmpty() ? QStringLiteral("(no title)") : t)
                       .arg(cls)
                       .arg(w)
                       .arg(hgt);
    }

    considerLoginCandidate(c, h, t, cls, w, hgt, pid, fromEpic);
    return TRUE;
}

static BOOL CALLBACK enumEpicLoginProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<EpicLoginEnumCtx *>(lp);

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    const bool fromEpic = c->epicPids.contains(pid);

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString cls = QString::fromWCharArray(wcls);
    if (cls.startsWith(QStringLiteral("Qt"), Qt::CaseInsensitive))
        return TRUE;

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    const bool offscreen = (rc.left <= -5000 || rc.top <= -5000);
    const bool visible = IsWindowVisible(h);

    if (c->doDump && visible && !offscreen && (fromEpic || isEpicLoginTitle(t)
                                              || cls.contains(QStringLiteral("Chrome"), Qt::CaseInsensitive))) {
        c->dump << QStringLiteral("TOP pid=%1 %2 | \"%3\" | %4 %5x%6")
                       .arg(pid)
                       .arg(processImageForPid(pid))
                       .arg(t.isEmpty() ? QStringLiteral("(no title)") : t)
                       .arg(cls)
                       .arg(w)
                       .arg(hgt);
    }

    // Обходим детей Epic-окон — логин часто внутри CEF
    if (fromEpic && visible)
        EnumChildWindows(h, enumEpicChildProc, lp);

    if (!offscreen) {
        if (!visible)
            return TRUE;
        if (w < 280 || hgt < 280)
            return TRUE;
    }

    considerLoginCandidate(c, h, t, cls, w, hgt, pid, fromEpic);
    return TRUE;
}
#endif

EpicAuth::EpicAuth(QObject *parent)
    : IPlatformAuth(parent)
{
}

EpicAuth::~EpicAuth()
{
    stopScout();
}

void EpicAuth::silentKill(const QString &image)
{
#ifdef Q_OS_WIN
    auto *p = new QProcess;
    p->setProgram(QStringLiteral("taskkill"));
    p->setArguments({QStringLiteral("/F"), QStringLiteral("/T"),
                     QStringLiteral("/IM"), image});
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     p, &QObject::deleteLater);
    p->start();
#else
    Q_UNUSED(image);
#endif
}

void EpicAuth::bumpLaunchGen()
{
    ++m_launchGen;
    m_silentRelaunchCount = 0;
}

void EpicAuth::killLauncher()
{
    bumpLaunchGen();
    stopScout();
    qWarning() << "[EPIC] killLauncher: taskkill Epic + WebHelper";
    silentKill(QStringLiteral("EpicGamesLauncher.exe"));
    silentKill(QStringLiteral("EpicWebHelper.exe"));
    silentKill(QStringLiteral("EpicGamesLauncher-Win64-Shipping.exe"));
}

static bool epicImageRunning(const QString &image)
{
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (QString::fromWCharArray(pe.szExeFile).compare(image, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    Q_UNUSED(image);
    return false;
#endif
}

static void killEpicAndWait(EpicAuth *self, int timeoutMs)
{
    if (!self)
        return;
    self->killLauncher();
#ifdef Q_OS_WIN
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const bool alive = epicImageRunning(QStringLiteral("EpicGamesLauncher.exe"))
                           || epicImageRunning(QStringLiteral("EpicWebHelper.exe"))
                           || epicImageRunning(QStringLiteral("EpicGamesLauncher-Win64-Shipping.exe"));
        if (!alive) {
            qWarning() << "[EPIC] killAndWait: процессы Epic завершены";
            QThread::msleep(300);
            return;
        }
        self->killLauncher();
        QThread::msleep(250);
    }
    qWarning() << "[EPIC] WARN: killAndWait timeout — wipe всё равно";
#else
    Q_UNUSED(timeoutMs);
#endif
}

static bool stripEpicRememberMeFile(const QString &path, QStringList *cleared)
{
    if (!QFileInfo::exists(path))
        return false;
    QString body = readTextFile(path);
    if (body.trimmed().isEmpty())
        return false;
    static const QRegularExpression reSection(
        QStringLiteral("(?ms)^\\[RememberMe\\].*?(?=^\\[|\\z)"));
    const QString before = body;
    body.replace(reSection, QStringLiteral("[RememberMe]\nEnable=False\n\n"));
    if (!body.contains(QStringLiteral("[RememberMe]"), Qt::CaseInsensitive))
        body.append(QStringLiteral("\n[RememberMe]\nEnable=False\n"));
    if (body == before)
        return false;
    if (writeTextFile(path, body)) {
        if (cleared)
            *cleared << (QStringLiteral("RememberMe:") + path);
        return true;
    }
    if (cleared)
        *cleared << (QStringLiteral("RememberMe:FAILED:") + path);
    return false;
}

static void clearEpicLocalSession(const QString &launcherExe)
{
    QStringList cleared;
    stripEpicRememberMeFile(epicGameUserSettingsPath(launcherExe), &cleared);
    if (!cleared.isEmpty())
        qWarning() << "[EPIC] cleared RememberMe →" << cleared;
}

static void wipeEpicPersonalSession(const QString &launcherExe)
{
    QStringList cleared;

    // Все известные GameUserSettings.ini — снести [RememberMe]
    QSet<QString> paths;
    for (const QString &p : epicGameUserSettingsCandidates(launcherExe))
        paths.insert(p);
    const QString found = findEpicRememberFile(launcherExe);
    if (!found.isEmpty())
        paths.insert(found);
    for (const QString &p : paths)
        stripEpicRememberMeFile(p, &cleared);

    // CEF / login persist рядом с лаунчером
    const QString saved = epicLocalAppData() + QStringLiteral("/EpicGamesLauncher/Saved");
    const QStringList wipeDirs = {
        QStringLiteral("Data"),
        QStringLiteral("webcache"),
        QStringLiteral("webcache_4430"),
        QStringLiteral("webcache_4431"),
        QStringLiteral("Cookies"),
        QStringLiteral("Local Storage"),
        QStringLiteral("Session Storage"),
        QStringLiteral("Service Worker"),
    };
    for (const QString &sub : wipeDirs) {
        const QString abs = saved + QLatin1Char('/') + sub;
        if (!QDir(abs).exists())
            continue;
        if (QDir(abs).removeRecursively())
            cleared << (QStringLiteral("dir:") + sub);
        else
            cleared << (QStringLiteral("dir:FAILED:") + sub);
    }

    qWarning().noquote() << "[EPIC] personal: full logout wipe |"
                         << (cleared.isEmpty() ? QStringLiteral("(nothing found)")
                                               : cleared.join(QStringLiteral(", ")));
}

bool EpicAuth::applyCache(const QJsonObject &authData)
{
    const QString login = authData.value(QStringLiteral("login")).toString();
    const QString password = authData.value(QStringLiteral("password")).toString();
    const QString mode = authData.value(QStringLiteral("auth")).toObject()
                             .value(QStringLiteral("mode")).toString();
    const QString platformSource = authData.value(QStringLiteral("platform_source")).toString();
    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }
    if (!exe.isEmpty())
        m_launcherExe = exe;

    const bool personal = (mode == QLatin1String("personal"))
        || (platformSource.compare(QStringLiteral("personal_account"), Qt::CaseInsensitive) == 0)
        || login.trimmed().isEmpty() || password.isEmpty();
    if (personal) {
        qWarning() << "[EPIC] applyCache: personal — kill Epic + full logout wipe";
        killEpicAndWait(this, 8000);
        wipeEpicPersonalSession(m_launcherExe);
        m_expectInteractive = false;
        m_allowsGameDetect = true;
        m_needBackup = false;
        return true;
    }

    const QJsonObject vdf = extractVdfFiles(authData);

    // Переиспользуем поля machine-cache: local_vdf = GameUserSettings.ini (RememberMe)
    QString settings = vdf.value(QStringLiteral("local_vdf")).toString();
    if (settings.trimmed().isEmpty())
        settings = vdf.value(QStringLiteral("config_vdf")).toString();

    const bool hasRemember = settings.contains(QStringLiteral("[RememberMe]"), Qt::CaseInsensitive)
                             && (settings.contains(QStringLiteral("Data="), Qt::CaseInsensitive)
                                 || settings.contains(QStringLiteral("Enable=True"), Qt::CaseInsensitive));

    if (!settings.trimmed().isEmpty() && hasRemember) {
        const QString path = epicGameUserSettingsPath(m_launcherExe);
        if (writeTextFile(path, settings)) {
            qWarning() << "[EPIC] applyCache: восстановлен GameUserSettings.ini ("
                       << settings.size() << "chars) →" << path;
            m_expectInteractive = false;
            m_allowsGameDetect = true;
            m_needBackup = false;
            return true;
        }
        qWarning() << "[EPIC] applyCache: не удалось записать" << path;
    } else {
        qWarning() << "[EPIC] applyCache: нет machine-cache RememberMe для" << login
                   << "| local_vdf chars:" << settings.size();
    }

    m_expectInteractive = true;
    m_allowsGameDetect = false;
    m_needBackup = true; // обязательно сохранить ini после сессии
    return false;
}

void EpicAuth::startLauncher(QProcess *process,
                             const QJsonObject &authData,
                             const QString &appIdHint)
{
    Q_UNUSED(appIdHint);
    if (!process)
        return;

    bumpLaunchGen();

    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }

    QString argsStr = authData.value(QStringLiteral("args")).toString().trimmed();
    if (argsStr.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        argsStr = launcher.value(QStringLiteral("args")).toString().trimmed();
    }

    if (exe.isEmpty()) {
        qCritical() << "[EPIC] exe_path пуст";
        return;
    }

    const QFileInfo fi(exe);
    process->setWorkingDirectory(fi.absolutePath());
    m_launcherExe = exe;
    m_launchUri.clear();
    if (argsStr.startsWith(QStringLiteral("com.epicgames.launcher://"), Qt::CaseInsensitive))
        m_launchUri = argsStr;

    const QString login = authData.value(QStringLiteral("login")).toString();
    const QString password = authData.value(QStringLiteral("password")).toString();
    // Только клубный cache/silent: без URI на старте — RememberMe ещё не готов.
    // Personal / interactive: URI сразу (как раньше).
    const bool clubSilent = !m_expectInteractive
                            && !login.trimmed().isEmpty()
                            && !password.isEmpty();

    QStringList args;
    if (!m_launchUri.isEmpty() && clubSilent) {
        qWarning() << "[EPIC] silent: стартуем лаунчер без product URL (ждём ready)";
    } else if (!m_launchUri.isEmpty()) {
        args << m_launchUri;
    } else if (!argsStr.isEmpty()) {
        args = QProcess::splitCommand(argsStr);
    }

    qWarning().noquote() << "[EPIC] Launch exe:" << exe;
    qWarning().noquote() << "[EPIC] Launch args:" << args;
    if (!m_launchUri.isEmpty())
        qWarning().noquote() << "[EPIC] product URI (deferred if silent):" << m_launchUri;
    process->start(exe, args);
}

void EpicAuth::stopScout()
{
    if (!m_scoutTimer)
        return;
    m_scoutTimer->stop();
    m_scoutTimer->deleteLater();
    m_scoutTimer = nullptr;
}

#ifdef Q_OS_WIN
static bool epicNameIsContinue(const QString &n)
{
    return n.contains(QStringLiteral("continue"), Qt::CaseInsensitive)
           || n.contains(QStringLiteral("продолжить"), Qt::CaseInsensitive);
}
#endif

void EpicAuth::injectEmail(quintptr hwndVal, const QString &email)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return;

    m_loginHwnd = hwndVal;
    m_phase = Phase::EmailSubmitted;
    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->requestClearGameSearch();
    setShellTopmostFrom(this, true);

    // One continuous keyboard sequence; SetForeground once at phase start.
    QTimer::singleShot(400, this, [this, hwndVal, email]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;

        if (auto *pm = qobject_cast<ProcessManager *>(parent()))
            pm->requestClearGameSearch();
        qWarning() << "[EPIC] scout:1 email phase — SetForeground once (under overlay)";
        placeWindowForInput(h, this);
        Sleep(120);

        sendTabs(2, "→ email field (Tab index 2)");
        Sleep(80);
        qWarning() << "[EPIC] scout:1b type email (Ctrl+A)";
        clearAndType(email);
        Sleep(150);

        // Focus still in email (index 2) → Tab×2 more → Continue (index 4)
        sendTabs(2, "→ Continue/Продолжить (Tab index 4)");
        Sleep(80);
        sendReturnKey();
        qWarning() << "[EPIC] scout:1c Enter on Continue/Продолжить";

        m_phase = Phase::WaitPasswordDialog;
        m_phaseTick = m_ticks;
        qWarning() << "[EPIC] scout:2 wait password window (~1–2s)";
    });
#else
    Q_UNUSED(hwndVal);
    Q_UNUSED(email);
#endif
}

void EpicAuth::injectPassword(quintptr hwndVal, const QString &password)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return;

    if (auto *pm = qobject_cast<ProcessManager *>(parent()))
        pm->requestClearGameSearch();
    setShellTopmostFrom(this, true);

    // Keyboard-only: Tab×3 → password → Tab×3 → Войти. No UIA/mouse mid-sequence.
    QTimer::singleShot(300, this, [this, hwndVal, password]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;

        if (auto *pm = qobject_cast<ProcessManager *>(parent()))
            pm->requestClearGameSearch();
        qWarning() << "[EPIC] scout:3 password phase — SetForeground once (under overlay)";
        placeWindowForInput(h, this);
        Sleep(120);

        sendTabs(3, "→ password field (Tab index 3)");
        Sleep(80);
        qWarning() << "[EPIC] scout:3b type password (Ctrl+A)";
        clearAndType(password);
        Sleep(150);

        // Focus still in password (index 3) → Tab×3 more → Войти (index 6)
        sendTabs(3, "→ Sign in/Войти (Tab index 6)");
        Sleep(80);
        sendReturnKey();
        qWarning() << "[EPIC] scout:3c Enter on Войти";

        m_needBackup = true;
        m_phase = Phase::PasswordSubmitted;
        m_phaseTick = m_ticks;
        m_allowsGameDetect = true;
        qWarning() << "[EPIC] scout:3d password submitted — game detect on";
    });
#else
    Q_UNUSED(hwndVal);
    Q_UNUSED(password);
#endif
}

void EpicAuth::relaunchGameUri()
{
#ifdef Q_OS_WIN
    if (m_launchUri.isEmpty()) {
        qWarning() << "[EPIC] relaunch: URI пуст";
        return;
    }
    ++m_silentRelaunchCount;
    const HINSTANCE r = ShellExecuteW(
        nullptr, L"open",
        reinterpret_cast<LPCWSTR>(m_launchUri.utf16()),
        nullptr, nullptr, SW_SHOWNORMAL);
    qWarning() << "[EPIC] relaunch game URI #" << m_silentRelaunchCount
               << "result:" << int(reinterpret_cast<quintptr>(r))
               << m_launchUri;
#else
    Q_UNUSED(m_launchUri);
#endif
}

#ifdef Q_OS_WIN
struct EpicLoginUiProbe {
    bool loginByTitle = false;
    bool hasEdit = false;
    bool hasPasswordEdit = false;
    bool hasContinue = false;
    bool hasSignIn = false;
    bool hasLoginWord = false;
    int uiaDescendants = 0;
    int edits = 0;
    QString hitDetail;

    bool detected() const
    {
        return loginByTitle || hasPasswordEdit || hasContinue || hasSignIn
               || (hasLoginWord && edits > 0) || (hasEdit && hasLoginWord);
    }
};

struct EpicReadyStatus {
    bool mainWindow = false;
    bool loginUi = false;
    bool libraryLikely = false;
    bool uiaReachable = false; // scanned tree had descendants
    bool confidentLoggedIn = false;
    QString bestTitle;
    QString bestClass;
    int bestW = 0;
    int bestH = 0;
    EpicLoginUiProbe probe;
    QString why;
};

static bool epicTextLooksLoginUi(const QString &n)
{
    if (n.isEmpty())
        return false;
    const QString t = n.trimmed();
    if (t.contains(QStringLiteral("continue"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("продолжить"), Qt::CaseInsensitive))
        return true;
    if (t.contains(QStringLiteral("sign in"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("log in"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("войти"), Qt::CaseInsensitive))
        return true;
    if (t.contains(QStringLiteral("email"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("e-mail"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("почт"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("username"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("имя пользовател"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("учётн"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("учетн"), Qt::CaseInsensitive))
        return true;
    if (t.contains(QStringLiteral("password"), Qt::CaseInsensitive)
        || t.contains(QStringLiteral("парол"), Qt::CaseInsensitive))
        return true;
    if (t.contains(QStringLiteral("epic games account"), Qt::CaseInsensitive))
        return true;
    return false;
}

static bool epicNameIsSignIn(const QString &n)
{
    const QString t = n.trimmed();
    if (t.isEmpty())
        return false;
    return t.contains(QStringLiteral("sign in"), Qt::CaseInsensitive)
           || t.compare(QStringLiteral("войти"), Qt::CaseInsensitive) == 0
           || t.startsWith(QStringLiteral("войти"), Qt::CaseInsensitive)
           || (t.contains(QStringLiteral("войти"), Qt::CaseInsensitive)
               && t.size() < 40);
}

// UIA probe: edit/password + Continue/Продолжить/Sign in/Войти inside Unreal/CEF.
static EpicLoginUiProbe epicProbeHwndLoginUi(HWND hwnd)
{
    EpicLoginUiProbe p;
    if (!hwnd || !IsWindow(hwnd))
        return p;

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool didInit = (hrInit == S_OK);

    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        if (didInit)
            CoUninitialize();
        return p;
    }

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        automation->Release();
        if (didInit)
            CoUninitialize();
        return p;
    }

    IUIAutomationCondition *trueCond = nullptr;
    automation->CreateTrueCondition(&trueCond);
    if (trueCond) {
        IUIAutomationElementArray *all = nullptr;
        hr = root->FindAll(TreeScope_Descendants, trueCond, &all);
        trueCond->Release();
        if (SUCCEEDED(hr) && all) {
            int len = 0;
            all->get_Length(&len);
            p.uiaDescendants = len;
            for (int i = 0; i < len && i < 160; ++i) {
                IUIAutomationElement *el = nullptr;
                if (FAILED(all->GetElement(i, &el)) || !el)
                    continue;
                CONTROLTYPEID ctype = 0;
                el->get_CurrentControlType(&ctype);
                BSTR name = nullptr, autoId = nullptr, help = nullptr;
                el->get_CurrentName(&name);
                el->get_CurrentAutomationId(&autoId);
                el->get_CurrentHelpText(&help);
                const QString n = bstrToQ(name);
                const QString aid = bstrToQ(autoId);
                const QString hlp = bstrToQ(help);
                if (name) SysFreeString(name);
                if (autoId) SysFreeString(autoId);
                if (help) SysFreeString(help);

                BOOL enabled = FALSE, offscreen = FALSE;
                el->get_CurrentIsEnabled(&enabled);
                el->get_CurrentIsOffscreen(&offscreen);
                RECT rect{};
                el->get_CurrentBoundingRectangle(&rect);
                const bool onScreen = enabled && !offscreen
                                      && rect.right > rect.left && rect.bottom > rect.top;

                if (epicTextLooksLoginUi(n) || epicTextLooksLoginUi(aid) || epicTextLooksLoginUi(hlp))
                    p.hasLoginWord = true;

                if (ctype == UIA_EditControlTypeId) {
                    ++p.edits;
                    p.hasEdit = true;
                    BOOL isPwd = FALSE;
                    el->get_CurrentIsPassword(&isPwd);
                    if (isPwd) {
                        p.hasPasswordEdit = true;
                        if (p.hitDetail.isEmpty())
                            p.hitDetail = QStringLiteral("password-edit");
                    } else if (p.hitDetail.isEmpty() && p.hasLoginWord)
                        p.hitDetail = QStringLiteral("edit+loginWord");
                }

                if (ctype == UIA_ButtonControlTypeId && onScreen) {
                    if (epicNameIsContinue(n)
                        || aid.contains(QStringLiteral("continue"), Qt::CaseInsensitive)) {
                        p.hasContinue = true;
                        if (p.hitDetail.isEmpty())
                            p.hitDetail = QStringLiteral("btn:") + n;
                    }
                    if (epicNameIsSignIn(n)
                        || aid.contains(QStringLiteral("sign"), Qt::CaseInsensitive)) {
                        p.hasSignIn = true;
                        if (p.hitDetail.isEmpty())
                            p.hitDetail = QStringLiteral("btn:") + n;
                    }
                }

                el->Release();
            }
            all->Release();
        }
    }

    root->Release();
    automation->Release();
    if (didInit)
        CoUninitialize();
    return p;
}

struct EpicReadyEnumCtx {
    bool mainWindow = false;
    bool loginWindow = false;
    QSet<DWORD> epicPids;
    QList<HWND> epicHwnds;
    QString bestTitle;
    QString bestClass;
    int bestArea = 0;
    int bestW = 0;
    int bestH = 0;
};

static BOOL CALLBACK enumEpicReadyProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<EpicReadyEnumCtx *>(lp);
    if (!IsWindowVisible(h))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!c->epicPids.contains(pid))
        return TRUE;

    wchar_t wtitle[512] = {};
    GetWindowTextW(h, wtitle, 511);
    const QString t = QString::fromWCharArray(wtitle).trimmed();

    wchar_t wcls[256] = {};
    GetClassNameW(h, wcls, 255);
    const QString cls = QString::fromWCharArray(wcls);

    RECT rc{};
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int hgt = rc.bottom - rc.top;
    if (w < 280 || hgt < 280)
        return TRUE;

    c->epicHwnds.append(h);

    const int area = w * hgt;
    if (area > c->bestArea) {
        c->bestArea = area;
        c->bestTitle = t.isEmpty() ? QStringLiteral("(empty)") : t;
        c->bestClass = cls;
        c->bestW = w;
        c->bestH = hgt;
    }

    if (isEpicLoginTitle(t))
        c->loginWindow = true;
    // Portal/Unreal: main chrome often titled "Epic Games" / "Epic Games Launcher"
    if (isEpicLauncherMainTitle(t)
        || (cls.contains(QStringLiteral("UnrealWindow"), Qt::CaseInsensitive)
            && t.contains(QStringLiteral("Epic"), Qt::CaseInsensitive))
        || (cls.contains(QStringLiteral("Chrome_WidgetWin"), Qt::CaseInsensitive)
            && t.contains(QStringLiteral("Epic"), Qt::CaseInsensitive)
            && !isEpicLoginTitle(t)))
        c->mainWindow = true;
    return TRUE;
}

static EpicReadyStatus epicProbeLauncherReady()
{
    EpicReadyStatus st;
    EpicReadyEnumCtx ctx;
    ctx.epicPids = collectEpicPids();
    if (ctx.epicPids.isEmpty()) {
        st.why = QStringLiteral("no-epic-pids");
        return st;
    }
    EnumWindows(enumEpicReadyProc, reinterpret_cast<LPARAM>(&ctx));

    st.mainWindow = ctx.mainWindow;
    st.bestTitle = ctx.bestTitle;
    st.bestClass = ctx.bestClass;
    st.bestW = ctx.bestW;
    st.bestH = ctx.bestH;
    st.probe.loginByTitle = ctx.loginWindow;

    EpicLoginUiProbe merged = st.probe;
    for (HWND h : ctx.epicHwnds) {
        const EpicLoginUiProbe one = epicProbeHwndLoginUi(h);
        merged.uiaDescendants = qMax(merged.uiaDescendants, one.uiaDescendants);
        merged.edits += one.edits;
        merged.hasEdit = merged.hasEdit || one.hasEdit;
        merged.hasPasswordEdit = merged.hasPasswordEdit || one.hasPasswordEdit;
        merged.hasContinue = merged.hasContinue || one.hasContinue;
        merged.hasSignIn = merged.hasSignIn || one.hasSignIn;
        merged.hasLoginWord = merged.hasLoginWord || one.hasLoginWord;
        if (merged.hitDetail.isEmpty() && !one.hitDetail.isEmpty())
            merged.hitDetail = one.hitDetail;
    }
    // Any visible edit/password on Epic during silent wait ≈ login (library search is rarer
    // on Portal splash; password/Continue/SignIn are decisive).
    if (merged.hasEdit && !merged.hasPasswordEdit && !merged.hasContinue
        && !merged.hasSignIn && !merged.hasLoginWord && !merged.loginByTitle) {
        // bare search-like edit without login words — do not treat as login alone
    } else if (merged.hasEdit || merged.hasPasswordEdit) {
        // edit + anything login-ish already covered by detected(); password alone = login
    }
    st.probe = merged;
    st.loginUi = merged.detected();
    st.uiaReachable = merged.uiaDescendants > 2;

    const bool largeMain = st.mainWindow && st.bestW >= 900 && st.bestH >= 560;
    st.libraryLikely = largeMain && !st.loginUi && st.uiaReachable
                       && !merged.hasPasswordEdit && !merged.hasContinue && !merged.hasSignIn;

    // Confident only when UIA sees a real tree AND login controls are gone.
    // Empty UIA (CEF/Unreal) while title looks like main = FALSE-POSITIVE ready (old bug).
    st.confidentLoggedIn = st.libraryLikely;

    if (!st.mainWindow)
        st.why = QStringLiteral("no-main-window");
    else if (st.loginUi)
        st.why = QStringLiteral("loginUi:") + (merged.hitDetail.isEmpty()
                    ? (merged.loginByTitle ? QStringLiteral("title") : QStringLiteral("uia"))
                    : merged.hitDetail);
    else if (!st.uiaReachable)
        st.why = QStringLiteral("uia-empty-uncertain");
    else if (!st.libraryLikely)
        st.why = QStringLiteral("not-library-yet");
    else
        st.why = QStringLiteral("library-ok");

    return st;
}
#endif

void EpicAuth::fallbackSilentToScout()
{
    const QString login = m_pendingLogin;
    const QString password = m_pendingPassword;
    qWarning() << "[EPIC] silent: login UI still up → fallback scout"
               << "| login:" << login
               << "| hasPassword:" << !password.isEmpty();
    stopScout();
    m_expectInteractive = true;
    m_allowsGameDetect = false;
    m_needBackup = true;
    m_silentRelaunchCount = 0;
    m_silentUriFirstTick = 0;
    m_silentConfidentTicks = 0;
    m_silentSawLibrary = false;
    startScout(login, password);
}

void EpicAuth::scheduleSilentRelaunch()
{
#ifdef Q_OS_WIN
    stopScout();
    m_ticks = 0;
    m_silentRelaunchCount = 0;
    m_silentUriFirstTick = 0;
    m_silentConfidentTicks = 0;
    m_silentSawLibrary = false;
    // Пока не подтвердили ready — игру не принимаем (login UI мог остаться)
    m_allowsGameDetect = false;

    if (m_launchUri.isEmpty()) {
        qWarning() << "[EPIC] silent: product URI пуст — только ждём игру";
        m_allowsGameDetect = true;
        return;
    }

    const int gen = m_launchGen;
    qWarning() << "[EPIC] silent: wait ready (strict) → product URL only if library confirmed";

    m_scoutTimer = new QTimer(this);
    m_scoutTimer->setInterval(500);
    connect(m_scoutTimer, &QTimer::timeout, this, [this, gen]() {
        if (gen != m_launchGen || !m_scoutTimer)
            return;
        pollSilentReadyThenRelaunch();
    });
    m_scoutTimer->start();
#else
    m_allowsGameDetect = true;
#endif
}

void EpicAuth::pollSilentReadyThenRelaunch()
{
#ifdef Q_OS_WIN
    if (!m_scoutTimer)
        return;

    ++m_ticks;

    // ~90s потолок
    if (m_ticks > 180) {
        qWarning() << "[EPIC] silent: timeout — fallback scout";
        fallbackSilentToScout();
        return;
    }

    const bool launcherAlive = epicImageRunning(QStringLiteral("EpicGamesLauncher.exe"))
                               || epicImageRunning(QStringLiteral("EpicWebHelper.exe"))
                               || epicImageRunning(QStringLiteral("EpicGamesLauncher-Win64-Shipping.exe"));
    if (!launcherAlive) {
        if (m_ticks == 10 || m_ticks == 30)
            qWarning() << "[EPIC] silent: ждём процесс Epic... tick" << m_ticks;
        return;
    }

    const EpicReadyStatus st = epicProbeLauncherReady();
    if (st.libraryLikely)
        m_silentSawLibrary = true;
    if (st.confidentLoggedIn)
        ++m_silentConfidentTicks;
    else
        m_silentConfidentTicks = 0;

    // Verbose every poll — diagnose false-positive ready
    qWarning().noquote()
        << QStringLiteral("[EPIC] silent poll tick=%1 title=\"%2\" class=%3 %4x%5 "
                          "| main=%6 loginUi=%7 library=%8 uia=%9 confident=%10 "
                          "| edits=%11 pwd=%12 cont=%13 signIn=%14 why=%15 uri#=%16")
               .arg(m_ticks)
               .arg(st.bestTitle)
               .arg(st.bestClass)
               .arg(st.bestW)
               .arg(st.bestH)
               .arg(st.mainWindow ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.loginUi ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.libraryLikely ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.probe.uiaDescendants)
               .arg(st.confidentLoggedIn ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.probe.edits)
               .arg(st.probe.hasPasswordEdit ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.probe.hasContinue ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.probe.hasSignIn ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(st.why)
               .arg(m_silentRelaunchCount);

    // ——— After URI already fired: safety net ———
    if (m_silentRelaunchCount >= 1) {
        if (st.loginUi) {
            qWarning() << "[EPIC] silent: login UI after URI → fallback scout | tick" << m_ticks;
            fallbackSilentToScout();
            return;
        }
        if (m_silentRelaunchCount == 1 && m_ticks >= m_silentUriFirstTick + 16) {
            qWarning() << "[EPIC] silent: second re-launch nudge";
            relaunchGameUri();
            return;
        }
        // ~20s after first URI: still no library / login stuck → scout
        if (m_silentRelaunchCount >= 2
            && m_ticks >= m_silentUriFirstTick + 40) {
            if (st.loginUi || !m_silentSawLibrary) {
                qWarning() << "[EPIC] silent: no game/library ~20s after URI → fallback scout"
                           << "| sawLibrary:" << m_silentSawLibrary
                           << "| loginUi:" << st.loginUi;
                fallbackSilentToScout();
                return;
            }
            qWarning() << "[EPIC] silent: relaunch done — library ok, ждём детект игры";
            m_allowsGameDetect = true;
            stopScout();
            return;
        }
        return;
    }

    // ——— Before URI: never fire until confident ———
    // Cautious window: first ~8s (tick 16) never fire — RememberMe / splash
    if (m_ticks < 16) {
        return;
    }

    // Login form visible → wait briefly for RememberMe, then force scout (~10s = tick 20)
    if (st.loginUi) {
        if (m_ticks >= 20) {
            qWarning() << "[EPIC] silent: login UI persists → fallback scout | why:" << st.why;
            fallbackSilentToScout();
        }
        return;
    }

    // Uncertain (UIA empty / not library): force scout by ~10s even if old ready said true
    if (!st.confidentLoggedIn) {
        if (m_ticks >= 20) {
            qWarning() << "[EPIC] silent: uncertain after 10s → force fallback scout | why:"
                       << st.why;
            fallbackSilentToScout();
        }
        return;
    }

    // Need 2 consecutive confident polls (~1s) before URI
    if (m_silentConfidentTicks < 2) {
        qWarning() << "[EPIC] silent: library candidate, confirm…" << m_silentConfidentTicks;
        return;
    }

    m_allowsGameDetect = true;
    m_silentUriFirstTick = m_ticks;
    qWarning() << "[EPIC] silent: confident library → re-launch product URL"
               << "| tick:" << m_ticks << "| title:" << st.bestTitle;
    relaunchGameUri();
#else
    Q_UNUSED(m_ticks);
#endif
}

void EpicAuth::startScout(const QString &login, const QString &password)
{
#ifdef Q_OS_WIN
    stopScout();
    m_ticks = 0;
    m_phaseTick = 0;
    m_phase = Phase::WaitEmailDialog;
    m_emailSent = false;
    m_passwordSent = false;
    m_loginHwnd = 0;
    m_pendingLogin = login;
    m_pendingPassword = password;
    // НЕ сбрасываем m_needBackup — processmanager/applyCache уже выставили флаг

    if (!m_expectInteractive || login.isEmpty() || password.isEmpty()) {
        qWarning() << "[EPIC] Scout пропущен (cache/silent)";
        // Клубный тихий вход: RememberMe на диске → ждём ready лаунчера → product URL
        // (если login UI останется — fallbackSilentToScout)
        if (!m_expectInteractive && !login.isEmpty() && !password.isEmpty())
            scheduleSilentRelaunch();
        else
            m_allowsGameDetect = true;
        return;
    }

    m_allowsGameDetect = false;
    m_needBackup = true;
    qWarning() << "[EPIC] Scout START — keyboard Tab: email(2)→Continue(4)→password(3)→Войти(6) → URI"
               << "| login:" << login;

    m_scoutTimer = new QTimer(this);
    m_scoutTimer->setInterval(400);

    connect(m_scoutTimer, &QTimer::timeout, this, [this, login, password]() {
        if (!m_scoutTimer)
            return;

        ++m_ticks;
        if (m_ticks > 300) {
            qWarning() << "[EPIC] Scout TIMEOUT — fallback URI (игра не стартовала?)";
            relaunchGameUri();
            m_allowsGameDetect = true;
            setShellTopmostFrom(this, true);
            stopScout();
            return;
        }

        if (m_phase == Phase::Done) {
            stopScout();
            return;
        }

        EpicLoginEnumCtx ctx;
        ctx.epicPids = collectEpicPids();
        ctx.doDump = (m_ticks == 5 || m_ticks == 20);
        EnumWindows(enumEpicLoginProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.doDump) {
            qWarning() << "[EPIC] scout tick" << m_ticks
                       << "| phase:" << int(m_phase)
                       << "| best:" << (ctx.best ? ctx.title : QStringLiteral("(none)"));
        }

        // Club interactive: login on-screen, shell TOPMOST кроет форму (personal не здесь).
        if (ctx.best) {
            setShellTopmostFrom(this, true);
            if (m_ticks == 1 || m_ticks == 5 || m_ticks % 25 == 0)
                qWarning() << "[EPIC] Интерактивный логин (under overlay):" << ctx.title;
        }

        // Silent fallback landed here but session already ok (no login HWND) → URI
        if (m_phase == Phase::WaitEmailDialog && !ctx.best && m_ticks >= 30) {
            qWarning() << "[EPIC] scout:0 no login UI ~12s — assume logged in → product URI";
            m_phase = Phase::Done;
            m_allowsGameDetect = true;
            relaunchGameUri();
            const int gen = m_launchGen;
            QTimer::singleShot(6000, this, [this, gen]() {
                if (gen != m_launchGen)
                    return;
                relaunchGameUri();
            });
            stopScout();
            return;
        }

        // 1) Login window → keyboard: Tab×2 email → Tab×2 Continue Enter
        if (m_phase == Phase::WaitEmailDialog && ctx.best && !m_emailSent && m_ticks >= 8) {
            m_emailSent = true;
            m_phase = Phase::EmailSubmitted;
            m_phaseTick = m_ticks;
            m_loginHwnd = reinterpret_cast<quintptr>(ctx.best);
            qWarning() << "[EPIC] scout:1 email step START | login:" << login << "|" << ctx.title;
            dumpWindowFull(ctx.best, QStringLiteral("CATCH email"), ctx.score);
            injectEmail(m_loginHwnd, login);
            return;
        }

        // 2) After Continue — wait ~1.6–2s for password UI, then Tab×3 password → Tab×3 Войти
        if (m_phase == Phase::WaitPasswordDialog && !m_passwordSent
            && m_ticks >= m_phaseTick + 5) {
            HWND h = ctx.best ? ctx.best : reinterpret_cast<HWND>(m_loginHwnd);
            if (!h || !IsWindow(h))
                return;
            m_passwordSent = true;
            m_phaseTick = m_ticks;
            m_loginHwnd = reinterpret_cast<quintptr>(h);
            qWarning() << "[EPIC] scout:3 password step START |"
                       << (ctx.best ? ctx.title : QStringLiteral("(reuse hwnd)"));
            injectPassword(m_loginHwnd, password);
            return;
        }

        // 3) After Войти — re-fire product URL, wait for game detect
        if (m_phase == Phase::PasswordSubmitted && m_ticks >= m_phaseTick + 15) {
            m_phase = Phase::Done;
            m_allowsGameDetect = true;
            qWarning() << "[EPIC] scout:4 done — re-launch product URI, ждём игру";
            relaunchGameUri();
            const int gen = m_launchGen;
            QTimer::singleShot(6000, this, [this, gen]() {
                if (gen != m_launchGen)
                    return;
                qWarning() << "[EPIC] interactive: second re-launch nudge";
                relaunchGameUri();
            });
            stopScout();
            return;
        }
    });

    m_scoutTimer->start();
#else
    Q_UNUSED(login);
    Q_UNUSED(password);
    m_allowsGameDetect = true;
#endif
}

void EpicAuth::backupCache(NetworkManager *net, int terminalId, const QString &login,
                           int accountId, int gameId)
{
    if (login.isEmpty()) {
        qWarning() << "[EPIC] cache backup: login пуст";
        return;
    }
    if (!net || net->serverUrl().isEmpty()) {
        qWarning() << "[EPIC] cache backup: serverUrl пуст";
        return;
    }

    const QString settings = readEpicGameUserSettings(m_launcherExe);
    if (settings.trimmed().isEmpty()) {
        qWarning() << "[EPIC] cache backup: сессия не найдена. launcherExe:" << m_launcherExe
                   << "| portal:" << epicPortalRootFromExe(m_launcherExe);
        return;
    }

    const bool hasRemember = settings.contains(QStringLiteral("[RememberMe]"), Qt::CaseInsensitive)
                             && (settings.contains(QStringLiteral("Data="), Qt::CaseInsensitive)
                                 || settings.contains(QStringLiteral("Enable=True"), Qt::CaseInsensitive));
    qWarning() << "[EPIC] cache backup: chars" << settings.size()
               << "| RememberMe usable:" << hasRemember;

    QJsonObject rootPayload;
    rootPayload.insert(QStringLiteral("login"), login);
    rootPayload.insert(QStringLiteral("terminal_id"), terminalId);
    if (accountId > 0)
        rootPayload.insert(QStringLiteral("account_id"), accountId);
    if (gameId > 0)
        rootPayload.insert(QStringLiteral("game_id"), gameId);
    rootPayload.insert(QStringLiteral("platform"), QStringLiteral("epic"));
    rootPayload.insert(QStringLiteral("local_vdf"), settings);
    rootPayload.insert(QStringLiteral("config_vdf"), settings);

    QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qWarning() << "[EPIC] cache backup → server, bytes:" << jsonData.size()
               << "login:" << login << "account_id:" << accountId << "game_id:" << gameId
               << "terminal:" << terminalId;

    QNetworkReply *reply = net->networkAccessManager()->post(request, jsonData);
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            qWarning() << "[EPIC] cache backup OK" << body;
        else
            qWarning() << "[EPIC] cache backup fail:" << reply->errorString() << body;
    });
}
