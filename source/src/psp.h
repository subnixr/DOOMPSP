// Shared declarations of the PSP port layer (psp_*.c, reqfile.c) and of the
// few hooks the engine files use.

#ifndef __PSP_H__
#define __PSP_H__

#include <psptypes.h>

// room for psp_home plus a subdir and file name
#define PSP_PATH_MAX 512

#define NUM_CHEAT_SLOTS 12
#define ADHOC_MAXPLAYERS 4

// psp_main.c
extern char psp_home[256];      // launch dir, ends in '/' (empty = current dir)
extern char psp_exe_path[256];
extern int psp_relaunch_ok;
extern int psp_use_tv;
extern u32 psp_btn_ok, psp_btn_back; // confirm/cancel, follow the system setting
extern volatile int psp_resume_count; // bumped by the power callback

// set by the launcher lobby before D_DoomMain
extern int psp_net_enabled;
extern int psp_net_player1;     // our player number, 1 = host
extern int psp_adhoc_numnodes;  // players in the game, us included
extern unsigned char psp_adhoc_mac[ADHOC_MAXPLAYERS][6]; // [0] is us, then the other players

void gui_PrePrint(void);
void gui_PostPrint(void);
int gui_PrintWidth(char *text);
void gui_Print(char *text, u32 fc, u32 bc, int x, int y);

// reqfile.c
char *RequestFile (char *initialPath);

// psp_system.c
extern int quit_requested;      // HOME -> Quit

// psp_sound.c
extern volatile int snd_ticks;  // advanced by the sound thread

// psp_video.c
void psp_tv_mode(int cable, int laced);

// g_game.c
extern int psp_stickturn;       // move stick X turns instead of strafing
extern int psp_alwaysrun;       // run by default, speed key walks
extern int psp_weapon_change;   // next/prev weapon, consumed by G_BuildTiccmd

// kernel helper modules (dvemgr.prx, relaunch.prx)
int pspDveMgrCheckVideoOut(void);
int pspDveMgrSetVideoOut(int, int, int, int, int, int, int);
int pspRelaunchSelf(int apitype, const char *path);

#endif
