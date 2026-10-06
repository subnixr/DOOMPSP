#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspgu.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define printf pspDebugScreenPrintf

#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "m_argv.h"
#include "m_bbox.h"
#include "d_main.h"

#include "r_draw.h"
#include "w_wad.h"
#include "z_zone.h"

#include "psp.h"


int SCREENWIDTH;
int SCREENHEIGHT;
int weirdaspect;

#define NUMPALETTES 14

static u32 video_colourtable[NUMPALETTES][256];
static int video_palette_index = 0;
static int video_palette_changed = 0;

static int video_is_laced = 0;
static int video_cable = 0;
static int video_is_tv = 0;
static int video_vsync = 0;

static int lineBytes, lineWidth;

static u32 *vram1;
static u32 *vram2;
static int vramflip = 0;
static int vramoffset;

static unsigned int total_frames = 0;

/**********************************************************************/
// switch the video output to the TV; cable: 1 = composite, 2 = component
void psp_tv_mode(int cable, int laced)
{
	if (cable == 1)
		pspDveMgrSetVideoOut(2, 0x1d1, 720, 503, 1, 15, 0); // composite
	else
		if (laced)
			pspDveMgrSetVideoOut(0, 0x1d1, 720, 503, 1, 15, 0); // component interlaced
		else
			pspDveMgrSetVideoOut(0, 0x1d2, 720, 480, 1, 15, 0); // component progressive
}

/**********************************************************************/
void video_set_vmode(void)
{
	if (video_is_tv)
		psp_tv_mode(video_cable, video_is_laced);
	else
		sceDisplaySetMode(0,480,272);
}

/**********************************************************************/
// Called by D_DoomMain,
// determines the hardware configuration
// and sets up the video mode
void I_InitGraphics (void)
{
	int p;

	video_cable = pspDveMgrCheckVideoOut();

	video_is_laced = M_CheckParm ("-laced");
	video_is_tv = M_CheckParm ("-tv");
	video_vsync = M_CheckParm ("-vsync");

	lineWidth = (SCREENWIDTH > 480) ? 768 : 512;
	lineBytes = lineWidth * 4;

	if (video_is_tv) {
		int i = 0;
		int j = 0;

		p = M_CheckParm ("-tvcx");
		if (p)
			sscanf(myargv[p+1], "%d", &i);
		p = M_CheckParm ("-tvcy");
		if (p)
			sscanf(myargv[p+1], "%d", &j);

		vramoffset = i*4 + lineBytes * (video_is_laced ? j/2 : j);
	} else
		vramoffset = SCREENWIDTH == 480 ? 0 : SCREENWIDTH == 368 ? (480-368)*2 : (480-320)*2 + (272-240)*lineBytes/2;

	if (video_is_tv)
	{
		// put it in the extra 32MB since we KNOW we're on a slim
		vram1 = (u32 *)0x4A000000;
		vram2 = (u32 *)(0x4A000000 + lineBytes * (video_is_laced ? 503 : 480));
	}
	else
	{
		// put it in EDRAM in case we're on a phat PSP
		vram1 = (u32 *)0x44000000;
		vram2 = (u32 *)(0x44000000 + lineBytes * 272);
	}

	I_RecalcPalettes ();

	video_set_vmode();
}

/**********************************************************************/
void I_ShutdownGraphics (void)
{
}

/**********************************************************************/
// recalculate colourtable[][] after changing usegamma
void I_RecalcPalettes (void)
{
  int p, i;
  byte *playpal, *palette;

  playpal = (byte *) W_CacheLumpName ("PLAYPAL", PU_CACHE);
  for (p = 0; p < NUMPALETTES; p++) {
    palette = playpal + p*768;
    for (i=0; i<256; i++) {
        // Better to define c locally here instead of for the whole function:
        u32 r = gammatable[usegamma][palette[i*3]];
        u32 g = gammatable[usegamma][palette[i*3+1]];
        u32 b = gammatable[usegamma][palette[i*3+2]];
        video_colourtable[p][i] = b<<16 | g<<8 | r;
    }
  }
}

/**********************************************************************/
// Takes full 8 bit values.
void I_SetPalette (byte *palette, int palette_index)
{
  video_palette_changed = 1;
  video_palette_index = palette_index;
}

/**********************************************************************/
// Called by anything that renders to screens[0] (except 3D view)
void I_MarkRect (int left, int top, int width, int height)
{
  M_AddToBox (dirtybox, left, top);
  M_AddToBox (dirtybox, left + width - 1, top + height - 1);
}

/**********************************************************************/
void I_StartUpdate (void)
{
}

/**********************************************************************/
void I_UpdateNoBlit (void)
{
}

/**********************************************************************/
void I_FinishUpdate (void)
{
	int i, j;
	u32 *base_address;
	static u32 *palette = video_colourtable[0];

	if (total_frames == 0)
	{
		memset((void *)vram1, 0, lineBytes * (!video_is_tv ? 272 : video_is_laced ? 503 : 480));
		memset((void *)vram2, 0, lineBytes * (!video_is_tv ? 272 : video_is_laced ? 503 : 480));
	}
	total_frames++;

    if (video_palette_changed != 0) {
      palette = video_colourtable[video_palette_index];
      video_palette_changed = 0;
    }

	base_address = vramflip ? vram1 : vram2;
	base_address = (u32 *)((u32)base_address + vramoffset);

	// the whole screen every frame, four 8-bit pixels at a time
	for (j=0; j<SCREENHEIGHT; j++)
		for (i=0; i<SCREENWIDTH; i+=4)
		{
			u32 fp = *(u32 *)&screens[0][i + j*SCREENWIDTH];
			if (video_is_laced)
			{
				if (j & 1)
				{
					base_address[i + (j>>1)*lineWidth] = palette[fp&0xff];
					base_address[i + 1 + (j>>1)*lineWidth] = palette[(fp>>8)&0xff];
					base_address[i + 2 + (j>>1)*lineWidth] = palette[(fp>>16)&0xff];
					base_address[i + 3 + (j>>1)*lineWidth] = palette[fp>>24];
				}
				else
				{
					base_address[i + (j>>1)*lineWidth + 262*lineWidth] = palette[fp&0xff];
					base_address[i + 1 + (j>>1)*lineWidth + 262*lineWidth] = palette[(fp>>8)&0xff];
					base_address[i + 2 + (j>>1)*lineWidth + 262*lineWidth] = palette[(fp>>16)&0xff];
					base_address[i + 3 + (j>>1)*lineWidth + 262*lineWidth] = palette[fp>>24];
				}
			}
			else
			{
				base_address[i + j*lineWidth] = palette[fp&0xff];
				base_address[i + 1 + j*lineWidth] = palette[(fp>>8)&0xff];
				base_address[i + 2 + j*lineWidth] = palette[(fp>>16)&0xff];
				base_address[i + 3 + j*lineWidth] = palette[fp>>24];
			}
		}

	if (video_vsync)
		sceDisplayWaitVblankStart();

	psp_step = "I_FinishUpdate flip";
	sceDisplaySetFrameBuf((void *) vramflip ? vram1 : vram2, lineWidth, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);

	vramflip ^= 1;
}

/**********************************************************************/
// Wait for vertical retrace or pause a bit.  Use when quit game.
void I_WaitVBL(int count)
{
  for ( ; count > 0; count--)
    sceDisplayWaitVblankStart();
}

/**********************************************************************/
void I_ReadScreen (byte* scr)
{
  memcpy (scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

/**********************************************************************/
void I_BeginRead (void)
{
}

/**********************************************************************/
void I_EndRead (void)
{
}

/**********************************************************************/
