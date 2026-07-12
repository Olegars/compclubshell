#pragma once
#include <QString>
#include <QByteArray>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace SteamCrypto {
    inline QString encryptTokenDPAPI(const QString &rawToken, const wchar_t* description) {
#ifdef Q_OS_WIN
        QByteArray dataToEncrypt = rawToken.trimmed().toUtf8();
        dataToEncrypt.append('\0'); // Критично для C-строк Steam

        DATA_BLOB dataIn, dataOut;
        dataIn.pbData = reinterpret_cast<BYTE*>(dataToEncrypt.data());
        dataIn.cbData = static_cast<DWORD>(dataToEncrypt.size());

        // Передаем description (L"SteamPassword" или L"ObfuscateBuffer")
        if (CryptProtectData(&dataIn, description, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
            QByteArray encryptedData(reinterpret_cast<char*>(dataOut.pbData), static_cast<int>(dataOut.cbData));
            LocalFree(dataOut.pbData);

            // Строго HEX в нижнем регистре
            return QString::fromLatin1(encryptedData.toHex().toLower());
        } else {
            qCritical() << "[CRYPTO-ERROR] CryptProtectData failed, code:" << GetLastError();
        }
#endif
        return QString();
    }
}