#pragma once

#include <QString>

/**
 * Профиль клиента FACEIT на D:, не на C:\\Users\\user.
 * Службу FACEIT.sys не останавливаем.
 */
class FaceitSession
{
public:
    static void mount(int userId, const QString &dataRoot);
    static void release(bool force = false);
};
