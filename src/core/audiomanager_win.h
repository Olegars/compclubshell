// Путь: src/core/audiomanager_win.h
#ifndef AUDIOMANAGER_WIN_H
#define AUDIOMANAGER_WIN_H

#ifdef __cplusplus
extern "C" {
#endif

void win32_init_com();
void win32_uninit_com();
void win32_set_master_volume(int level);

/** Continuously force default playback to headphones (Win32). No-op on other platforms. */
void win32_start_headphones_guard();
void win32_stop_headphones_guard();

#ifdef __cplusplus
}
#endif

#endif // AUDIOMANAGER_WIN_H
