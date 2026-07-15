#pragma once
#include <QString>
#include <QByteArray>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace SteamCrypto {

// Steam ConnectCache: CryptProtectData + entropy = accountName, description = BObfuscateBuffer
inline QString encryptTokenDPAPI(const QString &rawToken, const QString &accountName)
{
#ifdef Q_OS_WIN
    QByteArray dataToEncrypt = rawToken.trimmed().toUtf8();
    dataToEncrypt.append('\0');

    QByteArray entropyBytes = accountName.toUtf8();

    DATA_BLOB dataIn;
    dataIn.pbData = reinterpret_cast<BYTE*>(dataToEncrypt.data());
    dataIn.cbData = static_cast<DWORD>(dataToEncrypt.size());

    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE*>(entropyBytes.data());
    entropy.cbData = static_cast<DWORD>(entropyBytes.size());

    DATA_BLOB dataOut;
    if (CryptProtectData(&dataIn,
                         L"BObfuscateBuffer",
                         &entropy,
                         NULL,
                         NULL,
                         CRYPTPROTECT_UI_FORBIDDEN,
                         &dataOut)) {
        QByteArray encrypted(reinterpret_cast<char*>(dataOut.pbData), static_cast<int>(dataOut.cbData));
        LocalFree(dataOut.pbData);
        const QString hex = QString::fromLatin1(encrypted.toHex().toLower());
        qDebug() << "[CRYPTO] DPAPI Protect OK | account:" << accountName
                 << "| tokenLen:" << rawToken.trimmed().size()
                 << "| hexLen:" << hex.size();
        return hex;
    }

    qCritical() << "[CRYPTO] CryptProtectData FAILED, GetLastError=" << GetLastError()
                << "| account:" << accountName;
#else
    Q_UNUSED(rawToken);
    Q_UNUSED(accountName);
#endif
    return QString();
}

inline QString decryptTokenDPAPI(const QString &hexBlob, const QString &accountName)
{
#ifdef Q_OS_WIN
    QByteArray encrypted = QByteArray::fromHex(hexBlob.trimmed().toLatin1());
    if (encrypted.isEmpty()) {
        qWarning() << "[CRYPTO] decrypt: пустой/битый hex";
        return QString();
    }

    QByteArray entropyBytes = accountName.toUtf8();

    DATA_BLOB dataIn;
    dataIn.pbData = reinterpret_cast<BYTE*>(encrypted.data());
    dataIn.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE*>(entropyBytes.data());
    entropy.cbData = static_cast<DWORD>(entropyBytes.size());

    DATA_BLOB dataOut;
    LPWSTR descr = nullptr;
    if (CryptUnprotectData(&dataIn,
                           &descr,
                           &entropy,
                           NULL,
                           NULL,
                           CRYPTPROTECT_UI_FORBIDDEN,
                           &dataOut)) {
        QByteArray plain(reinterpret_cast<char*>(dataOut.pbData), static_cast<int>(dataOut.cbData));
        LocalFree(dataOut.pbData);
        if (descr) LocalFree(descr);
        while (plain.endsWith('\0'))
            plain.chop(1);
        const QString token = QString::fromUtf8(plain);
        qDebug() << "[CRYPTO] DPAPI Unprotect OK | account:" << accountName
                 << "| tokenPrefix:" << token.left(12)
                 << "| tokenLen:" << token.size();
        return token;
    }

    qCritical() << "[CRYPTO] CryptUnprotectData FAILED, GetLastError=" << GetLastError()
                << "| account:" << accountName;
#else
    Q_UNUSED(hexBlob);
    Q_UNUSED(accountName);
#endif
    return QString();
}

} // namespace SteamCrypto
