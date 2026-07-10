// Путь: src/core/audiomanager_win.h
#ifndef AUDIOMANAGER_WIN_H
#define AUDIOMANAGER_WIN_H

#ifdef __cplusplus
extern "C" {
#endif

void win32_init_com();
void win32_uninit_com();
void win32_set_master_volume(int level);

#ifdef __cplusplus
}
#endif

#endif // AUDIOMANAGER_WIN_H