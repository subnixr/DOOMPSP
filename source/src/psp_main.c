// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Main program, simply calls D_DoomMain high level loop.
//
//-----------------------------------------------------------------------------

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspmoduleinfo.h>
#include <psputility.h>
#include <psputility_osk.h>
#include <pspaudio.h>
#include <pspaudiolib.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <kubridge.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>

#include <pspsdk.h>
#include <psputility_netmodules.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>

#include "intraFont.h"

#define printf pspDebugScreenPrintf

#define VERS	 1
#define REVS	 4


#include "doomdef.h"

#include "m_argv.h"
#include "d_main.h"
#include "i_system.h"
#include "m_fixed.h"
#include "psp.h"


int VERSION = 110;

char psp_home[256];
char psp_exe_path[256];
int psp_relaunch_ok = 0;

int psp_use_intrafont = 0;
intraFont *ltn8 = 0;

PSP_MODULE_INFO("DOOM", 0, VERS, REVS);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-2048);	// all free RAM minus 2MB, left for system dialogs (OSK)
//PSP_HEAP_SIZE_KB(12000);

/* Exit callback */
int exit_callback(int arg1, int arg2, void *common) {
	quit_requested = 1;
	return 0;
}

/* Power callback: sleep (power switch, PSP Go slider, Pause Game).
 * While suspending, the main thread parks (psp_wait_resume) so it does
 * no file I/O. Sleep invalidates open file handles; w_wad.c reopens
 * them when psp_resume_count changes. */
volatile int psp_resume_count = 0;

int power_callback(int unknown, int flags, void *common) {
	if (flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY))
	{
		psp_suspending = 1;
		psp_sleeplog("power cb %08X: suspending, main at %s", flags, psp_step);
	}
	else if (flags & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE))
	{
		if (flags & PSP_POWER_CB_RESUME_COMPLETE)
		{
			psp_resume_count++;
			psp_sleeplog("power cb %08X: resume complete", flags);
		}
		psp_suspending = 0;
	}
	else
		psp_sleeplog("power cb %08X: other, main at %s", flags, psp_step);
	return 0;
}

/* Callback thread */
int CallbackThread(SceSize args, void *argp) {
	int cbid;

	cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
	sceKernelRegisterExitCallback(cbid);
	cbid = sceKernelCreateCallback("Power Callback", power_callback, NULL);
	scePowerRegisterCallback(-1, cbid);
	sceKernelSleepThreadCB();
	return 0;
}

/* Sets up the callback thread and returns its thread id */
int SetupCallbacks(void) {
	int thid = 0;

	thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, 0);
	if(thid >= 0) {
		sceKernelStartThread(thid, 0, 0);
	}

	return thid;
}

// blank debug text screen, white on black
static void psp_text_screen(void)
{
	pspDebugScreenInit();
	pspDebugScreenSetBackColor(0xFF000000);
	pspDebugScreenSetTextColor(0xFFFFFFFF);
	pspDebugScreenClear();
}

/*
 * OSK support code
 *
 */

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

static unsigned int __attribute__((aligned(16))) list[262144];

// osk = 1: state the system keyboard dialog wants, 0: the launcher's own
// drawing (blending on)
static void InitGu(int osk)
{
	sceGuInit();
	sceGuStart(GU_DIRECT,list);
	sceGuDrawBuffer(GU_PSM_8888,(void*)0,BUF_WIDTH);
	sceGuDispBuffer(SCR_WIDTH,SCR_HEIGHT,(void*)0x88000,BUF_WIDTH);
	sceGuDepthBuffer((void*)0x110000,BUF_WIDTH);
	sceGuOffset(2048 - (SCR_WIDTH/2),2048 - (SCR_HEIGHT/2));
	sceGuViewport(2048,2048,SCR_WIDTH,SCR_HEIGHT);
	if (osk)
		sceGuDepthRange(0xc350,0x2710);
	else
		sceGuDepthRange(65535,0);
	sceGuScissor(0,0,SCR_WIDTH,SCR_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuFrontFace(GU_CW);
	sceGuShadeModel(GU_SMOOTH);
	sceGuEnable(GU_CULL_FACE);
	sceGuEnable(GU_CLIP_PLANES);
	if (!osk)
	{
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	}
	sceGuFinish();
	sceGuSync(0,0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
}

// confirm/cancel buttons follow the system setting (O confirms on
// Japanese firmware, and CFW can swap it)
u32 psp_btn_ok = PSP_CTRL_CROSS;
u32 psp_btn_back = PSP_CTRL_CIRCLE;
int psp_btn_swap = 1; // PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP: 1 = X confirms, 0 = O confirms

void psp_read_button_swap(void)
{
	int val;

	if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &val) != PSP_SYSTEMPARAM_RETVAL_OK)
		return; // keep X-confirm defaults
	psp_btn_swap = val ? 1 : 0;
	psp_btn_ok = val ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
	psp_btn_back = val ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS;
}

int get_text_osk(char *input, unsigned short *intext, unsigned short *desc)
{
	int done=0;
	int shutdown=0;
	unsigned short outtext[128] = { 0 }; // text after input

	InitGu(1);

	SceUtilityOskData data;
	memset(&data, 0, sizeof(data));
	data.language = 2;			// key glyphs: 0-1=hiragana, 2+=western/whatever the other field says
	data.lines = 1;				// just one line
	data.unk_24 = 1;			// set to 1
	data.desc = desc;
	data.intext = intext;
	data.outtextlength = 128;	// sizeof(outtext) / sizeof(unsigned short)
	data.outtextlimit = 50;		// just allow 50 chars
	data.outtext = (unsigned short*)outtext;

	SceUtilityOskParams osk;
	memset(&osk, 0, sizeof(osk));
	osk.base.size = sizeof(osk);
	// dialog language: 0=Japanese, 1=English, 2=French, 3=Spanish, 4=German,
	// 5=Italian, 6=Dutch, 7=Portuguese, 8=Russian, 9=Korean, 10-11=Chinese, 12+=default
	osk.base.language = 1;
	osk.base.buttonSwap = psp_btn_swap;	// match system X/O setting
	osk.base.graphicsThread = 17;	// gfx thread pri
	osk.base.accessThread = 19;			// unknown thread pri (?)
	osk.base.fontThread = 18;
	osk.base.soundThread = 16;
	osk.datacount = 1;
	osk.data = &data;

	int rc = sceUtilityOskInitStart(&osk);
	if (rc)
	{
		InitGu(0);
		return 0;
	}

	while(!done) {
		int i,j=0;

		sceGuStart(GU_DIRECT,list);

		// clear screen
		sceGuClearColor(0xff554433);
		sceGuClearDepth(0);
		sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);

		sceGuFinish();
		sceGuSync(0,0);

		switch(sceUtilityOskGetStatus())
		{
			case PSP_UTILITY_DIALOG_INIT :
			break;
			case PSP_UTILITY_DIALOG_VISIBLE :
			sceUtilityOskUpdate(2); // 2 is taken from ps2dev.org recommendation
			break;
			case PSP_UTILITY_DIALOG_QUIT :
			if (!shutdown)
			{
				sceUtilityOskShutdownStart();
				shutdown = 1;
			}
			break;
			case PSP_UTILITY_DIALOG_FINISHED :
			done = 1;
			break;
			case PSP_UTILITY_DIALOG_NONE :
			// some firmwares go QUIT -> NONE without us seeing FINISHED
			if (shutdown)
				done = 1;
			break;
			default :
			break;
		}
		if (quit_requested)
		{
			// HOME -> Quit while the OSK is up
			sceKernelExitGame();
		}

		for(i = 0; data.outtext[i]; i++)
			if (data.outtext[i]!='\0' && data.outtext[i]!='\n' && data.outtext[i]!='\r')
			{
				input[j] = data.outtext[i];
				j++;
			}
		input[j] = 0;

		// wait TWO vblanks because one makes the input "twitchy"
		sceDisplayWaitVblankStart();
		sceDisplayWaitVblankStart();
		sceGuSwapBuffers();
	}

	InitGu(0);

	// 0 = init failed, -1 = user cancelled, 1 = text entered
	if (data.result == PSP_UTILITY_OSK_RESULT_CANCELLED)
		return -1;

	return 1;
}

/*
 * intraFont support code
 *
 */

// fallback when the system font can't be loaded: the SDK's 8x8 debug font
// as a 16x16 glyph atlas, drawn through GU like intraFont
extern unsigned char msx[];
static u32 __attribute__((aligned(16))) dbgfont_tex[128*128];

static void dbgfont_init(void)
{
	int c, row, col;

	for (c = 0; c < 256; c++)
		for (row = 0; row < 8; row++)
			for (col = 0; col < 8; col++)
				dbgfont_tex[((c >> 4) * 8 + row) * 128 + (c & 15) * 8 + col] =
					(msx[c * 8 + row] & (128 >> col)) ? 0xFFFFFFFF : 0x00FFFFFF;
	sceKernelDcacheWritebackAll();
}

static void dbgfont_print(char *text, u32 color, int x, int y)
{
	struct { float u, v; u32 c; float x, y, z; } *v;
	int i, len = strlen(text);

	if (!len)
		return;

	v = sceGuGetMemory(sizeof(*v) * 2 * len);
	for (i = 0; i < len; i++)
	{
		int c = (unsigned char)text[i];

		v[i*2].u = (c & 15) * 8;
		v[i*2].v = (c >> 4) * 8;
		v[i*2].c = color;
		v[i*2].x = x + i * 7;
		v[i*2].y = y - 8; // callers pass a baseline
		v[i*2].z = 0.0f;
		v[i*2+1].u = v[i*2].u + 8;
		v[i*2+1].v = v[i*2].v + 8;
		v[i*2+1].c = color;
		v[i*2+1].x = v[i*2].x + 8;
		v[i*2+1].y = v[i*2].y + 8;
		v[i*2+1].z = 0.0f;
	}

	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 128, 128, 128, dbgfont_tex);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexOffset(0.0f, 0.0f);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuEnable(GU_BLEND);      // InitGu(1) (OSK path) leaves blending off
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D, len * 2, 0, v);
	sceGuEnable(GU_DEPTH_TEST);
}

void psp_font_init(void)
{
	InitGu(0);

	dbgfont_init();

	intraFontInit();

	// Load font
    ltn8 = intraFontLoad("flash0:/font/ltn8.pgf"); // small latin sans-serif regular

	// Make sure the fonts are loaded
	psp_use_intrafont = ltn8 ? 1 : 0;
}


/*
 * GUI support code
 *
 */

void gui_PrePrint(void)
{
	sceGuStart(GU_DIRECT, list);

	sceGumMatrixMode(GU_PROJECTION);
	sceGumLoadIdentity();
	sceGumPerspective( 75.0f, 16.0f/9.0f, 0.5f, 1000.0f);

	sceGumMatrixMode(GU_VIEW);
	sceGumLoadIdentity();

	sceGumMatrixMode(GU_MODEL);
	sceGumLoadIdentity();

	sceGuClearColor(0xFF000000);
	sceGuClearDepth(0);
	sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
}

void gui_PostPrint(void)
{
	// End drawing
	sceGuFinish();
	sceGuSync(0,0);

	// Swap buffers (waiting for vsync)
	sceDisplayWaitVblankStart();
	sceGuSwapBuffers();
}

int gui_PrintWidth(char *text)
{
	if (psp_use_intrafont)
		return intraFontMeasureText(ltn8, text);
	else
		return strlen(text)*7;
}

void gui_Print(char *text, u32 fc, u32 bc, int x, int y)
{
	if (psp_use_intrafont)
	{
		intraFontSetStyle(ltn8, 1.0f, fc, bc,0); // scale = 1.0
		intraFontPrint(ltn8, x, y, text);
	}
	else
		dbgfont_print(text, fc, x, y);
}

// XMB-style highlight: translucent white bar behind the selected row,
// fading out at both ends and slowly pulsing
void gui_Highlight(int x0, int x1, int y)
{
	struct HlVertex { u32 color; short x, y, z; } *v;
	u32 a, c0, c1;
	int xs[4], k;
	float ph;

	if (x0 < 0) x0 = 0;
	if (x1 > 480) x1 = 480;
	if (x1 - x0 < 96)
		return;

	ph = (float)(sceKernelGetSystemTimeLow() % 1600000) / 1600000.0f;
	a = 0x38 + (u32)(0x28 * (1.0f + sinf(ph * 2.0f * 3.14159265f)) * 0.5f);
	c0 = 0x00FFFFFF;
	c1 = (a << 24) | 0x00FFFFFF;

	xs[0] = x0; xs[1] = x0 + 48; xs[2] = x1 - 48; xs[3] = x1;

	v = sceGuGetMemory(8 * sizeof(struct HlVertex));
	for (k = 0; k < 4; k++)
	{
		u32 c = (k == 1 || k == 2) ? c1 : c0;
		v[k*2].color = c;   v[k*2].x = xs[k];   v[k*2].y = y - 12; v[k*2].z = 0;
		v[k*2+1].color = c; v[k*2+1].x = xs[k]; v[k*2+1].y = y + 5; v[k*2+1].z = 0;
	}

	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_CULL_FACE); // strip winding would otherwise be culled
	sceGuEnable(GU_BLEND);      // InitGu(1) (OSK path) leaves blending off
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuShadeModel(GU_SMOOTH);
	sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 8, 0, v);
	sceGuEnable(GU_CULL_FACE);
	sceGuEnable(GU_DEPTH_TEST);
}

struct gui_menu {
	char *text;
	unsigned int flags;
	void *field1;
	void *field2;
	unsigned int enable;
};

struct gui_list {
	char *text;
	int index;
};

// gui_menu.flags
#define GUI_DIVIDER   0
#define GUI_TEXT      1
#define GUI_MENU      2
#define GUI_FUNCTION  3
#define GUI_SELECT    4
#define GUI_TOGGLE    5
#define GUI_INTEGER   6
#define GUI_FILE      7

#define GUI_CENTER 0x10000000
#define GUI_LEFT   0x20000000
#define GUI_RIGHT  0x30000000

// gui_menu.enable
#define GUI_DISABLED 0
#define GUI_ENABLED  1
#define GUI_SET_ME   0xFFFFFFFF

// gui_menu.flags = type | alignment
#define GUI_TYPE(flags)  ((flags) & 0x0FFFFFFF)
#define GUI_ALIGN(flags) ((flags) & 0xF0000000)

// misc
#define GUI_END_OF_MENU 0xFFFFFFFF
#define GUI_END_OF_LIST 0xFFFFFFFF

int psp_cpu_speed = 0;

int psp_use_tv = 0;

int psp_lcd_res = 0;
int psp_lcd_sync = 0;
int psp_lcd_aspect = 0; // default to FOV = 90 degrees until fix fov issues
int psp_lcd_detail = 0;

int psp_tv_cable = -1;
int psp_tv_res = 0;
int psp_tv_sync = 0;
int psp_tv_laced = 1;
int psp_tv_aspect = 0;
int psp_tv_detail = 0;
int psp_tv_cx = 0;
int psp_tv_cy = 0;

int psp_snd_upd = 0;
int psp_sfx_enabled = 1;
int psp_music_enabled = 1;

int psp_stick_cx = 128;
int psp_stick_cy = 128;
int psp_stick_minx = 0;
int psp_stick_miny = 0;
int psp_stick_maxx = 255;
int psp_stick_maxy = 255;
int psp_ctrl_cheat[NUM_CHEAT_SLOTS] = { 3, 12, 2, 1, 9, 11, 7, 8, 10, 5, 6, 4 };
int psp_ctrl_swapmove = 0;
int psp_ctrl_swapturn = 0;
int psp_ctrl_run = 0;

char *psp_iwad_file = 0;
char *psp_pwad_file1 = 0;
char *psp_pwad_file2 = 0;
char *psp_pwad_file3 = 0;
char *psp_pwad_file4 = 0;
char *psp_deh_file1 = 0;
char *psp_deh_file2 = 0;
char *psp_deh_file3 = 0;
char *psp_deh_file4 = 0;

int psp_game_nomonsters = 0;
int psp_game_respawn = 0;
int psp_game_fast = 0;
int psp_game_turbo = 0;
int psp_game_maponhu = 0;
int psp_game_rotatemap = 0;
int psp_game_deathmatch = 0; // 0 = co-op, 1 = deathmatch, 2 = alt deathmatch
int psp_game_record = 0;
int psp_game_playdemo = 0;
int psp_game_forcedemo = 0;
int psp_game_timedemo = 0;
int psp_game_timer = 0;
int psp_game_skill = 2; // Hurt Me Plenty (medium)
int psp_game_level = 1;

int psp_net_enabled = 0;
int psp_net_role = 0; // 0 = host, 1 = join
int psp_net_channel = 0; // index into adhoc_channels, mirrors the PSP system setting
int psp_net_extratic = 0;
int psp_net_player1 = 1; // our player number, 1 = host; set by the lobby
int psp_net_error = 0;

// filled by the lobby for psp_net.c: [0] is us, then the other players
int psp_adhoc_numnodes = 0;
unsigned char psp_adhoc_mac[ADHOC_MAXPLAYERS][6];


int gui_menu_len(struct gui_menu *menu)
{
	int c = 0;
	while (menu[c].flags != GUI_END_OF_MENU)
		c++;
	return c;
}

static int gui_skip_row(struct gui_menu *item)
{
	int type = GUI_TYPE(item->flags);
	return type == GUI_DIVIDER || (type == GUI_TEXT && item->enable != GUI_ENABLED);
}

int gui_list_len(struct gui_list *list)
{
	int c = 0;
	while (list[c].index != GUI_END_OF_LIST)
		c++;
	return c;
}

// GUI_INTEGER: add delta within the item's range (field2 = { min, max });
// the delay makes a held d-pad auto-repeat
static void gui_int_step(struct gui_menu *item, int delta)
{
	int *val = (int *)item->field1;
	int *rng = (int *)item->field2;
	int min = rng ? rng[0] : (int)0x80000000;
	int max = rng ? rng[1] : 0x7FFFFFFF;

	sceKernelDelayThread(200*1000);
	// compare before adding so an unbounded value can't wrap around
	if (delta > 0)
		*val = (*val > max - delta) ? max : *val + delta;
	else
		*val = (*val < min - delta) ? min : *val + delta;
}

static int gui_start_requested = 0;

void psp_save_config(void *arg);

// path relative to the game dir, for display (unchanged if outside it)
static const char *psp_rel_path(const char *path)
{
	size_t len = strlen(psp_home);

	return (len && !strncmp(path, psp_home, len)) ? path + len : path;
}

void psp_gui_start(void *arg)
{
	gui_start_requested = 1;
}

// Join / Host > Start: start a network game in that role (arg = psp_net_role)
void psp_gui_start_net(void *arg)
{
	psp_net_role = (int)arg;
	psp_net_enabled = 1;
	gui_start_requested = 1;
}

void do_gui(struct gui_menu *menu, void *menufn, int toplevel);

// wait until none of the buttons in mask is held
static void gui_wait_release(SceCtrlData *pad, u32 mask)
{
	while (pad->Buttons & mask)
		sceCtrlReadBufferPositive(pad, 1);
}

// GUI_SELECT: next (dir = 1) or previous (dir = -1) entry, wrapping around
static void gui_select_step(struct gui_menu *item, int dir)
{
	int len = gui_list_len((struct gui_list *)item->field1);
	int *val = (int *)item->field2;

	*val = (*val + dir + len) % len;
}

// OK pressed on an enabled row
static void gui_item_ok(struct gui_menu *item, SceCtrlData *pad)
{
	void (*fnptr)(void *);
	char temp[PSP_PATH_MAX];
	char *req;

	switch (GUI_TYPE(item->flags))
	{
		case GUI_MENU:
		gui_wait_release(pad, psp_btn_ok);
		do_gui((struct gui_menu *)item->field1, item->field2, 0);
		break;
		case GUI_FUNCTION:
		gui_wait_release(pad, psp_btn_ok);
		fnptr = item->field1;
		(*fnptr)(item->field2);
		break;
		case GUI_TOGGLE:
		gui_wait_release(pad, psp_btn_ok);
		*(int *)item->field1 ^= 1;
		break;
		case GUI_SELECT:
		// with OK held: right/down = next, left/up = previous
		if (pad->Buttons & (PSP_CTRL_RIGHT | PSP_CTRL_DOWN))
		{
			gui_wait_release(pad, PSP_CTRL_RIGHT | PSP_CTRL_DOWN);
			gui_select_step(item, 1);
		}
		if (pad->Buttons & (PSP_CTRL_LEFT | PSP_CTRL_UP))
		{
			gui_wait_release(pad, PSP_CTRL_LEFT | PSP_CTRL_UP);
			gui_select_step(item, -1);
		}
		break;
		case GUI_FILE:
		gui_wait_release(pad, psp_btn_ok);
		snprintf(temp, sizeof(temp), "%s%s/", psp_home, (char *)item->field2);
		req = RequestFile(temp);
		// cancel clears patch WAD/DEH slots, but keeps the main WAD
		if (req || strcmp((char *)item->field2, "iwad"))
		{
			if (*(int *)item->field1)
				free(*(char **)item->field1);
			*(char **)item->field1 = req ? strdup(req) : 0;
		}
		break;
		case GUI_INTEGER:
		// with OK held: left/right step by 1, down/up by 10
		if (pad->Buttons & PSP_CTRL_RIGHT)
			gui_int_step(item, 1);
		if (pad->Buttons & PSP_CTRL_LEFT)
			gui_int_step(item, -1);
		if (pad->Buttons & PSP_CTRL_DOWN)
			gui_int_step(item, 10);
		if (pad->Buttons & PSP_CTRL_UP)
			gui_int_step(item, -10);
		break;
	}
}

// left/right pressed on an enabled row
static void gui_item_adjust(struct gui_menu *item, SceCtrlData *pad)
{
	int dir = (pad->Buttons & PSP_CTRL_RIGHT) ? 1 : -1;

	switch (GUI_TYPE(item->flags))
	{
		case GUI_TOGGLE:
		gui_wait_release(pad, PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
		*(int *)item->field1 ^= 1;
		break;
		case GUI_SELECT:
		gui_wait_release(pad, PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
		gui_select_step(item, dir);
		break;
		case GUI_INTEGER:
		gui_int_step(item, dir);
		break;
	}
}

// one frame: the hint lines, then the rows from msy down, csel highlighted
static void gui_draw(struct gui_menu *menu, int mlen, int msy, int csel)
{
	u32 fc = 0xFFFFFFFF, bc = 0x00000000;
	int i, tx, ty;
	char line[256];

	sceDisplayWaitVblankStart();
	gui_PrePrint();

	switch (GUI_TYPE(menu[csel].flags))
	{
		case GUI_SELECT:
		case GUI_TOGGLE:
		case GUI_INTEGER:
		if (menu[csel].enable == GUI_ENABLED)
		{
			gui_Print("Left/Right = Change Value", fc, bc, 14, 264);
			break;
		}
		// fall through
		default:
		gui_Print("START = Start DOOM", fc, bc, 14, 264);
		break;
	}
	gui_Print("SELECT = Save Config", fc, bc, 14, 248);
	strcpy(line, psp_btn_swap ? "X/O = Enter/Back" : "O/X = Enter/Back");
	gui_Print(line, fc, bc, 466 - gui_PrintWidth(line), 264);

	for (i=0; i<mlen; i++)
	{
		char temp[16];

		bc = 0x00000000;
		if (GUI_TYPE(menu[i].flags) == GUI_DIVIDER)
			continue;
		if (menu[i].enable == GUI_ENABLED)
			fc = (i==csel) ? 0xFFFFFFFF : 0xFFCCCCCC;
		else
			fc = 0xFFAAAAAA;

		snprintf(line, sizeof(line), "%s", menu[i].text);
		switch (GUI_TYPE(menu[i].flags))
		{
			case GUI_SELECT:
			strcat(line, " : ");
			strcat(line, ((struct gui_list *)menu[i].field1)[*(int *)menu[i].field2].text);
			break;
			case GUI_TOGGLE:
			strcat(line, " : ");
			strcat(line, *(int *)menu[i].field1 ? "on" : "off");
			break;
			case GUI_INTEGER:
			strcat(line, " : ");
			snprintf(temp, sizeof(temp), "%d", *(int *)menu[i].field1);
			strcat(line, temp);
			break;
			case GUI_FILE:
			if (menu[i].text[0])
				strcat(line, " : ");
			else if (!*(int *)menu[i].field1)
				strcat(line, "(none)"); // unlabeled path row
			strncat(line, *(int *)menu[i].field1 ? psp_rel_path(*(char **)menu[i].field1) : "", sizeof(line) - strlen(line) - 1);
			break;
			case GUI_TEXT:
			if ((int)menu[i].field1)
				fc = (u32)menu[i].field1 | 0xFF000000;
			if ((int)menu[i].field2)
				bc = (u32)menu[i].field2 | 0xFF000000;
			break;
		}

		ty = msy + i * 16;
		switch (GUI_ALIGN(menu[i].flags))
		{
			case GUI_LEFT:
			tx = 7;
			break;
			case GUI_RIGHT:
			tx = 473 - gui_PrintWidth(line);
			break;
			case GUI_CENTER:
			default:
			tx = 240 - gui_PrintWidth(line) / 2;
		}
		if (i == csel)
			gui_Highlight(tx - 60, tx + gui_PrintWidth(line) + 60, ty);
		gui_Print(line, fc, bc, tx, ty);
	}
	gui_PostPrint();
}

void do_gui(struct gui_menu *menu, void *menufn, int toplevel)
{
	SceCtrlData pad;
	u32 prev_buttons;
	int msy, mlen;
	int i;
	int csel = 0;

	mlen = gui_menu_len(menu);
	msy = 136 - mlen*8;
	// keep the last row clear of the two hint lines at the bottom
	if (msy + (mlen - 1) * 16 > 234)
		msy = 234 - (mlen - 1) * 16;

	// start on the first selectable row (on "Start" in the top menu)
	for (i = 0; i < mlen && gui_skip_row(&menu[i]); i++);
	csel = (i < mlen) ? i : 0;
	if (toplevel)
		for (i = 0; i < mlen; i++)
			if (menu[i].field1 == (void *)&psp_gui_start)
				csel = i;

	psp_text_screen();

	sceCtrlReadBufferPositive(&pad, 1);
	prev_buttons = pad.Buttons;
	// O backs out of sub-menus only; the game is started with Start/START
	while (!gui_start_requested && (toplevel || !(pad.Buttons & psp_btn_back)))
	{
		void (*fnptr)(struct gui_menu *);
		fnptr = menufn;
		if (fnptr)
			(*fnptr)(menu);
		if (pad.Buttons & psp_btn_ok)
		{
			if (menu[csel].enable == GUI_ENABLED)
				gui_item_ok(&menu[csel], &pad);
			// functions/sub-menus may leave buttons held (e.g. START confirms
			// the OSK), so only count presses made after they return
			sceCtrlReadBufferPositive(&pad, 1);
			prev_buttons = pad.Buttons;
		}
		else
		{
			if (pad.Buttons & PSP_CTRL_UP)
			{
				while (pad.Buttons & PSP_CTRL_UP)
					sceCtrlReadBufferPositive(&pad, 1);
				i = 0;
				do
					csel = (csel == 0) ? mlen - 1 : csel - 1;
				while (gui_skip_row(&menu[csel]) && ++i < mlen);
			}
			if (pad.Buttons & PSP_CTRL_DOWN)
			{
				while (pad.Buttons & PSP_CTRL_DOWN)
					sceCtrlReadBufferPositive(&pad, 1);
				i = 0;
				do
					csel = (csel == (mlen - 1)) ? 0 : csel + 1;
				while (gui_skip_row(&menu[csel]) && ++i < mlen);
			}
			if ((pad.Buttons & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) && menu[csel].enable == GUI_ENABLED)
				gui_item_adjust(&menu[csel], &pad);
		}

		gui_draw(menu, mlen, msy, csel);

		sceKernelDelayThread(20*1000);
		sceCtrlReadBufferPositive(&pad, 1);
		if ((pad.Buttons & PSP_CTRL_START) && !(prev_buttons & PSP_CTRL_START))
		{
			// START launches the game from any menu level,
			// as host from the Host menu
			while (pad.Buttons & PSP_CTRL_START)
				sceCtrlReadBufferPositive(&pad, 1);
			if (menu[0].field1 == (void *)&psp_gui_start_net)
				psp_gui_start_net(menu[0].field2);
			gui_start_requested = 1;
		}
		if ((pad.Buttons & PSP_CTRL_SELECT) && !(prev_buttons & PSP_CTRL_SELECT))
		{
			// SELECT saves config from any menu level
			while (pad.Buttons & PSP_CTRL_SELECT)
				sceCtrlReadBufferPositive(&pad, 1);
			psp_save_config(0);
			// buttons used to close the OSK (START, X/O) must not act here
			do
				sceCtrlReadBufferPositive(&pad, 1);
			while (pad.Buttons & (psp_btn_ok | psp_btn_back));
		}
		prev_buttons = pad.Buttons;
		if (quit_requested)
			sceKernelExitGame();
	}
	while (pad.Buttons & psp_btn_back)
		sceCtrlReadBufferPositive(&pad, 1);

	psp_text_screen();
}

void set_myargv(void);
static int psp_file_exists(const char *path);
static char *psp_find_file(const char *dir, const char *name);
static void psp_fix_path(char **file, const char *subdir);

char psp_cfg_status[PSP_PATH_MAX] = "(none)"; // path of current config file, relative to the game dir

// Launcher config: an INI file ("[section]", "key = value", "#" comments)
// mapped onto the launcher variables by cfg_table, which drives both
// loading and saving.
enum { CFG_INT, CFG_ENUM, CFG_PATH };

struct cfg_entry {
	const char *section;
	const char *key;
	int type;
	void *ptr;			// int *, or char ** for CFG_PATH
	const char **names;	// CFG_ENUM: value names, by index; CFG_PATH: { subdir }
	int def;			// value when the file doesn't set it
	int min, max;		// CFG_INT: allowed range, both 0 = on/off
};

static const char *cfg_output_names[] = { "lcd", "tv", 0 };
static const char *cfg_lcd_res_names[] = { "480x272", "368x272", "320x240", 0 };
static const char *cfg_tv_res_names[] = { "720x480", "704x448", "640x400", 0 };
static const char *cfg_music_rate_names[] = { "140", "70", "35", 0 };
static const char *cfg_cpu_names[] = { "default", "133", "222", "266", "300", "333", 0 };
static const char *cfg_mode_names[] = { "coop", "deathmatch", "altdeath", 0 };
static const char *cfg_skill_names[] = { "1", "2", "3", "4", "5", 0 };
static const char *cfg_iwad_dir[] = { "iwad" };
static const char *cfg_pwad_dir[] = { "pwad" };
static const char *cfg_deh_dir[] = { "deh" };

static struct cfg_entry cfg_table[] = {
	{ "video", "output", CFG_ENUM, &psp_use_tv, cfg_output_names, 0 },
	{ "video", "lcd_res", CFG_ENUM, &psp_lcd_res, cfg_lcd_res_names, 0 },
	{ "video", "lcd_vsync", CFG_INT, &psp_lcd_sync, 0, 0 },
	{ "video", "lcd_widescreen", CFG_INT, &psp_lcd_aspect, 0, 0 },
	{ "video", "lcd_lowdetail", CFG_INT, &psp_lcd_detail, 0, 0 },
	{ "video", "tv_res", CFG_ENUM, &psp_tv_res, cfg_tv_res_names, 0 },
	{ "video", "tv_vsync", CFG_INT, &psp_tv_sync, 0, 0 },
	{ "video", "tv_interlaced", CFG_INT, &psp_tv_laced, 0, 1 },
	{ "video", "tv_widescreen", CFG_INT, &psp_tv_aspect, 0, 0 },
	{ "video", "tv_lowdetail", CFG_INT, &psp_tv_detail, 0, 0 },
	{ "video", "tv_cx", CFG_INT, &psp_tv_cx, 0, 0, 0, 80 },
	{ "video", "tv_cy", CFG_INT, &psp_tv_cy, 0, 0, 0, 80 },

	{ "sound", "sfx", CFG_INT, &psp_sfx_enabled, 0, 1 },
	{ "sound", "music", CFG_INT, &psp_music_enabled, 0, 1 },
	{ "sound", "music_rate", CFG_ENUM, &psp_snd_upd, cfg_music_rate_names, 0 },

	{ "system", "cpu", CFG_ENUM, &psp_cpu_speed, cfg_cpu_names, 0 },

	{ "files", "iwad", CFG_PATH, &psp_iwad_file, cfg_iwad_dir, 0 },
	{ "files", "pwad1", CFG_PATH, &psp_pwad_file1, cfg_pwad_dir, 0 },
	{ "files", "pwad2", CFG_PATH, &psp_pwad_file2, cfg_pwad_dir, 0 },
	{ "files", "pwad3", CFG_PATH, &psp_pwad_file3, cfg_pwad_dir, 0 },
	{ "files", "pwad4", CFG_PATH, &psp_pwad_file4, cfg_pwad_dir, 0 },
	{ "files", "deh1", CFG_PATH, &psp_deh_file1, cfg_deh_dir, 0 },
	{ "files", "deh2", CFG_PATH, &psp_deh_file2, cfg_deh_dir, 0 },
	{ "files", "deh3", CFG_PATH, &psp_deh_file3, cfg_deh_dir, 0 },
	{ "files", "deh4", CFG_PATH, &psp_deh_file4, cfg_deh_dir, 0 },

	{ "game", "mode", CFG_ENUM, &psp_game_deathmatch, cfg_mode_names, 0 },
	{ "game", "skill", CFG_ENUM, &psp_game_skill, cfg_skill_names, 2 },
	{ "game", "map", CFG_INT, &psp_game_level, 0, 1, 1, 36 },
	{ "game", "nomonsters", CFG_INT, &psp_game_nomonsters, 0, 0 },
	{ "game", "respawn", CFG_INT, &psp_game_respawn, 0, 0 },
	{ "game", "fast", CFG_INT, &psp_game_fast, 0, 0 },
	{ "game", "turbo", CFG_INT, &psp_game_turbo, 0, 0 },
	{ "game", "timer", CFG_INT, &psp_game_timer, 0, 0, 0, 1440 },
	{ "game", "map_on_hud", CFG_INT, &psp_game_maponhu, 0, 0 },
	{ "game", "rotate_map", CFG_INT, &psp_game_rotatemap, 0, 0 },
	{ "game", "record_demo", CFG_INT, &psp_game_record, 0, 0, 0, 99 },
	{ "game", "play_demo", CFG_INT, &psp_game_playdemo, 0, 0, 0, 99 },
	{ "game", "time_demo", CFG_INT, &psp_game_timedemo, 0, 0, 0, 99 },
	{ "game", "force_demo", CFG_INT, &psp_game_forcedemo, 0, 0 },

	{ "controls", "analog_cx", CFG_INT, &psp_stick_cx, 0, 128, 0, 255 },
	{ "controls", "analog_cy", CFG_INT, &psp_stick_cy, 0, 128, 0, 255 },
	{ "controls", "analog_minx", CFG_INT, &psp_stick_minx, 0, 0, 0, 255 },
	{ "controls", "analog_miny", CFG_INT, &psp_stick_miny, 0, 0, 0, 255 },
	{ "controls", "analog_maxx", CFG_INT, &psp_stick_maxx, 0, 255, 0, 255 },
	{ "controls", "analog_maxy", CFG_INT, &psp_stick_maxy, 0, 255, 0, 255 },
	{ "controls", "swap_move", CFG_INT, &psp_ctrl_swapmove, 0, 0 },
	{ "controls", "swap_turn", CFG_INT, &psp_ctrl_swapturn, 0, 0 },
	{ "controls", "always_run", CFG_INT, &psp_ctrl_run, 0, 0 },

	{ "cheats", "slot1", CFG_INT, &psp_ctrl_cheat[0], 0, 3, 0, 12 },
	{ "cheats", "slot2", CFG_INT, &psp_ctrl_cheat[1], 0, 12, 0, 12 },
	{ "cheats", "slot3", CFG_INT, &psp_ctrl_cheat[2], 0, 2, 0, 12 },
	{ "cheats", "slot4", CFG_INT, &psp_ctrl_cheat[3], 0, 1, 0, 12 },
	{ "cheats", "slot5", CFG_INT, &psp_ctrl_cheat[4], 0, 9, 0, 12 },
	{ "cheats", "slot6", CFG_INT, &psp_ctrl_cheat[5], 0, 11, 0, 12 },
	{ "cheats", "slot7", CFG_INT, &psp_ctrl_cheat[6], 0, 7, 0, 12 },
	{ "cheats", "slot8", CFG_INT, &psp_ctrl_cheat[7], 0, 8, 0, 12 },
	{ "cheats", "slot9", CFG_INT, &psp_ctrl_cheat[8], 0, 10, 0, 12 },
	{ "cheats", "slot10", CFG_INT, &psp_ctrl_cheat[9], 0, 5, 0, 12 },
	{ "cheats", "slot11", CFG_INT, &psp_ctrl_cheat[10], 0, 6, 0, 12 },
	{ "cheats", "slot12", CFG_INT, &psp_ctrl_cheat[11], 0, 4, 0, 12 },

	{ "network", "extratic", CFG_INT, &psp_net_extratic, 0, 0 },

	{ 0 }
};

// strips blanks and the line end, in place
static char *cfg_trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && strchr(" \t\r\n", end[-1]))
		end--;
	*end = 0;
	return s;
}

static void cfg_set(struct cfg_entry *e, char *value)
{
	int i;
	char temp[PSP_PATH_MAX];

	// a path may hold a '#', anything else may be followed by a comment
	if (e->type != CFG_PATH && strchr(value, '#'))
	{
		*strchr(value, '#') = 0;
		value = cfg_trim(value);
	}
	if (!value[0])
		return;

	switch (e->type)
	{
		case CFG_INT:
		// not a number (old style config, typo): keep the default
		if (sscanf(value, "%d", &i) == 1)
		{
			int max = e->max ? e->max : 1;

			*(int *)e->ptr = i < e->min ? e->min : i > max ? max : i;
		}
		break;
		case CFG_ENUM:
		for (i = 0; e->names[i]; i++)
			if (!strcasecmp(value, e->names[i]))
				*(int *)e->ptr = i;
		break;
		case CFG_PATH:
		// no device or leading slash: relative to the game dir
		if (!strchr(value, ':') && value[0] != '/')
		{
			snprintf(temp, sizeof(temp), "%s%s", psp_home, value);
			value = temp;
		}
		free(*(char **)e->ptr);
		*(char **)e->ptr = strdup(value);
		break;
	}
}

// Returns 0 if the file can't be opened, leaving the settings alone.
// Anything not understood in the file is skipped.
static int psp_cfg_read(const char *path)
{
	struct cfg_entry *e;
	FILE *handle;
	char line[PSP_PATH_MAX + 64];
	char section[32] = "";
	char *key, *value;
	char *iwad;

	handle = fopen (path, "r");
	if (handle == NULL)
		return 0;

	// the file only has to name what differs from the defaults,
	// except for the IWAD: no (valid) one keeps the current one
	iwad = psp_iwad_file;
	psp_iwad_file = 0;
	for (e = cfg_table; e->key; e++)
		if (e->type == CFG_PATH)
		{
			free(*(char **)e->ptr);
			*(char **)e->ptr = 0;
		}
		else
			*(int *)e->ptr = e->def;

	while (fgets(line, sizeof(line), handle))
	{
		key = cfg_trim(line);
		if (key[0] == '[' && strchr(key, ']'))
		{
			*strchr(key, ']') = 0;
			snprintf(section, sizeof(section), "%s", cfg_trim(key + 1));
			continue;
		}
		value = strchr(key, '=');
		if (key[0] == '#' || key[0] == ';' || !value)
			continue;
		*value++ = 0;
		key = cfg_trim(key);
		for (e = cfg_table; e->key; e++)
			if (!strcasecmp(section, e->section) && !strcasecmp(key, e->key))
				cfg_set(e, cfg_trim(value));
	}

	fclose (handle);

	// paths go stale when the game dir moves or a file is removed
	for (e = cfg_table; e->key; e++)
		if (e->type == CFG_PATH)
			psp_fix_path((char **)e->ptr, e->names[0]);

	if (psp_iwad_file)
		free(iwad);
	else
		psp_iwad_file = iwad;

	// a network game is only started from Join or Host > Start
	psp_net_enabled = 0;

	return 1;
}

static int psp_cfg_write(const char *path)
{
	struct cfg_entry *e;
	FILE *handle;
	const char *section = "";
	int i;

	handle = fopen (path, "w");
	if (handle == NULL)
		return 0;

	fprintf(handle, "# DOOM PSP launcher config\n");

	for (e = cfg_table; e->key; e++)
	{
		if (strcmp(section, e->section))
		{
			section = e->section;
			fprintf(handle, "\n[%s]\n", section);
		}

		switch (e->type)
		{
			case CFG_INT:
			fprintf(handle, "%s = %d\n", e->key, *(int *)e->ptr);
			break;
			case CFG_ENUM:
			// value, then the possible ones as a comment
			fprintf(handle, "%s = %s  #", e->key, e->names[*(int *)e->ptr]);
			for (i = 0; e->names[i]; i++)
				fprintf(handle, "%s %s", i ? " |" : "", e->names[i]);
			fprintf(handle, "\n");
			break;
			case CFG_PATH:
			// inside the game dir: stored relative to it, so the dir can move
			fprintf(handle, "%s = %s\n", e->key,
				*(char **)e->ptr ? psp_rel_path(*(char **)e->ptr) : "");
			break;
		}
	}

	fclose (handle);
	return 1;
}

void psp_load_defaults()
{
	int i;
	char temp[PSP_PATH_MAX];

	// config/default.cfg in the launch dir, then relative to the current dir
	for (i = 0; i < 2; i++)
	{
		snprintf(temp, sizeof(temp), "%sconfig/default.cfg", i ? "" : psp_home);
		if (psp_cfg_read(temp))
		{
			snprintf(psp_cfg_status, sizeof(psp_cfg_status), "%s", psp_rel_path(temp));
			return;
		}
	}
}

void psp_load_config(void *arg)
{
	char *req;
	char dir[PSP_PATH_MAX];

	snprintf(dir, sizeof(dir), "%sconfig/", psp_home);
	req = RequestFile(dir);
	if (!req)
		return;	// requester cancelled: keep current config

	psp_text_screen();

	printf("Attempting to load config from %s\n\n", req);

	if (!psp_cfg_read(req))
	{
		printf("Error! Couldn't open file %s\n\n", req);
		sceKernelDelayThread(2*1000*1000);
		return;
	}

	snprintf(psp_cfg_status, sizeof(psp_cfg_status), "%s", psp_rel_path(req));

	printf("\nConfig loaded\n\n");
	sceKernelDelayThread(3*1000*1000);
	pspDebugScreenClear();
}

void psp_save_config(void *arg)
{
	int ok, i;
	char filename[64];
	unsigned short intext[128]  = { 'd', 'e', 'f', 'a', 'u', 'l', 't', 0 }; // text already in the edit box on start
	unsigned short desc[128]	= { 'E', 'n', 't', 'e', 'r', ' ', 'F', 'i', 'l', 'e', ' ', 'N', 'a', 'm', 'e', 0 }; // description
	char *slash = strrchr(psp_cfg_status, '/');

	// start with the name of the current config, if it lives in config/
	// ("(none)" keeps default); .cfg is left out, it's added on save
	if (slash && slash[1] && slash - psp_cfg_status >= 6 && !strncmp(slash - 6, "config", 6)
		&& (slash - psp_cfg_status == 6 || slash[-7] == '/'))
	{
		for (i = 0; i < 50 && slash[1 + i]; i++)
			intext[i] = (unsigned char)slash[1 + i];
		if (i > 4 && !strcasecmp(slash + 1 + i - 4, ".cfg"))
			i -= 4;
		intext[i] = 0;
	}

	ok = get_text_osk(filename, intext, desc);

	psp_text_screen();

	if (ok < 0)
		return;	// keyboard cancelled: don't save

	if (ok && filename[0])
	{
		char temp[PSP_PATH_MAX];

		// doesn't end in .cfg: add it
		i = strlen(filename);
		snprintf(temp, sizeof(temp), "%sconfig/%s%s", psp_home, filename,
			(i >= 4 && !strcasecmp(filename + i - 4, ".cfg")) ? "" : ".cfg");

		printf("Attempting to save config to %s\n\n", temp);

		if (!psp_cfg_write(temp))
		{
			printf("Error! Couldn't open file %s\n\n", temp);
			sceKernelDelayThread(2*1000*1000);
			return;
		}

		snprintf(psp_cfg_status, sizeof(psp_cfg_status), "%s", psp_rel_path(temp));
		printf("Config saved to %s\n\n", temp);
		sceKernelDelayThread(2*1000*1000);
	}
	else
	{
		printf("You need to enter a filename to save!\n\n");
		sceKernelDelayThread(2*1000*1000);
	}
	pspDebugScreenClear();
}

// width, height by psp_lcd_res / psp_tv_res (same order as the cfg names)
static const int lcd_res[3][2] = { { 480, 272 }, { 368, 272 }, { 320, 240 } };
static const int tv_res[3][2] = { { 720, 480 }, { 704, 448 }, { 640, 400 } };

static void drawLine(int inX0, int inY0, int inX1, int inY1, u32 inColor, u32* inDestination, int inWidth)
{
     int tempDY = inY1 - inY0;
     int tempDX = inX1 - inX0;
     int tempStepX, tempStepY;
     int inZ;

     if(tempDY < 0) {
          tempDY = -tempDY;  tempStepY = -inWidth;
     } else {
          tempStepY = inWidth;
     }

     if(tempDX < 0) {
          tempDX = -tempDX;  tempStepX = -1;
     } else {
          tempStepX = 1;
     }

     tempDY <<= 1;
     tempDX <<= 1;

     inY0 *= inWidth;
     inY1 *= inWidth;
     inDestination[inX0 + inY0] = inColor;
     if(tempDX > tempDY) {
          int tempFraction = tempDY - (tempDX >> 1);
          while(inX0 != inX1) {
               if(tempFraction >= 0) {
                    inY0 += tempStepY;
                    tempFraction -= tempDX;
               }
               inX0 += tempStepX;
               tempFraction += tempDY;
               if (!psp_tv_laced)
                    inDestination[inX0 + inY0] = inColor;
               else {
                    inZ = inY0 / inWidth;
                    if (inZ & 1)
                         inDestination[inX0 + (inZ>>1) * inWidth] = inColor;
                    else
                         inDestination[inX0 + ((inZ>>1) + 262) * inWidth] = inColor;
               }
          }
     } else {
          int tempFraction = tempDX - (tempDY >> 1);
          while(inY0 != inY1) {
               if(tempFraction >= 0) {
                    inX0 += tempStepX;
                    tempFraction -= tempDY;
               }
               inY0 += tempStepY;
               tempFraction += tempDX;
               if (!psp_tv_laced)
                    inDestination[inX0 + inY0] = inColor;
               else {
                    inZ = inY0 / inWidth;
                    if (inZ & 1)
                         inDestination[inX0 + (inZ>>1) * inWidth] = inColor;
                    else
                         inDestination[inX0 + ((inZ>>1) + 262) * inWidth] = inColor;
               }
          }
     }
}

void psp_tv_center(void *arg)
{
	SceCtrlData pad;
	int cx, cy, mx, my, h, w;

	if (psp_tv_cable > 0)
	{
		psp_tv_mode(psp_tv_cable, psp_tv_laced);

		sceDisplaySetFrameBuf((void *)0x44000000, 768, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);

		w = tv_res[psp_tv_res][0];
		h = tv_res[psp_tv_res][1];
		mx = 720 - w;
		my = 480 - h;
		cx = psp_tv_cx > mx ? mx : psp_tv_cx;
		cy = psp_tv_cy > my ? my : psp_tv_cy;


		sceCtrlReadBufferPositive(&pad, 1);
		while (!(pad.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)))
		{
			if (pad.Buttons & PSP_CTRL_UP)
				cy = cy > 0 ? cy-1 : 0;
			if (pad.Buttons & PSP_CTRL_DOWN)
				cy = cy < my ? cy+1 : my;
			if (pad.Buttons & PSP_CTRL_LEFT)
				cx = cx > 0 ? cx-1 : 0;
			if (pad.Buttons & PSP_CTRL_RIGHT)
				cx = cx < mx ? cx+1 : mx;

			sceDisplayWaitVblankStart();
			memset((void *)0x44000000, 0, 503*768*4); // clear screen

			drawLine(cx, cy, cx+w-1, cy, 0xFFFFFF, (u32 *)0x44000000, 768);
			drawLine(cx+w-1, cy, cx+w-1, cy+h-1, 0xFFFFFF, (u32 *)0x44000000, 768);
			drawLine(cx+w-1, cy+h-1, cx, cy+h-1, 0xFFFFFF, (u32 *)0x44000000, 768);
			drawLine(cx, cy+h-1, cx, cy, 0xFFFFFF, (u32 *)0x44000000, 768);
			drawLine(cx, cy, cx+w-1, cy+h-1, 0xFFFFFF, (u32 *)0x44000000, 768);
			drawLine(cx, cy+h-1, cx+w-1, cy, 0xFFFFFF, (u32 *)0x44000000, 768);

			sceKernelDelayThread(100*1000);
			sceCtrlReadBufferPositive(&pad, 1);
		}
		while (pad.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS))
			sceCtrlReadBufferPositive(&pad, 1);

		psp_tv_cx = cx;
		psp_tv_cy = cy;

		pspDveMgrSetVideoOut(0, 0, 480, 272, 1, 15, 0); // LCD
		psp_text_screen();
	}
}

void psp_stick_calibrate(void *arg)
{
	SceCtrlData pad;
	int cx, cy, mx, my, Mx, My;
	u32 prev;
	u32 *addr;

	cx = cy = mx = my = Mx = My = 128;

	psp_text_screen();
	printf("    Move the stick to the corners, release it, then press X or O\n");

	drawLine(112, 8, 112+255, 8, 0xFFFFFF, (u32 *)0x44000000, 512);
	drawLine(112+255, 8, 112+255, 8+255, 0xFFFFFF, (u32 *)0x44000000, 512);
	drawLine(112+255, 8+255, 112, 8+255, 0xFFFFFF, (u32 *)0x44000000, 512);
	drawLine(112, 8+255, 112, 8, 0xFFFFFF, (u32 *)0x44000000, 512);

	addr = (u32 *)(0x44000000 + (112+cx)*4 +(8+cy)*512*4);
	prev = *addr;

	sceCtrlReadBufferPositive(&pad, 1);
	while (!(pad.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)))
	{
		sceDisplayWaitVblankStart();
		*addr = prev;

		sceCtrlReadBufferPositive(&pad, 1);
		cx = pad.Lx;
		cy = pad.Ly;
		if (cx<mx) mx = cx;
		if (cx>Mx) Mx = cx;
		if (cy<my) my = cy;
		if (cy>My) My = cy;

		addr = (u32 *)(0x44000000 + (112+cx)*4 +(8+cy)*512*4);
		prev = *addr;
		*addr = (u32)0xFFFFFF;

		pspDebugScreenSetXY(0, 4);
		printf("Current");
		pspDebugScreenSetXY(0, 5);
		printf(" %02X %02X", cx, cy);
		pspDebugScreenSetXY(0, 7);
		printf("Minimum");
		pspDebugScreenSetXY(0, 8);
		printf(" %02X %02X", mx, my);
		pspDebugScreenSetXY(0, 10);
		printf("Maximum");
		pspDebugScreenSetXY(0, 11);
		printf(" %02X %02X", Mx, My);
	}
	while (pad.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS))
		sceCtrlReadBufferPositive(&pad, 1);

	pspDebugScreenClear();

	psp_stick_cx = cx;
	psp_stick_cy = cy;
	psp_stick_minx = mx;
	psp_stick_miny = my;
	psp_stick_maxx = Mx;
	psp_stick_maxy = My;
}

void psp_gui(void)
{
	struct gui_menu AboutLevel[] = {
		{ "Doom for the PSP", GUI_CENTER | GUI_TEXT, (void *)0xFFFFFF, 0, GUI_DISABLED },
		{ "by Chilly Willy", GUI_CENTER | GUI_TEXT, (void *)0xFFFFFF, 0, GUI_DISABLED },
		{ "based on ADoomPPC v1.7 by Jarmo Laakkonen", GUI_CENTER | GUI_TEXT, (void *)0x88FF88, 0, GUI_DISABLED },
		{ "based on ADoomPPC v1.3 by Joseph Fenton", GUI_CENTER | GUI_TEXT, (void *)0x88FF88, 0, GUI_DISABLED },
		{ "based on ADoom v1.2 by Peter McGavin", GUI_CENTER | GUI_TEXT, (void *)0x88FF88, 0, GUI_DISABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Alternate MIDI Instruments by Zer0-X/o'Moses", GUI_CENTER | GUI_TEXT, (void *)0x8888FF, 0, GUI_DISABLED },
		{ "based on samples from Christian Buchner's GMPlay V1.3", GUI_CENTER | GUI_TEXT, (void *)0x8888FF, 0, GUI_DISABLED },
		{ "and the original MIDI Instruments by Joseph Fenton", GUI_CENTER | GUI_TEXT, (void *)0x8888FF, 0, GUI_DISABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Uses intraFont by BenHur", GUI_CENTER | GUI_TEXT, (void *)0xFF8888, 0, GUI_DISABLED },
		{ "TV-out support lib thanks to Dark_AleX", GUI_CENTER | GUI_TEXT, (void *)0xFFFFFF, 0, GUI_DISABLED },
		{ "Sleep, launcher, ad-hoc, and control improvements by Subnixr", GUI_CENTER | GUI_TEXT, (void *)0xFFFFFF, 0, GUI_DISABLED },
        { "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	// same order as adhoc_channels
	struct gui_list net_channel_list[] = {
		{ "Automatic", 0 },
		{ "1", 1 },
		{ "6", 2 },
		{ "11", 3 },
		{ 0, GUI_END_OF_LIST }
	};

	// same limits as cfg_table
	int game_level_range[2] = { 1, 36 };
	int game_timer_range[2] = { 0, 1440 };
	int game_demo_range[2] = { 0, 99 };

	struct gui_list game_skill_list[] = {
		{ "I'm Too Young To Die", 0 },
		{ "Hey, Not Too Rough", 1 },
		{ "Hurt Me Plenty", 2 },
		{ "Ultra-Violence", 3 },
		{ "Nightmare!", 4 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list game_mode_list[] = {
		{ "Co-op", 0 },
		{ "Deathmatch", 1 },
		{ "Alt Deathmatch", 2 },
		{ 0, GUI_END_OF_LIST }
	};

	// the host's settings apply to every player
	struct gui_menu HostLevel[] = {
		{ "Start", GUI_CENTER | GUI_FUNCTION, &psp_gui_start_net, (void *)0, GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Game Mode", GUI_CENTER | GUI_SELECT, &game_mode_list, &psp_game_deathmatch, GUI_ENABLED },
		{ "Timed Game", GUI_CENTER | GUI_INTEGER, &psp_game_timer, &game_timer_range, GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Starting Skill Level", GUI_CENTER | GUI_SELECT, &game_skill_list, &psp_game_skill, GUI_ENABLED },
		{ "Starting Map Level", GUI_CENTER | GUI_INTEGER, &psp_game_level, &game_level_range, GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		// same settings as in Configure > Game
		{ "No Monsters", GUI_CENTER | GUI_TOGGLE, &psp_game_nomonsters, 0, GUI_ENABLED },
		{ "Respawn", GUI_CENTER | GUI_TOGGLE, &psp_game_respawn, 0, GUI_ENABLED },
		{ "Fast", GUI_CENTER | GUI_TOGGLE, &psp_game_fast, 0, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_menu NetLevel[] = {
		{ "Ad-Hoc Channel", GUI_CENTER | GUI_SELECT, &net_channel_list, &psp_net_channel, GUI_ENABLED },
		{ "Network Extra Tic", GUI_CENTER | GUI_TOGGLE, &psp_net_extratic, 0, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_menu GameLevel[] = {
		{ "No Monsters", GUI_CENTER | GUI_TOGGLE, &psp_game_nomonsters, 0, GUI_ENABLED },
		{ "Respawn", GUI_CENTER | GUI_TOGGLE, &psp_game_respawn, 0, GUI_ENABLED },
		{ "Fast", GUI_CENTER | GUI_TOGGLE, &psp_game_fast, 0, GUI_ENABLED },
		{ "Turbo", GUI_CENTER | GUI_TOGGLE, &psp_game_turbo, 0, GUI_ENABLED },
		{ "Map on HU", GUI_CENTER | GUI_TOGGLE, &psp_game_maponhu, 0, GUI_ENABLED },
		{ "Rotate Map", GUI_CENTER | GUI_TOGGLE, &psp_game_rotatemap, 0, GUI_ENABLED },
		{ "Force Demo", GUI_CENTER | GUI_TOGGLE, &psp_game_forcedemo, 0, GUI_ENABLED },
		{ "Play Demo", GUI_CENTER | GUI_INTEGER, &psp_game_playdemo, &game_demo_range, GUI_ENABLED },
		{ "Time Demo", GUI_CENTER | GUI_INTEGER, &psp_game_timedemo, &game_demo_range, GUI_ENABLED },
		{ "Record Demo", GUI_CENTER | GUI_INTEGER, &psp_game_record, &game_demo_range, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_menu FileLevel[] = {
		{ "Main WAD", GUI_LEFT | GUI_FILE, &psp_iwad_file, "iwad", GUI_ENABLED },
		{ "Patch WAD", GUI_LEFT | GUI_FILE, &psp_pwad_file1, "pwad", GUI_ENABLED },
		{ "Patch WAD", GUI_LEFT | GUI_FILE, &psp_pwad_file2, "pwad", GUI_ENABLED },
		{ "Patch WAD", GUI_LEFT | GUI_FILE, &psp_pwad_file3, "pwad", GUI_ENABLED },
		{ "Patch WAD", GUI_LEFT | GUI_FILE, &psp_pwad_file4, "pwad", GUI_ENABLED },
		{ "DEH File", GUI_LEFT | GUI_FILE, &psp_deh_file1, "deh", GUI_ENABLED },
		{ "DEH File", GUI_LEFT | GUI_FILE, &psp_deh_file2, "deh", GUI_ENABLED },
		{ "DEH File", GUI_LEFT | GUI_FILE, &psp_deh_file3, "deh", GUI_ENABLED },
		{ "DEH File", GUI_LEFT | GUI_FILE, &psp_deh_file4, "deh", GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_list cheat_list[] = {
		{ "none", 0 },
		{ "God Mode", 1 },
		{ "Fucking Arsenal", 2 },
		{ "Key Full Ammo", 3 },
		{ "No Clipping", 4 },
		{ "Toggle Map", 5 },
		{ "Invincible with Chainsaw", 6 },
		{ "Berserker Strength Power-up", 7 },
		{ "Invincibility Power-up", 8 },
		{ "Invisibility Power-Up", 9 },
		{ "Automap Power-up", 10 },
		{ "Anti-Radiation Suit Power-up", 11 },
		{ "Light-Amplification Visor Power-up", 12 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list ctrl_move_list[] = {
		{ "Analog", 0 },
		{ "DPad", 1 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list ctrl_lr_list[] = {
		{ "Turn", 0 },
		{ "Strafe", 1 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list ctrl_speed_list[] = {
		{ "Walk", 0 },
		{ "Run", 1 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_menu ControlLevel[] = {
		{ "Calibrate Analog Stick", GUI_CENTER | GUI_FUNCTION, &psp_stick_calibrate, 0, GUI_ENABLED },
		{ "Movement", GUI_CENTER | GUI_SELECT, &ctrl_move_list, &psp_ctrl_swapmove, GUI_ENABLED },
		{ "L/R", GUI_CENTER | GUI_SELECT, &ctrl_lr_list, &psp_ctrl_swapturn, GUI_ENABLED },
		{ "Default movement", GUI_CENTER | GUI_SELECT, &ctrl_speed_list, &psp_ctrl_run, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_menu CheatLevel[] = {
		{ "SELECT + SQUARE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[2], GUI_ENABLED },
		{ "SELECT + TRIANGLE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[3], GUI_ENABLED },
		{ "SELECT + CIRCLE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[0], GUI_ENABLED },
		{ "SELECT + CROSS", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[1], GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "SELECT + R + SQUARE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[6], GUI_ENABLED },
		{ "SELECT + R + TRIANGLE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[7], GUI_ENABLED },
		{ "SELECT + R + CIRCLE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[4], GUI_ENABLED },
		{ "SELECT + R + CROSS", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[5], GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "SELECT + L + SQUARE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[10], GUI_ENABLED },
		{ "SELECT + L + TRIANGLE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[11], GUI_ENABLED },
		{ "SELECT + L + CIRCLE", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[8], GUI_ENABLED },
		{ "SELECT + L + CROSS", GUI_CENTER | GUI_SELECT, &cheat_list, &psp_ctrl_cheat[9], GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_list snd_upd_list[] = {
		{ "140 Hz", 0 },
		{ "70 Hz", 1 },
		{ "35 Hz", 2 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_menu SoundLevel[] = {
		{ "Update Frequency", GUI_CENTER | GUI_SELECT, &snd_upd_list, &psp_snd_upd, GUI_ENABLED },
		{ "Sound Effects", GUI_CENTER | GUI_TOGGLE, &psp_sfx_enabled, 0, GUI_ENABLED },
		{ "Music", GUI_CENTER | GUI_TOGGLE, &psp_music_enabled, 0, GUI_SET_ME },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_list tv_res_list[] = {
		{ "720x480", 0 },
		{ "704x448", 1 },
		{ "640x400", 2 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list tv_cable_list[] = {
		{ "None found", 0 },
		{ "Composite", 1 },
		{ "Component", 2 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list psp_detail_list[] = {
		{ "High", 0 },
		{ "Low", 1 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_list psp_aspect_list[] = {
		{ "4:3", 0 },
		{ "16:9", 1 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_menu TvLevel[] = {
		{ "Cable", GUI_CENTER | GUI_SELECT, &tv_cable_list, &psp_tv_cable, GUI_DISABLED },
		{ "Resolution", GUI_CENTER | GUI_SELECT, &tv_res_list, &psp_tv_res, GUI_ENABLED },
		{ "Sync to VBlank", GUI_CENTER | GUI_TOGGLE, &psp_tv_sync, 0, GUI_ENABLED },
		{ "Detail", GUI_CENTER | GUI_SELECT, &psp_detail_list, &psp_tv_detail, GUI_ENABLED },
		{ "Interlaced", GUI_CENTER | GUI_TOGGLE, &psp_tv_laced, 0, GUI_SET_ME },
		{ "Aspect Ratio", GUI_CENTER | GUI_SELECT, &psp_aspect_list, &psp_tv_aspect, GUI_ENABLED },
		{ "Center Screen", GUI_CENTER | GUI_FUNCTION, &psp_tv_center, 0, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_list lcd_res_list[] = {
		{ "480x272", 0 },
		{ "368x272", 1 },
		{ "320x240", 2 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_menu LcdLevel[] = {
		{ "Resolution", GUI_CENTER | GUI_SELECT, &lcd_res_list, &psp_lcd_res, GUI_ENABLED },
		{ "Sync to VBlank", GUI_CENTER | GUI_TOGGLE, &psp_lcd_sync, 0, GUI_ENABLED },
		{ "Detail", GUI_CENTER | GUI_SELECT, &psp_detail_list, &psp_lcd_detail, GUI_ENABLED },
		{ "Aspect Ratio", GUI_CENTER | GUI_SELECT, &psp_aspect_list, &psp_lcd_aspect, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_list psp_display_list[] = {
		{ "Use LCD", 0 },
		{ "Use TV", 1 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_menu VideoLevel[] = {
		{ "LCD", GUI_CENTER | GUI_MENU, LcdLevel, 0, GUI_ENABLED },
		{ "TV", GUI_CENTER | GUI_MENU, TvLevel, 0, GUI_SET_ME },
		{ "Display", GUI_CENTER | GUI_SELECT, &psp_display_list, &psp_use_tv, GUI_SET_ME },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_list cpu_speed_list[] = {
		{ "Default", 0 },
		{ "133/66 MHz", 1 },
		{ "222/111 MHz", 2 },
		{ "266/133 MHz", 3 },
		{ "300/150 MHz", 4 },
		{ "333/166 MHz", 5 },
		{ 0, GUI_END_OF_LIST }
	};

	struct gui_menu CpuLevel[] = {
		{ "CPU Clock Frequency", GUI_CENTER | GUI_SELECT, &cpu_speed_list, &psp_cpu_speed, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_menu EditLevel[] = {
		{ "File", GUI_CENTER | GUI_MENU, FileLevel, 0, GUI_ENABLED },
		{ "Controller", GUI_CENTER | GUI_MENU, ControlLevel, 0, GUI_ENABLED },
		{ "Game", GUI_CENTER | GUI_MENU, GameLevel, 0, GUI_ENABLED },
		{ "Cheats", GUI_CENTER | GUI_MENU, CheatLevel, 0, GUI_ENABLED },
		{ "Sound", GUI_CENTER | GUI_MENU, SoundLevel, 0, GUI_ENABLED },
		{ "Video", GUI_CENTER | GUI_MENU, VideoLevel, 0, GUI_ENABLED },
		{ "Network", GUI_CENTER | GUI_MENU, NetLevel, 0, GUI_ENABLED },
		{ "CPU", GUI_CENTER | GUI_MENU, CpuLevel, 0, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	struct gui_menu TopLevel[] = {
		{ "Start", GUI_CENTER | GUI_FUNCTION, &psp_gui_start, 0, GUI_ENABLED },
		{ "Join", GUI_CENTER | GUI_FUNCTION, &psp_gui_start_net, (void *)1, GUI_ENABLED },
		{ "Host", GUI_CENTER | GUI_MENU, HostLevel, 0, GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Main WAD:", GUI_CENTER | GUI_TEXT, 0, 0, GUI_DISABLED },
		{ "", GUI_CENTER | GUI_FILE, &psp_iwad_file, "iwad", GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Config:", GUI_CENTER | GUI_TEXT, 0, 0, GUI_DISABLED },
		{ psp_cfg_status, GUI_CENTER | GUI_FUNCTION, &psp_load_config, 0, GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "Configure", GUI_CENTER | GUI_MENU, EditLevel, 0, GUI_ENABLED },
		{ "Save", GUI_CENTER | GUI_FUNCTION, &psp_save_config, 0, GUI_ENABLED },
		{ "", GUI_CENTER | GUI_DIVIDER, 0, 0, GUI_DISABLED },
		{ "About", GUI_CENTER | GUI_MENU, AboutLevel, 0, GUI_ENABLED },
		{ 0, GUI_END_OF_MENU, 0, 0, 0 } // end of menu
	};

	FILE *temp;
	char str[PSP_PATH_MAX];

	// start by setting some of the enables that depend on certain variables
	VideoLevel[1].enable = (psp_tv_cable > 0) ? GUI_ENABLED : GUI_DISABLED;
	VideoLevel[2].enable = (psp_tv_cable > 0) ? GUI_ENABLED : GUI_DISABLED;
	TvLevel[4].enable = (psp_tv_cable == 2) ? GUI_ENABLED : GUI_DISABLED;
	if (psp_tv_cable == 1)
		psp_tv_laced = 1; // composite cable is always interlaced
	snprintf(str,sizeof(str),"%s%s",psp_home,"midi/MIDI_Instruments");
	temp = fopen(str, "rb");
	if (!temp)
		psp_music_enabled = 0; // no instruments, no music
	SoundLevel[2].enable = temp ? GUI_ENABLED : GUI_DISABLED;
	if (temp)
		fclose(temp);

	gui_start_requested = 0;
	do_gui(TopLevel, (void *)0, 1);

	// now set the arg list
	set_myargv();
}

// Network support code
//
// Ad-hoc only. Every PSP joins the same ad-hoc group as an equal; who hosts
// and who plays is settled by a small lobby on its own PDP port before the
// game opens its socket (psp_net.c).

#define ADHOC_GROUP     "DOOM"
#define ADHOC_LOBBYPORT 5030 // the game itself uses 5029

#define LOBBY_MAGIC 0x4D4F4F44 // "DOOM"
#define LOBBY_HOST  0
#define LOBBY_JOIN  1
#define LOBBY_TICK  (50*1000) // lobby loops poll at 20 Hz
#define LOBBY_SEND  4         // ticks between announcements
#define LOBBY_LOST  60        // ticks of silence before a peer is dropped

typedef struct {
	u32 magic;
	u8 type;     // LOBBY_HOST or LOBBY_JOIN
	u8 started;  // host: the player list is final, go
	u8 count;    // host: players in mac[]
	u8 mode;     // host: the Host menu settings from here on, see lobby_host_packet
	u32 checksum; // sender's loaded content
	u8 mac[ADHOC_MAXPLAYERS][6]; // host: players in order, host first
	u8 skill;
	u8 level;
	u8 nomonsters;
	u8 respawn;
	u8 fast;
	u8 pad[3];
	s32 timer;
} lobby_packet_t;

// values of PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL, in menu order
static const int adhoc_channels[] = { 0, 1, 6, 11 };

static int lobby_pdp = -1;

void psp_adhoc_read_channel(void)
{
	int val, i;

	if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL, &val) != PSP_SYSTEMPARAM_RETVAL_OK)
		return;
	for (i = 0; i < 4; i++)
		if (adhoc_channels[i] == val)
			psp_net_channel = i;
}

// the channel is a PSP system setting: this changes it for the XMB too
static void psp_adhoc_write_channel(void)
{
	int val;

	if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL, &val) == PSP_SYSTEMPARAM_RETVAL_OK
	  && val == adhoc_channels[psp_net_channel])
		return;
	if (sceUtilitySetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL, adhoc_channels[psp_net_channel]) != PSP_SYSTEMPARAM_RETVAL_OK)
	{
		psp_adhoc_read_channel();
		if (adhoc_channels[psp_net_channel])
			printf("Could not change the ad-hoc channel, using %d.\n", adhoc_channels[psp_net_channel]);
		else
			printf("Could not change the ad-hoc channel, using automatic.\n");
	}
}

static u32 psp_checksum_add(u32 sum, const void *data, int len)
{
	const unsigned char *p = data;

	// FNV-1a
	while (len--)
		sum = (sum ^ *p++) * 16777619u;
	return sum;
}

// name, size and (WADs) the lump directory or (DEH) the whole file
static u32 psp_checksum_file(u32 sum, char *path, int wad)
{
	unsigned char buf[1024];
	char *name;
	FILE *f;
	int size, left, n;

	if (!path)
		return sum;

	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	for (; *name; name++)
	{
		buf[0] = (*name >= 'A' && *name <= 'Z') ? *name + 32 : *name;
		sum = psp_checksum_add(sum, buf, 1);
	}
	buf[0] = 0;
	sum = psp_checksum_add(sum, buf, 1);

	f = fopen(path, "rb");
	if (!f)
		return sum;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	sum = psp_checksum_add(sum, &size, sizeof(size));

	left = size;
	fseek(f, 0, SEEK_SET);
	if (wad)
	{
		struct { char id[4]; int numlumps; int infotableofs; } header;

		left = 0;
		if (fread(&header, 1, sizeof(header), f) == sizeof(header)
		  && header.numlumps > 0 && header.infotableofs > 0
		  && header.infotableofs < size
		  && header.numlumps <= (size - header.infotableofs) / 16)
		{
			left = header.numlumps * 16;
			fseek(f, header.infotableofs, SEEK_SET);
		}
	}
	while (left > 0)
	{
		n = fread(buf, 1, left < sizeof(buf) ? left : sizeof(buf), f);
		if (n <= 0)
			break;
		sum = psp_checksum_add(sum, buf, n);
		left -= n;
	}
	fclose(f);
	return sum;
}

// players must load the same files in the same order or the game desyncs
static u32 psp_content_checksum(void)
{
	u32 sum = 2166136261u;

	sum = psp_checksum_file(sum, psp_iwad_file, 1);
	sum = psp_checksum_file(sum, psp_pwad_file1, 1);
	sum = psp_checksum_file(sum, psp_pwad_file2, 1);
	sum = psp_checksum_file(sum, psp_pwad_file3, 1);
	sum = psp_checksum_file(sum, psp_pwad_file4, 1);
	sum = psp_checksum_file(sum, psp_deh_file1, 0);
	sum = psp_checksum_file(sum, psp_deh_file2, 0);
	sum = psp_checksum_file(sum, psp_deh_file3, 0);
	sum = psp_checksum_file(sum, psp_deh_file4, 0);
	return sum;
}

int InitialiseNetwork(void)
{
  struct productStruct product;
  int err;

  printf("load network modules...");
  err = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
  if (err != 0)
  {
    printf("Error, could not load PSP_NET_MODULE_COMMON %08X\n", err);
    return 1;
  }
  err = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
  if (err != 0)
  {
    printf("Error, could not load PSP_NET_MODULE_ADHOC %08X\n", err);
    return 1;
  }
  printf("done\n");

  err = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
  if (err == 0)
    err = sceNetAdhocInit();
  if (err == 0)
  {
    // any product id works, as long as all players use the same one
    memset(&product, 0, sizeof(product));
    memcpy(product.product, "DOOM00001", 9);
    err = sceNetAdhocctlInit(0x2000, 0x30, &product);
  }
  if (err != 0)
  {
    printf("Error, could not initialise the network %08X\n", err);
    return 1;
  }
  return 0;
}

// The net stack is loaded only when a connection is actually requested:
// once initialised it blocks PSP sleep (LED blinks, then power off).
static int psp_net_started = 0;

static int psp_net_start(void)
{
	if (psp_net_started)
		return 0;
	if (psp_net_error)
		return 1;
	if (InitialiseNetwork() != 0)
	{
		psp_net_error = 1;
		return 1;
	}
	psp_net_started = 1;
	return 0;
}

// back in the launcher without a game: unload it again so the PSP can sleep
static void psp_net_stop(void)
{
	if (!psp_net_started)
		return;
	sceNetAdhocctlTerm();
	sceNetAdhocTerm();
	sceNetTerm();
	sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
	sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
	psp_net_started = 0;
}

static int psp_adhoc_wait_wlan(void)
{
	int i;

	if (sceWlanGetSwitchState() != 1)
		printf("Please enable WLAN or press a button to go back to the launcher.\n");
	for (i=0; i<10; i++)
	{
		SceCtrlData pad;
		if (sceWlanGetSwitchState() == 1) break;
		sceCtrlReadBufferPositive(&pad, 1);
		if (pad.Buttons) return 0;
		printf("%d... ", 10-i);
		sceKernelDelayThread(1000 * 1000);
	}
	printf("\n");
	return i < 10;
}

static int psp_adhoc_connect(void)
{
	int err, i, state;

	err = sceNetAdhocctlConnect(ADHOC_GROUP);
	if (err != 0)
	{
		printf("sceNetAdhocctlConnect returns %08X\n", err);
		return 0;
	}

	printf("Joining the ad-hoc group...\n");
	for (i=0; i<300; i++) // 15 seconds
	{
		err = sceNetAdhocctlGetState(&state);
		if (err != 0)
		{
			printf("sceNetAdhocctlGetState returns %08X\n", err);
			return 0;
		}
		if (state == 1) // connected
			return 1;
		sceKernelDelayThread(50 * 1000);
	}
	printf("Could not join the ad-hoc group.\n");
	return 0;
}

static void lobby_send(unsigned char *mac, lobby_packet_t *pkt)
{
	sceNetAdhocPdpSend(lobby_pdp, mac, ADHOC_LOBBYPORT, pkt, sizeof(*pkt), 0, 1);
}

// -1 = nothing waiting, 0 = not a lobby packet, 1 = got one
static int lobby_recv(unsigned char *mac, lobby_packet_t *pkt)
{
	unsigned short port;
	int len = sizeof(*pkt);

	if (sceNetAdhocPdpRecv(lobby_pdp, mac, &port, pkt, &len, 0, 1) < 0)
		return -1;
	return len == sizeof(*pkt) && pkt->magic == LOBBY_MAGIC;
}

#define LOBBY_NAME 32

// the PSP's nickname (System Settings), its MAC address if we can't get it
static void lobby_peer_name(unsigned char *mac, char *name)
{
	char nick[128];
	int i;

	nick[0] = 0;
	if (sceNetAdhocctlGetNameByAddr(mac, nick) < 0 || !nick[0])
	{
		sceNetEtherNtostr(mac, name);
		return;
	}
	// the debug screen font is ASCII only
	for (i = 0; i < LOBBY_NAME - 1 && nick[i]; i++)
		name[i] = (nick[i] >= 32 && nick[i] < 127) ? nick[i] : '?';
	name[i] = 0;
}

// the host's announcement: who is in, and the Host menu settings
static void lobby_host_packet(lobby_packet_t *pkt, int n, u32 checksum, int started)
{
	memset(pkt, 0, sizeof(*pkt));
	pkt->magic = LOBBY_MAGIC;
	pkt->type = LOBBY_HOST;
	pkt->started = started;
	pkt->count = n;
	pkt->checksum = checksum;
	memcpy(pkt->mac, psp_adhoc_mac, sizeof(pkt->mac));
	pkt->mode = psp_game_deathmatch;
	pkt->skill = psp_game_skill;
	pkt->level = psp_game_level;
	pkt->nomonsters = psp_game_nomonsters;
	pkt->respawn = psp_game_respawn;
	pkt->fast = psp_game_fast;
	pkt->timer = psp_game_timer;
}

// joiner: play with the host's settings instead of our own
static void lobby_adopt_settings(lobby_packet_t *pkt)
{
	psp_game_deathmatch = pkt->mode;
	psp_game_skill = pkt->skill;
	psp_game_level = pkt->level;
	psp_game_nomonsters = pkt->nomonsters;
	psp_game_respawn = pkt->respawn;
	psp_game_fast = pkt->fast;
	psp_game_timer = pkt->timer;
	set_myargv();
}

// returns the bits of 'mask' that went down since the last call
static u32 lobby_buttons(u32 mask)
{
	static u32 prev = 0xFFFFFFFF; // whatever is held on entry does not count
	SceCtrlData pad;
	u32 pressed;

	sceCtrlReadBufferPositive(&pad, 1);
	pressed = pad.Buttons & ~prev & mask;
	prev = pad.Buttons;
	return pressed;
}

// returns the number of players, 0 if cancelled
static int psp_adhoc_host(u32 checksum)
{
	struct SceNetAdhocctlPeerInfo peers[16];
	unsigned char bad[ADHOC_MAXPLAYERS][6];
	int lastseen[ADHOC_MAXPLAYERS];
	char names[ADHOC_MAXPLAYERS][LOBBY_NAME]; // as shown when they joined
	lobby_packet_t pkt, in;
	unsigned char from[6];
	char str[LOBBY_NAME];
	int n = 1, nbad = 0, tick, i, r, len;
	u32 buttons;

	printf("Hosting. %s starts the game once the players are in, %s cancels.\n\n",
		psp_btn_swap ? "X" : "O", psp_btn_swap ? "O" : "X");

	for (tick = 0; ; tick++)
	{
		// joiners keep announcing themselves with their content checksum
		while ((r = lobby_recv(from, &in)) >= 0)
		{
			if (!r || in.type != LOBBY_JOIN)
				continue;
			if (in.checksum != checksum)
			{
				for (i = 0; i < nbad; i++)
					if (!memcmp(bad[i], from, 6))
						break;
				if (i == nbad && nbad < ADHOC_MAXPLAYERS)
				{
					memcpy(bad[nbad++], from, 6);
					lobby_peer_name(from, str);
					printf("%s refused: content mismatch\n", str);
				}
				continue;
			}
			for (i = 1; i < n; i++)
				if (!memcmp(psp_adhoc_mac[i], from, 6))
					break;
			if (i == ADHOC_MAXPLAYERS)
				continue; // full
			lastseen[i] = tick;
			if (i == n)
			{
				lobby_peer_name(from, names[n]);
				memcpy(psp_adhoc_mac[n++], from, 6);
				printf("%s joined, %d players\n", names[n-1], n);
			}
		}

		// joiners that went quiet cancelled or are out of range
		for (i = 1; i < n; i++)
			if (tick - lastseen[i] > LOBBY_LOST)
			{
				strcpy(str, names[i]);
				n--;
				memmove(psp_adhoc_mac[i], psp_adhoc_mac[i+1], (n - i) * 6);
				memmove(names[i], names[i+1], (n - i) * LOBBY_NAME);
				memmove(&lastseen[i], &lastseen[i+1], (n - i) * sizeof(int));
				printf("%s left, %d players\n", str, n);
				i--;
			}

		// tell the whole group who is in, that is also how joiners find us
		if (!(tick % LOBBY_SEND))
		{
			lobby_host_packet(&pkt, n, checksum, 0);
			len = sizeof(peers);
			if (sceNetAdhocctlGetPeerList(&len, peers) == 0)
				for (i = 0; i < len / sizeof(peers[0]); i++)
					lobby_send(peers[i].mac, &pkt);
		}

		buttons = lobby_buttons(psp_btn_ok | psp_btn_back);
		if ((buttons & psp_btn_back) || quit_requested)
			return 0;
		if ((buttons & psp_btn_ok) && n > 1)
			break;
		sceKernelDelayThread(LOBBY_TICK);
	}

	// the list is final: repeat it so no joiner misses the start
	lobby_host_packet(&pkt, n, checksum, 1);
	for (r = 0; r < 10; r++)
	{
		for (i = 1; i < n; i++)
			lobby_send(psp_adhoc_mac[i], &pkt);
		sceKernelDelayThread(100*1000);
	}

	psp_net_player1 = 1;
	return n;
}

// returns the number of players, 0 if cancelled
static int psp_adhoc_joiner(u32 checksum)
{
	lobby_packet_t pkt, in;
	unsigned char host[6], from[6];
	char str[LOBBY_NAME];
	int havehost = 0, hostseen = 0, warned = 0, shown = 0;
	int tick, i, j, n, r;

	printf("Looking for a host, %s cancels.\n\n", psp_btn_swap ? "O" : "X");

	for (tick = 0; ; tick++)
	{
		while ((r = lobby_recv(from, &in)) >= 0)
		{
			if (!r || in.type != LOBBY_HOST)
				continue;
			if (havehost && memcmp(from, host, 6))
				continue; // a second host in range, stay with ours
			if (!havehost)
			{
				memcpy(host, from, 6);
				havehost = 1;
				warned = shown = 0;
				lobby_peer_name(host, str);
				printf("Found host %s\n", str);
			}
			hostseen = tick;

			if (in.checksum != checksum)
			{
				if (!warned)
				{
					printf("Loaded content differs from the host's. All players need the same\n");
					printf("IWAD, PWADs and DEH files in the same order. Fix it and launch again.\n");
				}
				warned = 1;
				continue;
			}

			if (in.count > ADHOC_MAXPLAYERS)
				continue;
			// settings we couldn't start a game with
			if (in.mode > 2 || in.skill > 4 || in.level < 1 || in.level > 36
			  || in.timer < 0 || in.timer > 1440)
				continue;
			for (i = 0; i < in.count; i++)
				if (!memcmp(in.mac[i], psp_adhoc_mac[0], 6))
					break;
			if (i < in.count && shown != in.count)
			{
				shown = in.count;
				printf("Joined as player %d of %d, waiting for the host to start.\n", i+1, in.count);
			}
			if (!in.started)
				continue;
			if (i == in.count)
			{
				printf("The host started without us.\n");
				return 0;
			}

			lobby_adopt_settings(&in);
			psp_net_player1 = i+1;
			n = 1;
			for (j = 0; j < in.count; j++)
				if (j != i)
					memcpy(psp_adhoc_mac[n++], in.mac[j], 6);
			return n;
		}

		if (havehost && tick - hostseen > LOBBY_LOST)
		{
			printf("Host lost, looking again.\n");
			havehost = 0;
		}

		// keep announcing ourselves, a mismatch too so the host can show it
		if (havehost && !(tick % LOBBY_SEND))
		{
			memset(&pkt, 0, sizeof(pkt));
			pkt.magic = LOBBY_MAGIC;
			pkt.type = LOBBY_JOIN;
			pkt.checksum = checksum;
			lobby_send(host, &pkt);
		}

		if (lobby_buttons(psp_btn_back) || quit_requested)
			return 0;
		sceKernelDelayThread(LOBBY_TICK);
	}
}

// join the ad-hoc group and run the lobby; fills psp_adhoc_mac,
// psp_adhoc_numnodes and psp_net_player1 for psp_net.c
static int psp_adhoc_join(void)
{
	int n = 0;

	psp_adhoc_numnodes = 0;

	psp_adhoc_write_channel();
	if (psp_net_start() != 0)
	{
		printf("Networking not available.\n");
		return 0;
	}

	if (psp_adhoc_wait_wlan() && psp_adhoc_connect())
	{
		sceWlanGetEtherAddr(psp_adhoc_mac[0]);
		lobby_pdp = sceNetAdhocPdpCreate(psp_adhoc_mac[0], ADHOC_LOBBYPORT, 0x2000, 0);
		if (lobby_pdp < 0)
			printf("sceNetAdhocPdpCreate returns %08X\n", lobby_pdp);
		else
		{
			if (psp_net_role)
				n = psp_adhoc_joiner(psp_content_checksum());
			else
				n = psp_adhoc_host(psp_content_checksum());

			sceNetAdhocPdpDelete(lobby_pdp, 0);
			lobby_pdp = -1;
		}
		if (!n)
			sceNetAdhocctlDisconnect();
	}

	if (!n)
	{
		psp_net_stop();
		return 0;
	}

	psp_adhoc_numnodes = n;
	return 1;
}

static void psp_set_cpu_speed(void)
{
	int i, p;

	p = M_CheckParm ("-cpuMHz");
	if (p && p < myargc - 1)
	{
		i = atoi (myargv[p+1]);
		scePowerSetClockFrequency(i, i, i>>1);
	}
}

// joins the ad-hoc game if one was asked for; returns 0 when it could
// not be started
static int psp_net_join(void)
{
	if (psp_net_enabled)
	{
		if (psp_adhoc_join())
			printf("\nStarting a %d player game as player %d.\n", psp_adhoc_numnodes, psp_net_player1);
		else
		{
			printf("\nNo network game, back to the launcher.\n");
			psp_net_enabled = 0;
			sceKernelDelayThread(3*1000*1000);
			return 0;
		}
		sceKernelDelayThread(3*1000*1000);
	}
	return 1;
}

int main (int argc, char **argv)
{
    int i;

	psp_text_screen();

	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	SetupCallbacks();
	psp_read_button_swap();

	// remember our EBOOT path so I_Quit can relaunch into the GUI
	strncpy(psp_exe_path,(argc > 0 && argv[0]) ? argv[0] : "",sizeof(psp_exe_path)-1);
	psp_exe_path[sizeof(psp_exe_path)-1] = 0;

	// get launch directory name for our home
	strncpy(psp_home,(argc > 0 && argv[0]) ? argv[0] : "",sizeof(psp_home)-1);
	psp_home[sizeof(psp_home)-1] = 0;
	char *str_ptr=strrchr(psp_home,'/');
	if (str_ptr){
		str_ptr++;
		*str_ptr = 0;
	}
	else
		psp_home[0] = 0; // no dir in argv[0], use current dir

	// user data dirs, created on first run
	{
		char dir[256];
		snprintf(dir, sizeof(dir), "%sconfig", psp_home);
		mkdir(dir, 0777);
		snprintf(dir, sizeof(dir), "%ssaves", psp_home);
		mkdir(dir, 0777);
	}

	if (sceKernelDevkitVersion() >= 0x03070110)
		if (kuKernelGetModel() == PSP_MODEL_SLIM_AND_LITE)
		{
			char str[PSP_PATH_MAX];
			snprintf(str,sizeof(str),"%s%s",psp_home,"dvemgr.prx");
			if (pspSdkLoadStartModule(str, PSP_MEMORY_PARTITION_KERNEL) >= 0)
				psp_tv_cable = pspDveMgrCheckVideoOut();
		}

	// kernel helper so I_Quit can relaunch us; user mode LoadExec is refused on real hardware
	{
		char str[PSP_PATH_MAX];
		snprintf(str,sizeof(str),"%s%s",psp_home,"relaunch.prx");
		psp_relaunch_ok = pspSdkLoadStartModule(str, PSP_MEMORY_PARTITION_KERNEL) >= 0;
	}

	psp_adhoc_read_channel();
	psp_font_init();

	if ((myargv = malloc(sizeof(char *) * MAXARGVS)) == NULL)
	{
		printf("malloc(%d) failed", sizeof(char *) * MAXARGVS);
		sceKernelDelayThread(5*1000*1000);
		sceKernelExitGame();
	}
	memset (myargv, 0, sizeof(char *)*MAXARGVS);
	myargc = 0;

	psp_load_defaults();

	if (!psp_iwad_file)
	{
		// no (valid) IWAD in config: pick first one found in <home>iwad/,
		// full versions before shareware
		static const char *iwads[] = { "doom2.wad", "plutonia.wad", "tnt.wad",
			"doomu.wad", "doom.wad", "freedoom2.wad", "freedoom1.wad",
			"doom1.wad", "freedm.wad", 0 };
		char temp[256];

		snprintf(temp, sizeof(temp), "%siwad", psp_home);
		for (i = 0; iwads[i] && !psp_iwad_file; i++)
			psp_iwad_file = psp_find_file(temp, iwads[i]);
	}

/* The original fixed point code is faster on GCC */
#ifdef USE_FLOAT_FIXED
	SetFPMode ();  /* set FPU rounding mode to "trunc towards -infinity" */
#endif

	// a Host/Join that is cancelled or fails comes back to the launcher
	do
	{
		psp_gui();

		psp_text_screen();

		printf ("DOOM v%d.%d for the PSP\n\n", VERS, REVS);
		printf ("Args passed to D_DoomMain() are:\n");
		for (i = 1 ; i < myargc; i++)
			printf (" %s", myargv[i]);
		printf ("\n\n");

		psp_set_cpu_speed();
	}
	while (!psp_net_join());

	i = scePowerGetCpuClockFrequency();
	printf("The current CPU speed is %d MHz\n\n", i);
	sceKernelDelayThread(1*1000*1000);

	D_DoomMain ();

	sceKernelDelayThread(5*1000*1000);
	sceKernelExitGame();
	return 0;
}


int isIWADDoom2 (void)
{
	static const char *doom2[] = { "doom2f.wad", "doom2.wad", "plutonia.wad",
		"tnt.wad", "freedoom2.wad", "freedm.wad", 0 };
	const char *name;
	int i;

	if (!psp_iwad_file)
		return 1;

	name = strrchr(psp_iwad_file, '/');
	name = name ? name + 1 : psp_iwad_file;

	for (i = 0; doom2[i]; i++)
		if (!strcasecmp(name, doom2[i]))
			return 1;

	return 0;
}

// append to myargv; the last entry stays a terminating null
static void arg_add(const char *arg)
{
	if (myargc < MAXARGVS - 1)
		myargv[myargc++] = strdup(arg);
}

static void arg_addf(const char *fmt, ...)
{
	char temp[32];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(temp, sizeof(temp), fmt, ap);
	va_end(ap);
	arg_add(temp);
}

static void arg_flag(int on, const char *arg)
{
	if (on)
		arg_add(arg);
}

static void arg_int(const char *arg, int value)
{
	arg_add(arg);
	arg_addf("%d", value);
}

// "<arg> file file ...", nothing if no file is set
static void arg_files(const char *arg, char **files, int n)
{
	int i, any = 0;

	for (i = 0; i < n; i++)
		if (files[i])
		{
			if (!any++)
				arg_add(arg);
			arg_add(files[i]);
		}
}

void set_myargv(void)
{
	char *pwads[] = { psp_pwad_file1, psp_pwad_file2, psp_pwad_file3, psp_pwad_file4 };
	char *dehs[] = { psp_deh_file1, psp_deh_file2, psp_deh_file3, psp_deh_file4 };
	int i;

	// free old argv entries
	for (i = 0 ; i < myargc; i++)
	{
		free(myargv[i]);
		myargv[i] = 0;
	}
	myargc = 0;

	arg_add("Doom");

	if (psp_use_tv)
	{
		arg_add("-tv");
		arg_int("-width", tv_res[psp_tv_res][0]);
		arg_int("-height", tv_res[psp_tv_res][1]);
		arg_flag(psp_tv_sync, "-vsync");
		arg_flag(psp_tv_laced, "-laced");
		arg_flag(psp_tv_aspect, "-16:9");
		arg_flag(psp_tv_detail, "-lowdetail");
		arg_int("-tvcx", psp_tv_cx);
		arg_int("-tvcy", psp_tv_cy);
	}
	else
	{
		arg_int("-width", lcd_res[psp_lcd_res][0]);
		arg_int("-height", lcd_res[psp_lcd_res][1]);
		arg_flag(psp_lcd_sync, "-vsync");
		arg_flag(psp_lcd_aspect, "-16:9");
		arg_flag(psp_lcd_detail, "-lowdetail");
	}

	arg_flag(!psp_sfx_enabled, "-nosfx");
	arg_flag(psp_music_enabled, "-music");
	// 35Hz is the default
	arg_flag(psp_snd_upd == 0, "-140Hz");
	arg_flag(psp_snd_upd == 1, "-70Hz");

	if (psp_iwad_file)
	{
		arg_add("-iwad");
		arg_add(psp_iwad_file);
	}
	arg_files("-file", pwads, 4);
	arg_files("-deh", dehs, 4);

	arg_flag(psp_game_nomonsters, "-nomonsters");
	arg_flag(psp_game_respawn, "-respawn");
	arg_flag(psp_game_fast, "-fast");
	arg_flag(psp_game_turbo, "-turbo");
	arg_flag(psp_game_maponhu, "-maponhu");
	arg_flag(psp_game_rotatemap, "-rotatemap");
	arg_flag(psp_game_deathmatch == 1, "-deathmatch");
	arg_flag(psp_game_deathmatch == 2, "-altdeath");

	if (psp_game_record)
	{
		arg_add("-record");
		arg_addf("demo%d", psp_game_record);
	}
	if (psp_game_playdemo)
	{
		arg_add("-playdemo");
		arg_addf("demo%d", psp_game_playdemo);
	}
	arg_flag(psp_game_forcedemo, "-forcedemo");
	if (psp_game_timedemo)
	{
		arg_add("-timedemo");
		arg_addf("demo%d", psp_game_timedemo);
	}
	if (psp_game_timer)
		arg_int("-timer", psp_game_timer);

	if (psp_cpu_speed != 0)
	{
		arg_add("-cpuMHz");
		arg_add(cfg_cpu_names[psp_cpu_speed]);
	}

	arg_int("-analogcx", psp_stick_cx);
	arg_int("-analogcy", psp_stick_cy);
	arg_int("-analogminx", psp_stick_minx);
	arg_int("-analogminy", psp_stick_miny);
	arg_int("-analogmaxx", psp_stick_maxx);
	arg_int("-analogmaxy", psp_stick_maxy);

	for (i=0; i<NUM_CHEAT_SLOTS; i++)
		if (psp_ctrl_cheat[i])
		{
			arg_addf("-cheat%d", i+1);
			arg_addf("%d", psp_ctrl_cheat[i]);
		}

	arg_flag(psp_ctrl_swapmove, "-swapmove");
	arg_flag(psp_ctrl_swapturn, "-swapturn");
	arg_flag(psp_ctrl_run, "-run");

	// network tuning; only used in a network game
	arg_flag(psp_net_extratic, "-extratic");

	if (psp_game_skill != 2)
		arg_int("-skill", psp_game_skill+1);

	if (psp_game_level != 1)
	{
		arg_add("-warp");
		if (isIWADDoom2())
			arg_addf("%d", psp_game_level);
		else
		{
			arg_addf("%d", (psp_game_level-1) / 9 + 1);
			arg_addf("%d", (psp_game_level-1) % 9 + 1);
		}
	}
}

static int psp_file_exists(const char *path)
{
	FILE *hnd = fopen(path, "rb");
	if (hnd)
		fclose(hnd);
	return hnd != NULL;
}

// Look for name in dir ignoring case (host filesystems under PPSSPP are
// case sensitive, the memory stick is not). Returns a malloc'd path or 0.
static char *psp_find_file(const char *dir, const char *name)
{
	DIR *dp;
	struct dirent *de;
	char *found = 0;

	if (!(dp = opendir(dir)))
		return 0;

	while (!found && (de = readdir(dp)))
		if (!strcasecmp(de->d_name, name))
		{
			found = malloc(strlen(dir) + strlen(de->d_name) + 2);
			if (found)
				sprintf(found, "%s/%s", dir, de->d_name);
		}

	closedir(dp);
	return found;
}

// Config files may hold absolute paths, which go stale when the game dir
// moves (e.g. ms0: <-> ef0:). If a file is missing, look for the same
// name in <home><subdir>/; if that fails too, drop it.
static void psp_fix_path(char **file, const char *subdir)
{
	char temp[256];
	char *base;

	if (!*file || psp_file_exists(*file))
		return;

	base = strrchr(*file, '/');
	base = base ? base + 1 : *file;
	snprintf(temp, sizeof(temp), "%s%s", psp_home, subdir);
	base = psp_find_file(temp, base);

	free(*file);
	*file = base;
}
