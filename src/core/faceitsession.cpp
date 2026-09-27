#include "faceitsession.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QDebug>
#include <cstring>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

int g_userId = 0;
QString g_profileDir;

QString native(const QString &path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

#ifdef Q_OS_WIN
bool isReparse(const QString &path)
{
    const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool createJunction(const QString &linkPath, const QString &targetPath)
{
    const QString link = native(linkPath);
    const QString target = native(targetPath);
    if (QFileInfo::exists(link)) {
        if (isReparse(link))
            RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));
        else {
            qWarning() << "[FACEIT] профиль уже каталог, junction не ставим:" << link;
            return false;
        }
    }
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()), nullptr))
        return false;

    const std::wstring substitute = std::wstring(L"\\??\\") + target.toStdWString();
    const std::wstring printName = target.toStdWString();
    const DWORD subBytes = static_cast<DWORD>(substitute.size() * sizeof(wchar_t));
    const DWORD printBytes = static_cast<DWORD>(printName.size() * sizeof(wchar_t));
    const DWORD pathBytes = subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    QByteArray raw(static_cast<int>(sizeof(DWORD) + sizeof(WORD) * 6 + pathBytes), 0);
    auto *buf = reinterpret_cast<unsigned char *>(raw.data());
    auto write16 = [&](int offset, quint16 value) {
        buf[offset] = static_cast<unsigned char>(value & 0xff);
        buf[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xff);
    };
    *reinterpret_cast<DWORD *>(buf) = 0xA0000003; // IO_REPARSE_TAG_MOUNT_POINT
    write16(4, static_cast<quint16>(pathBytes + 8));
    write16(6, 0);
    write16(8, 0);
    write16(10, static_cast<quint16>(subBytes));
    write16(12, static_cast<quint16>(subBytes + sizeof(wchar_t)));
    write16(14, static_cast<quint16>(printBytes));
    memcpy(buf + 16, substitute.data(), subBytes);
    memcpy(buf + 16 + subBytes + sizeof(wchar_t), printName.data(), printBytes);

    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(link.utf16()),
                                GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));
        return false;
    }
    DWORD ignored = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, raw.data(),
                                    static_cast<DWORD>(raw.size()), nullptr, 0, &ignored, nullptr);
    CloseHandle(handle);
    if (!ok)
        RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));
    return ok;
}

void removeJunction(const QString &linkPath)
{
    const QString link = native(linkPath);
    if (isReparse(link))
        RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));
}
#endif

QString envPath(const char *name)
{
    return QDir::fromNativeSeparators(qEnvironmentVariable(name));
}

} // namespace

void FaceitSession::mount(int userId, const QString &dataRoot)
{
    if (userId < 1)
        return;
    if (g_userId == userId)
        return;
    if (g_userId > 0)
        release(false);
#ifndef Q_OS_WIN
    Q_UNUSED(dataRoot);
    g_userId = userId;
    return;
#else
    const QString root = dataRoot.isEmpty() ? QStringLiteral("D:/ShellData") : dataRoot;
    const QString base = native(root + QStringLiteral("/faceit/") + QString::number(userId));
    QDir().mkpath(base + QStringLiteral("/Local"));
    QDir().mkpath(base + QStringLiteral("/Roaming"));
    const QString local = envPath("LOCALAPPDATA") + QStringLiteral("/FACEIT");
    const QString roaming = envPath("APPDATA") + QStringLiteral("/FACEIT");
    const bool a = createJunction(local, base + QStringLiteral("/Local"));
    const bool b = createJunction(roaming, base + QStringLiteral("/Roaming"));
    if (a && b) {
        g_userId = userId;
        g_profileDir = base;
    } else {
        qWarning() << "[FACEIT] junction Local" << a << "Roaming" << b;
    }
#endif
}

void FaceitSession::release(bool force)
{
    if (!force && g_userId < 1 && g_profileDir.isEmpty())
        return;
    g_userId = 0;
#ifdef Q_OS_WIN
    QProcess::execute(QStringLiteral("taskkill"), {QStringLiteral("/F"), QStringLiteral("/IM"), QStringLiteral("FACEIT.exe"), QStringLiteral("/T")});
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software"), QSettings::NativeFormat);
    reg.remove(QStringLiteral("FACEIT"));
    reg.sync();
    const QString local = envPath("LOCALAPPDATA") + QStringLiteral("/FACEIT");
    const QString roaming = envPath("APPDATA") + QStringLiteral("/FACEIT");
    removeJunction(local);
    removeJunction(roaming);
    if (!g_profileDir.isEmpty()) {
        QDir dir(g_profileDir);
        if (dir.exists())
            dir.removeRecursively();
        g_profileDir.clear();
    }
#else
    Q_UNUSED(g_userId);
#endif
}
