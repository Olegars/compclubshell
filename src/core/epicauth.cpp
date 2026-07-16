#include "epicauth.h"
#include "processmanager.h"
#include "networkmanager.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
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

static void placeWindowForInput(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    DWORD pid = 0;
    const DWORD epicTid = GetWindowThreadProcessId(hwnd, &pid);
    AllowSetForegroundWindow(pid);

    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);

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

static void clickScreen(int x, int y)
{
    SetCursorPos(x, y);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

static void clickClient(HWND hwnd, int clientX, int clientY)
{
    POINT pt{ clientX, clientY };
    ClientToScreen(hwnd, &pt);
    clickScreen(pt.x, pt.y);
}

static void sendTabs(int count, const char *why)
{
    qWarning() << "[EPIC] Tab x" << count << why;
    for (int i = 0; i < count; ++i) {
        sendVk(VK_TAB);
        Sleep(140);
    }
}

// Ручной порядок Tab на email-экране Epic:
// 1 — ничего, 2 — поле email, 3 — ничего, 4 — «Продолжить».
// После фокуса в поле: ещё 2×Tab → «Продолжить».

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

// Только один способ ввода — раньше typeUnicode + Ctrl+V давали пароль дважды.
static void clearAndType(HWND hwnd, const QString &text)
{
    placeWindowForInput(hwnd);
    sendCtrlA();
    Sleep(40);
    sendVk(VK_DELETE);
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
            qWarning() << "[EPIC] input via clipboard paste, len" << text.size();
        } else {
            CloseClipboard();
        }
    }
    if (!pasted) {
        qWarning() << "[EPIC] input via unicode type, len" << text.size();
        typeUnicode(text);
    }
}

static void sendSpace() { sendVk(VK_SPACE); }

static void setShellTopmostFrom(QObject *auth, bool enabled)
{
    if (auto *pm = qobject_cast<ProcessManager *>(auth->parent()))
        pm->setShellTopmost(enabled);
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
    if (t.isEmpty() || isEpicLauncherMainTitle(t))
        return false;
    return t.contains(QStringLiteral("Войти"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Sign in"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Sign In"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Log in"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Login"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("учётн"), Qt::CaseInsensitive)
           || t.contains(QStringLiteral("Account"), Qt::CaseInsensitive);
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

void EpicAuth::killLauncher()
{
    qWarning() << "[EPIC] killLauncher: taskkill Epic + WebHelper";
    silentKill(QStringLiteral("EpicGamesLauncher.exe"));
    silentKill(QStringLiteral("EpicWebHelper.exe"));
    silentKill(QStringLiteral("EpicGamesLauncher-Win64-Shipping.exe"));
}

bool EpicAuth::applyCache(const QJsonObject &authData)
{
    const QString login = authData.value(QStringLiteral("login")).toString();
    QString exe = authData.value(QStringLiteral("exe_path")).toString().trimmed();
    if (exe.isEmpty()) {
        const QJsonObject launcher = authData.value(QStringLiteral("launcher")).toObject();
        exe = launcher.value(QStringLiteral("exe_path")).toString().trimmed();
    }
    if (!exe.isEmpty())
        m_launcherExe = exe;

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

    QStringList args;
    if (!m_launchUri.isEmpty())
        args << m_launchUri;
    else if (!argsStr.isEmpty())
        args = QProcess::splitCommand(argsStr);

    qWarning().noquote() << "[EPIC] Launch exe:" << exe;
    qWarning().noquote() << "[EPIC] Launch args:" << args;
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

void EpicAuth::injectEmail(quintptr hwndVal, const QString &email)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd))
        return;

    setShellTopmostFrom(this, false);
    placeWindowForInput(hwnd);

    QTimer::singleShot(400, this, [this, hwndVal, email]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;
        placeWindowForInput(h);
        // Без мыши: 2×Tab → поле email
        sendTabs(2, "→ поле email");
        QTimer::singleShot(200, this, [this, hwndVal, email]() {
            HWND h2 = reinterpret_cast<HWND>(hwndVal);
            if (!h2 || !IsWindow(h2) || !m_scoutTimer)
                return;
            placeWindowForInput(h2);
            qWarning() << "[EPIC] type email";
            clearAndType(h2, email);
            QTimer::singleShot(400, this, [this, hwndVal]() {
                HWND h3 = reinterpret_cast<HWND>(hwndVal);
                if (!h3 || !IsWindow(h3) || !m_scoutTimer)
                    return;
                placeWindowForInput(h3);
                // Ещё 2×Tab → «Продолжить», Enter (не кликаем — Xbox рядом)
                sendTabs(2, "→ Продолжить");
                Sleep(100);
                sendReturnKey();
                qWarning() << "[EPIC] Enter на Продолжить — ждём пароль";
                m_phase = Phase::WaitPasswordDialog;
                m_phaseTick = m_ticks;
            });
        });
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

    setShellTopmostFrom(this, false);
    placeWindowForInput(hwnd);

    QTimer::singleShot(400, this, [this, hwndVal, password]() {
        HWND h = reinterpret_cast<HWND>(hwndVal);
        if (!h || !IsWindow(h) || !m_scoutTimer)
            return;
        placeWindowForInput(h);
        // Шаг 3 — поле пароля; 4 — глаз (показать); 6 — «Войти»
        sendTabs(3, "шаг3 → поле password");
        QTimer::singleShot(200, this, [this, hwndVal, password]() {
            HWND h2 = reinterpret_cast<HWND>(hwndVal);
            if (!h2 || !IsWindow(h2) || !m_scoutTimer)
                return;
            placeWindowForInput(h2);
            qWarning() << "[EPIC] шаг3: ввод пароля (один раз)";
            clearAndType(h2, password);
            QTimer::singleShot(350, this, [this, hwndVal]() {
                HWND h3 = reinterpret_cast<HWND>(hwndVal);
                if (!h3 || !IsWindow(h3) || !m_scoutTimer)
                    return;
                placeWindowForInput(h3);
                // Чекбокс «Запомнить меня» (слева под полем) — нужен для cache
                RECT rc{};
                GetClientRect(h3, &rc);
                const int remX = int((rc.right - rc.left) * 0.36);
                const int remY = int((rc.bottom - rc.top) * 0.58);
                qWarning() << "[EPIC] click Запомнить меня" << remX << remY;
                clickClient(h3, remX, remY);
                Sleep(150);
                sendTabs(1, "шаг4 → глаз показать пароль");
                Sleep(80);
                sendSpace();
                qWarning() << "[EPIC] шаг4: показать пароль (Space)";
                QTimer::singleShot(500, this, [this, hwndVal]() {
                    HWND h4 = reinterpret_cast<HWND>(hwndVal);
                    if (!h4 || !IsWindow(h4) || !m_scoutTimer)
                        return;
                    placeWindowForInput(h4);
                    sendTabs(2, "шаг6 → Войти");
                    Sleep(100);
                    sendReturnKey();
                    m_needBackup = true;
                    m_phase = Phase::PasswordSubmitted;
                    m_phaseTick = m_ticks;
                    m_allowsGameDetect = true;
                    qWarning() << "[EPIC] шаг6: Enter на Войти — детект игры включён";
                });
            });
        });
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
    const HINSTANCE r = ShellExecuteW(
        nullptr, L"open",
        reinterpret_cast<LPCWSTR>(m_launchUri.utf16()),
        nullptr, nullptr, SW_SHOWNORMAL);
    qWarning() << "[EPIC] relaunch game URI result:" << int(reinterpret_cast<quintptr>(r))
               << m_launchUri;
#else
    Q_UNUSED(m_launchUri);
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
    // НЕ сбрасываем m_needBackup — processmanager/applyCache уже выставили флаг

    if (!m_expectInteractive || login.isEmpty() || password.isEmpty()) {
        qWarning() << "[EPIC] Scout пропущен (cache/silent)";
        m_allowsGameDetect = true;
        return;
    }

    m_allowsGameDetect = false;
    m_needBackup = true;
    qWarning() << "[EPIC] Scout START (UIA пуст → Tab+Enter, без опроса), login:" << login;

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

        if (m_phase == Phase::WaitEmailDialog && ctx.best && !m_emailSent && m_ticks >= 8) {
            m_emailSent = true;
            m_phase = Phase::EmailSubmitted;
            m_phaseTick = m_ticks;
            qWarning() << "[EPIC] окно логина:" << ctx.title;
            dumpWindowFull(ctx.best, QStringLiteral("CATCH email"), ctx.score);
            injectEmail(reinterpret_cast<quintptr>(ctx.best), login);
            return;
        }

        // После Tab+Enter «Продолжить» — экран пароля
        if (m_phase == Phase::WaitPasswordDialog && ctx.best && !m_passwordSent
            && m_ticks >= m_phaseTick + 18) {
            m_passwordSent = true;
            m_phaseTick = m_ticks;
            qWarning() << "[EPIC] экран пароля";
            injectPassword(reinterpret_cast<quintptr>(ctx.best), password);
            return;
        }

        // URI не повторяем. Оверлей снимет processmanager по gameStartedSuccessfully.
        if (m_phase == Phase::PasswordSubmitted && m_ticks >= m_phaseTick + 15) {
            m_phase = Phase::Done;
            m_allowsGameDetect = true;
            // Не поднимаем shell TOPMOST снова — иначе игра останется под оверлеем
            qWarning() << "[EPIC] Scout done — ждём детект игры / снятие оверлея";
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

void EpicAuth::backupCache(NetworkManager *net, int terminalId, const QString &login)
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
    rootPayload.insert(QStringLiteral("platform"), QStringLiteral("epic"));
    rootPayload.insert(QStringLiteral("local_vdf"), settings);
    rootPayload.insert(QStringLiteral("config_vdf"), settings);

    QUrl url(net->serverUrl() + QStringLiteral("/api/shell/games/update-vdf"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray jsonData = QJsonDocument(rootPayload).toJson(QJsonDocument::Compact);
    qWarning() << "[EPIC] cache backup → server, bytes:" << jsonData.size()
               << "login:" << login << "terminal:" << terminalId;

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
