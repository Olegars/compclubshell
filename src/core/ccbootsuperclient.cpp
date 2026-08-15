#include "ccbootsuperclient.h"
#include "securitymanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QString>
#include <QTextStream>

#include <initializer_list>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
struct FindTextCtx {
    const wchar_t **needles = nullptr;
    int needleCount = 0;
    HWND found = nullptr;
    bool matchClassEdit = false;
};

BOOL CALLBACK enumChildProc(HWND hwnd, LPARAM lp)
{
    auto *ctx = reinterpret_cast<FindTextCtx *>(lp);
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 511);
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 127);

    if (ctx->matchClassEdit) {
        if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"TEdit") == 0) {
            ctx->found = hwnd;
            return FALSE;
        }
    }

    const QString titleStr = QString::fromWCharArray(title);
    for (int i = 0; i < ctx->needleCount; ++i) {
        if (titleStr.contains(QString::fromWCharArray(ctx->needles[i]), Qt::CaseInsensitive)) {
            ctx->found = hwnd;
            return FALSE;
        }
    }

    EnumChildWindows(hwnd, enumChildProc, lp);
    return ctx->found ? FALSE : TRUE;
}

BOOL CALLBACK enumTopProc(HWND hwnd, LPARAM lp)
{
    if (!IsWindowVisible(hwnd))
        return TRUE;
    return enumChildProc(hwnd, lp);
}

HWND findControl(const wchar_t **needles, int count, bool editField)
{
    FindTextCtx ctx;
    ctx.needles = needles;
    ctx.needleCount = count;
    ctx.matchClassEdit = editField;
    EnumWindows(enumTopProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool clickControl(HWND hwnd)
{
    if (!hwnd)
        return false;
    SetForegroundWindow(GetAncestor(hwnd, GA_ROOT));
    SendMessageW(hwnd, BM_CLICK, 0, 0);
    return true;
}

bool setEditText(HWND hwnd, const QString &text)
{
    if (!hwnd)
        return false;
    const std::wstring w = text.toStdWString();
    SendMessageW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(w.c_str()));
    return true;
}
#endif

} // namespace

CcbootSuperClient::CcbootSuperClient(SecurityManager *security, QObject *parent)
    : QObject(parent)
    , m_security(security)
{
    m_tick = new QTimer(this);
    m_tick->setInterval(350);
    connect(m_tick, &QTimer::timeout, this, &CcbootSuperClient::tick);
    refresh();
}

void CcbootSuperClient::setMainWindow(QWindow *window)
{
    m_mainWindow = window;
}

void CcbootSuperClient::refresh()
{
    detectClientPath();
    detectSuperClientFlag();
    if (!m_busy) {
        if (m_clientPath.isEmpty())
            m_lastMessage = QStringLiteral("CCBoot Client не найден — Super Client включается на сервере.");
        else if (m_superClientActive)
            m_lastMessage = QStringLiteral("Похоже, Super Client уже включён. После правок — выключить и сохранить образ.");
        else
            m_lastMessage = QStringLiteral("Клиент найден. Пароль — Admin Password CCBoot, не PIN брони.");
        m_lastOk = !m_clientPath.isEmpty();
    }
    emit statusChanged();
}

void CcbootSuperClient::unlockForMaintenance()
{
    if (m_security)
        m_security->unlockSystem();
    m_kioskUnlocked = true;
    m_lastOk = true;
    m_lastMessage = QStringLiteral("Киоск снят, explorer запущен. Можно править образ.");
    emit statusChanged();
}

void CcbootSuperClient::lockKiosk()
{
    if (m_security)
        m_security->lockDownSystem();
    m_kioskUnlocked = false;
    restoreShellWindow();
    m_lastOk = true;
    m_lastMessage = QStringLiteral("Киоск включён. Для Super Client лучше reboot после Disable.");
    emit statusChanged();
}

void CcbootSuperClient::enableSuperClient(const QString &password, const QString &diskMode)
{
    startAutomation(true, password, diskMode, true);
}

void CcbootSuperClient::disableSuperClient(const QString &password, bool saveImage)
{
    startAutomation(false, password, QStringLiteral("image"), saveImage);
}

void CcbootSuperClient::openCcbootClient()
{
    unlockForMaintenance();
    hideShellForDialogs();
    if (!launchClient()) {
        restoreShellWindow();
        finish(false, QStringLiteral("Не удалось запустить CCBoot Client."));
        return;
    }
    m_lastOk = true;
    m_lastMessage = QStringLiteral("Открыт CCBoot Client. Enable Super Client — в его окне, затем reboot.");
    emit statusChanged();
}

void CcbootSuperClient::startAutomation(bool enable, const QString &password,
                                        const QString &diskMode, bool saveImage)
{
    if (m_busy)
        return;
    if (password.trimmed().isEmpty()) {
        finish(false, QStringLiteral("Введите Admin Password CCBoot."));
        return;
    }
    detectClientPath();
    if (m_clientPath.isEmpty()) {
        finish(false, QStringLiteral("CCBootClient.exe не найден. Поставьте клиент в образ или укажите Diskless/client_exe."));
        return;
    }

    m_enable = enable;
    m_password = password.trimmed();
    m_diskMode = diskMode.trimmed().toLower();
    if (m_diskMode.isEmpty())
        m_diskMode = QStringLiteral("image");
    m_saveImage = saveImage;
    m_typedPassword = false;
    m_pickedType = false;
    m_ticks = 0;
    m_phase = Phase::WaitMain;

    unlockForMaintenance();
    hideShellForDialogs();
    if (!launchClient()) {
        restoreShellWindow();
        finish(false, QStringLiteral("Не удалось запустить CCBoot Client."));
        return;
    }

    setBusy(true);
    m_lastOk = true;
    m_lastMessage = enable
            ? QStringLiteral("Жду окно CCBoot Client → Enable Super Client…")
            : QStringLiteral("Жду окно CCBoot Client → Disable Super Client…");
    emit statusChanged();
    m_tick->start();
}

void CcbootSuperClient::tick()
{
#ifdef Q_OS_WIN
    ++m_ticks;
    if (m_ticks > 80) { // ~28 с
        m_tick->stop();
        setBusy(false);
        m_phase = Phase::Failed;
        m_lastOk = false;
        m_lastMessage = QStringLiteral(
            "Не дождался диалогов CCBoot. Окно клиента открыто — нажмите Enable/Disable вручную.");
        emit statusChanged();
        return;
    }

    auto clickNamed = [](std::initializer_list<const wchar_t *> names) -> bool {
        const wchar_t *arr[8] = {};
        int n = 0;
        for (const wchar_t *s : names) {
            if (n < 8)
                arr[n++] = s;
        }
        HWND h = findControl(arr, n, false);
        return clickControl(h);
    };

    switch (m_phase) {
    case Phase::WaitMain:
        if (m_enable) {
            if (clickNamed({L"Enable Super Client", L"Enable SuperClient", L"Enable super client"})) {
                m_phase = Phase::WaitTypeDialog;
                m_lastMessage = QStringLiteral("Выбор диска (image / disk)…");
                emit statusChanged();
            }
        } else if (clickNamed({L"Disable Super Client", L"Disable SuperClient", L"Disable super client"})) {
            m_phase = Phase::WaitPassword;
            m_lastMessage = QStringLiteral("Ввод пароля…");
            emit statusChanged();
        }
        break;

    case Phase::WaitTypeDialog: {
        if (!m_pickedType) {
            const wchar_t *needle = L"image";
            if (m_diskMode == QLatin1String("disk"))
                needle = L"Disk";
            else if (m_diskMode == QLatin1String("both"))
                needle = L"Image + Disk";
            const wchar_t *arr[] = {needle, L"image", L"Image"};
            HWND h = findControl(arr, 3, false);
            if (h) {
                clickControl(h);
                m_pickedType = true;
            }
        }
        if (clickNamed({L"OK", L"Ok", L"&OK"}))
            m_phase = Phase::WaitPassword;
        break;
    }

    case Phase::WaitPassword: {
        if (!m_typedPassword) {
            HWND edit = findControl(nullptr, 0, true);
            if (edit && setEditText(edit, m_password))
                m_typedPassword = true;
        }
        if (m_typedPassword && clickNamed({L"OK", L"Ok", L"&OK"})) {
            m_phase = m_enable ? Phase::WaitRebootPrompt : Phase::WaitConfirm;
            m_lastMessage = m_enable
                    ? QStringLiteral("Подтверждение reboot…")
                    : QStringLiteral("Сохранение образа…");
            emit statusChanged();
        }
        break;
    }

    case Phase::WaitConfirm:
        if (m_saveImage)
            clickNamed({L"Yes", L"&Yes"});
        else
            clickNamed({L"No", L"&No"});
        m_phase = Phase::WaitRebootPrompt;
        break;

    case Phase::WaitRebootPrompt:
        clickNamed({L"OK", L"Yes", L"&Yes", L"&OK"});
        if (m_ticks > 12) {
            m_tick->stop();
            finish(true, m_enable
                   ? QStringLiteral("Super Client: команда отправлена. ПК должен уйти в reboot.")
                   : QStringLiteral("Disable Super Client: следуйте диалогам CCBoot (сохранить образ → shutdown)."));
        }
        break;

    default:
        break;
    }
#else
    m_tick->stop();
    finish(false, QStringLiteral("Super Client только на Windows."));
#endif
}

bool CcbootSuperClient::launchClient()
{
    if (m_clientPath.isEmpty())
        return false;
    return QProcess::startDetached(m_clientPath, {});
}

void CcbootSuperClient::hideShellForDialogs()
{
    if (!m_mainWindow)
        return;
    m_mainWindow->showMinimized();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(m_mainWindow->winId());
    if (hwnd)
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

void CcbootSuperClient::restoreShellWindow()
{
    if (!m_mainWindow)
        return;
    m_mainWindow->showFullScreen();
    m_mainWindow->raise();
    m_mainWindow->requestActivate();
}

void CcbootSuperClient::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void CcbootSuperClient::finish(bool ok, const QString &message)
{
    m_tick->stop();
    m_phase = Phase::Idle;
    setBusy(false);
    m_lastOk = ok;
    m_lastMessage = message;
    detectSuperClientFlag();
    emit statusChanged();
}

void CcbootSuperClient::detectClientPath()
{
    QSettings ini(QCoreApplication::applicationDirPath() + QStringLiteral("/config.ini"),
                  QSettings::IniFormat);
    const QString configured = ini.value(QStringLiteral("Diskless/client_exe")).toString().trimmed();
    const QStringList candidates = {
        configured,
        QStringLiteral("C:/CCBootClient/CCBootClient.exe"),
        QStringLiteral("C:/CCBoot/CCBootClient.exe"),
        QStringLiteral("C:/Program Files/CCBoot/CCBootClient.exe"),
        QStringLiteral("C:/Program Files (x86)/CCBoot/CCBootClient.exe"),
        QStringLiteral("C:/Program Files/CCBootCloud/CCBootClient.exe"),
    };
    m_clientPath.clear();
    for (const QString &p : candidates) {
        if (!p.isEmpty() && QFileInfo::exists(p)) {
            m_clientPath = QFileInfo(p).absoluteFilePath();
            break;
        }
    }
}

void CcbootSuperClient::detectSuperClientFlag()
{
    m_superClientActive = false;
#ifdef Q_OS_WIN
    QSettings ccboot(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\CCBoot"), QSettings::NativeFormat);
    if (ccboot.value(QStringLiteral("SuperClient")).toInt() == 1
        || ccboot.value(QStringLiteral("EnableSuperClient")).toInt() == 1)
        m_superClientActive = true;
    QSettings young(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Youngzsoft\\CCBoot"),
                    QSettings::NativeFormat);
    if (young.value(QStringLiteral("SuperClient")).toInt() == 1)
        m_superClientActive = true;
#endif
    const QStringList inis = {
        QStringLiteral("C:/CCBootClient/CCBoot.ini"),
        QStringLiteral("C:/CCBoot/CCBoot.ini"),
        QFileInfo(m_clientPath).absolutePath() + QStringLiteral("/CCBoot.ini"),
    };
    for (const QString &ini : inis) {
        const QString v = readIniValue(ini, QStringLiteral("SuperClient"));
        if (v == QLatin1String("1") || v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
            || v.compare(QLatin1String("yes"), Qt::CaseInsensitive) == 0) {
            m_superClientActive = true;
            break;
        }
    }
}

QString CcbootSuperClient::readIniValue(const QString &filePath, const QString &key) const
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&f);
    const QString prefix = key + QLatin1Char('=');
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(prefix, Qt::CaseInsensitive))
            return line.mid(prefix.size()).trimmed();
    }
    return {};
}
