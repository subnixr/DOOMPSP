#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <psppower.h>
#include <psploadexec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_main.h"
#include "d_net.h"
#include "g_game.h"
#include "m_argv.h"
#include "doomstat.h"
#include "m_menu.h"

#define printf pspDebugScreenPrintf

int pspDveMgrSetVideoOut(int, int, int, int, int, int, int);

extern int psp_use_tv;
extern char psp_exe_path[];

typedef unsigned char      uint8_t;
typedef signed   char      sint8_t;
typedef unsigned short     uint16_t;
typedef signed   short     sint16_t;
typedef signed   int       sint32_t;

extern byte *vid_mem;

int quit_requested = 0;


#define MIN_ZONESIZE  (2*1024*1024)
#define MAX_ZONESIZE  (6*1024*1024)

void psp_getevents (void);

static int stick_disabled = 0;
static int stick_cx = 128;
static int stick_cy = 128;
static int stick_minx = 0;
static int stick_miny = 0;
static int stick_maxx = 255;
static int stick_maxy = 255;
#define NUM_CHEAT_SLOTS 12
static int ctrl_cheat[NUM_CHEAT_SLOTS];
static int swap_move = 0;   // DPad moves, analog does the DPad actions
static int swap_turn = 0;   // L/R strafe, move stick X turns

extern int psp_stickturn;

/**********************************************************************/
// Called by DoomMain.
void I_Init (void)
{
	int p, i;

	I_InitSound ();
	I_InitMusic ();
	//I_InitGraphics ();

	// Init PSP controller stuff
	p = M_CheckParm ("-noanalog");
	if (p)
		stick_disabled = 1;
	else
	{
		p = M_CheckParm ("-analogcx");
		if (p && p < myargc - 1)
			stick_cx = atoi (myargv[p+1]);
		p = M_CheckParm ("-analogcy");
		if (p && p < myargc - 1)
			stick_cy = atoi (myargv[p+1]);
		p = M_CheckParm ("-analogminx");
		if (p && p < myargc - 1)
			stick_minx = atoi (myargv[p+1]);
		p = M_CheckParm ("-analogminy");
		if (p && p < myargc - 1)
			stick_miny = atoi (myargv[p+1]);
		p = M_CheckParm ("-analogmaxx");
		if (p && p < myargc - 1)
			stick_maxx = atoi (myargv[p+1]);
		p = M_CheckParm ("-analogmaxy");
		if (p && p < myargc - 1)
			stick_maxy = atoi (myargv[p+1]);
	}

	for (i=0; i<NUM_CHEAT_SLOTS; i++)
	{
		char arg[24];

		sprintf(arg, "-cheat%d", i+1);
		p = M_CheckParm (arg);
		if (p && p < myargc - 1)
			ctrl_cheat[i] = atoi (myargv[p+1]);
	}

	swap_move = M_CheckParm ("-swapmove") != 0;
	swap_turn = M_CheckParm ("-swapturn") != 0;
	psp_stickturn = swap_turn;

}

/**********************************************************************/
// Called by startup code
// to get the ammount of memory to malloc
// for the zone management.
byte*	I_ZoneBase (int *size)
{
	byte *zone;
	int p;

	p = M_CheckParm ("-heapsize");
	if (p && p < myargc - 1)
		*size = 1024 * atoi (myargv[p+1]);
	else
		*size = MAX_ZONESIZE;

	if ((zone = (byte *)malloc(*size)) == NULL)
	{
		printf("Couldn't allocate %d bytes for zone\n", *size);
		*size = MIN_ZONESIZE;
		if ((zone = (byte *)malloc(*size)) == NULL)
			I_Error ("malloc() %d bytes for zone management failed", *size);
	}

	printf ("I_ZoneBase(): Allocated %d bytes for zone management\n", *size);

	return zone;
}

/**********************************************************************/
// Called by D_DoomLoop,
// returns current time in tics.

extern volatile int snd_ticks; // advanced by sound thread

int I_GetTime (void)
{
  return snd_ticks;
}

void I_Yield (void)
{
  sceKernelDelayThread(1000);
}

/**********************************************************************/
// Sleep handling. The power callback sets psp_suspending; the main
// thread parks here so it does no rendering or file I/O while the
// system goes to sleep.

volatile int psp_suspending = 0;
const char *psp_step = "startup";

extern char psp_home[256];

// Debug log for sleep problems: open/append/close per line so it
// survives the PSP powering off.
void psp_sleeplog (const char *fmt, ...)
{
  char path[288];
  char line[256];
  va_list ap;
  int n;
  SceUID fd;

  n = snprintf(line, sizeof(line), "%10u ", (unsigned)sceKernelGetSystemTimeLow());
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(line) - 2)
    n = sizeof(line) - 2;
  line[n++] = '\n';

  snprintf(path, sizeof(path), "%slogs", psp_home);
  sceIoMkdir(path, 0777);
  snprintf(path, sizeof(path), "%slogs/sleep.log", psp_home);
  fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0)
    return;
  sceIoWrite(fd, line, n);
  sceIoClose(fd);
}

void psp_wait_resume (void)
{
  const char *where;

  if (!psp_suspending)
    return;
  where = psp_step;
  while (psp_suspending)
    sceKernelDelayThread(10*1000);
  // logged only now: no file I/O while the system is suspending
  psp_sleeplog("main: was parked at %s, resumed", where);
}

/**********************************************************************/
//
// Called by D_DoomLoop,
// called before processing any tics in a frame
// (just after displaying a frame).
// Time consuming syncronous operations
// are performed here (joystick reading).
// Can call D_PostEvent.
//
void I_StartFrame (void)
{
	if (quit_requested)
		I_Error ("User forced quit via Home button\n");
    psp_wait_resume ();
    psp_step = "I_StartFrame";
    psp_getevents ();
}

/**********************************************************************/
//
// Called by D_DoomLoop,
// called before processing each tic in a frame.
// Quick syncronous operations are performed here.
// Can call D_PostEvent.
void I_StartTic (void)
{
	if (quit_requested)
		I_Error ("User forced quit via Home button\n");
}

/**********************************************************************/
// Asynchronous interrupt functions should maintain private queues
// that are read by the synchronous functions
// to be converted into events.

// Either returns a null ticcmd,
// or calls a loadable driver to build it.
// This ticcmd will then be modified by the gameloop
// for normal input.
ticcmd_t	emptycmd;
ticcmd_t* I_BaseTiccmd (void)
{
  return &emptycmd;
}

/**********************************************************************/
// Called by M_Responder when quit is selected.
// Clean exit, displays sell blurb.
void I_Quit (void)
{
	D_QuitNetGame ();
	I_ShutdownSound();
	I_ShutdownMusic();
	M_SaveDefaults ();
	I_ShutdownGraphics();

	// relaunch our own EBOOT so we come back up in the launcher GUI
	if (psp_exe_path[0])
	{
		struct SceKernelLoadExecParam param;

		param.size = sizeof(param);
		param.args = strlen(psp_exe_path) + 1;
		param.argp = psp_exe_path;
		param.key = NULL;
		sceKernelLoadExec(psp_exe_path, &param);
	}

	// relaunch failed: fall back to exiting to the XMB
	sceKernelExitGame();
}

/**********************************************************************/
// Allocates from low memory under dos,
// just mallocs under unix
byte* I_AllocLow (int length)
{
  byte*	mem;

  if ((mem = (byte *)malloc (length)) == NULL)
    I_Error ("Out of memory allocating %d bytes", length);
  memset (mem,0,length);
  return mem;
}

/**********************************************************************/
void I_Tactile (int on, int off, int total)
{
  // UNUSED.
  on = off = total = 0;
}

/**********************************************************************/
void *I_malloc (size_t size)
{
  void *b;

  if ((b = malloc (size)) == NULL)
    I_Error ("Out of memory allocating %d bytes", size);
  return b;
}

/**********************************************************************/
void *I_calloc (size_t nelt, size_t esize)
{
  void *b;

  if ((b = calloc (nelt, esize)) == NULL)
    I_Error ("Out of memory allocating %d bytes", nelt * esize);
  return b;
}

/**********************************************************************/
void I_Error (char *error, ...)
{
	va_list	argptr;
	char msg[256];

	if (psp_use_tv)
		pspDveMgrSetVideoOut(0, 0, 480, 272, 1, 15, 0); // LCD

	pspDebugScreenInit();
	pspDebugScreenSetBackColor(0x000000);
	pspDebugScreenSetTextColor(0xffffff);

	// Message first.
	va_start (argptr, error);
	vsprintf (msg, error, argptr);
	va_end (argptr);
	printf("Error: %s\n", msg);

	// Shutdown. Here might be other errors.
	if (demorecording)
		G_CheckDemoStatus ();

	D_QuitNetGame ();
	I_ShutdownGraphics ();

	sceKernelDelayThread(5*1000*1000);
	sceKernelExitGame();
}

/**********************************************************************/
// misc extra functions needed by DOOM

int access(const char *path, int mode)
{
	FILE *lock;

	if ((lock = fopen(path, "rb"))) {
		fclose(lock);
		return 0;
	}

	return -1;
}

sint32_t _atoi(uint8_t *s)
{
#define ISNUM(c) ((c) >= '0' && (c) <= '9')

  register uint32_t i = 0;
  register uint32_t sign = 0;
  register uint8_t *p = s;

  /* Conversion starts at the first numeric character or sign. */
  while(*p && !ISNUM(*p) && *p != '-') p++;

  /*
     If we got a sign, set a flag.
     This will negate the value before return.
   */
  if(*p == '-')
    {
      sign++;
      p++;
    }

  /* Don't care when 'u' overflows (Bug?) */
  while(ISNUM(*p))
    {
      i *= 10;
      i += *p++ - '0';
    }

  /* Return according to sign */
  if(sign)
    return - i;
  else
    return i;

#undef ISNUM
}

int islower(int c)
{
	if (c < 'a')
		return 0;

	if (c > 'z')
		return 0;

	// passed both criteria, so it
	// is a lower case alpha char
	return 1;
}

int _toupper(int c)
{
	if ( islower( c ) ){
		c -= 32;
	}
	return c;
}

/**********************************************************************/

void psp_do_cheat(int cheat)
{
    event_t event;
    char *str;
    int i;

	switch (cheat)
	{
		case 1: // God Mode
		str = "iddqd";
		break;
		case 2: // Fucking Arsenal
		str = "idfa";
		break;
		case 3: // Key Full Ammo
		str = "idkfa";
		break;
		case 4: // No Clipping
		str = "idclip";
		break;
		case 5: // Toggle Map
		str = "iddt";
		break;
		case 6: // Invincible with Chainsaw
		str = "idchoppers";
		break;
		case 7: // Berserker Strength Power-up
		str = "idbeholds";
		break;
		case 8: // Invincibility Power-up
		str = "idbeholdv";
		break;
		case 9: // Invisibility Power-Up
		str = "idbeholdi";
		break;
		case 10: // Automap Power-up
		str = "idbeholda";
		break;
		case 11: // Anti-Radiation Suit Power-up
		str = "idbeholdr";
		break;
		case 12: // Light-Amplification Visor Power-up
		str = "idbeholdl";
		break;
		default:
		return;
	}

	for (i=0; i<strlen(str); i++)
	{
        event.type = ev_keydown;
        event.data1 = str[i];
        D_PostEvent (&event);
        event.type = ev_keyup;
        event.data1 = str[i];
        D_PostEvent (&event);
	}
}

extern boolean menuactive;
extern int psp_weapon_change;

#define PSP_NUMSLOTS 7

// Weapon slot (number key - 1) holding weapon w
static int psp_weapon_slot(weapontype_t w)
{
    switch (w)
    {
      case wp_fist:
      case wp_chainsaw:     return 0;
      case wp_pistol:       return 1;
      case wp_shotgun:
      case wp_supershotgun: return 2;
      case wp_chaingun:     return 3;
      case wp_missile:      return 4;
      case wp_plasma:       return 5;
      default:              return 6; // wp_bfg
    }
}

// True if the player owns any usable weapon in slot
static boolean psp_slot_occupied(player_t *player, int slot)
{
    switch (slot)
    {
      case 0: return true; // fist is always there
      case 1: return player->weaponowned[wp_pistol];
      case 2: return player->weaponowned[wp_shotgun]
                  || (gamemode == commercial && player->weaponowned[wp_supershotgun]);
      case 3: return player->weaponowned[wp_chaingun];
      case 4: return player->weaponowned[wp_missile];
      case 5: return gamemode != shareware && player->weaponowned[wp_plasma];
      default: return gamemode != shareware && player->weaponowned[wp_bfg];
    }
}

// Pick the next (dir = 1) or previous (dir = -1) occupied weapon slot,
// selecting it like its number key (the engine picks fist/chainsaw and
// shotgun/super shotgun within the slot)
static void psp_cycle_weapon(int dir)
{
    static const weapontype_t slot_key[PSP_NUMSLOTS] = {
        wp_fist, wp_pistol, wp_shotgun, wp_chaingun, wp_missile, wp_plasma, wp_bfg
    };
    player_t *player;
    weapontype_t current;
    int slot, n;

    if (gamestate != GS_LEVEL || menuactive || !playeringame[consoleplayer])
        return;
    player = &players[consoleplayer];
    if (!player->mo || player->playerstate != PST_LIVE)
        return;

    // start from the weapon already on its way, so repeated taps chain
    current = player->readyweapon;
    if (player->pendingweapon != wp_nochange)
        current = player->pendingweapon;
    if (psp_weapon_change != wp_nochange)
        current = psp_weapon_change;

    slot = psp_weapon_slot(current);
    for (n = 1; n < PSP_NUMSLOTS; n++)
    {
        slot = (slot + dir + PSP_NUMSLOTS) % PSP_NUMSLOTS;
        if (psp_slot_occupied(player, slot))
        {
            psp_weapon_change = slot_key[slot];
            return;
        }
    }
}

static void psp_postkey (evtype_t type, int key)
{
    event_t event;

    event.type = type;
    event.data1 = key;
    D_PostEvent (&event);
}

static void psp_tapkey (int key)
{
    psp_postkey (ev_keydown, key);
    psp_postkey (ev_keyup, key);
}

// hold a key while a button is down (edge triggered)
static void psp_holdkey (u32 cur, u32 previous, u32 button, int key, int allowed)
{
    if (allowed && (cur & button) && !(previous & button))
        psp_postkey (ev_keydown, key);
    else if (!(cur & button) && (previous & button))
        psp_postkey (ev_keyup, key);
}

#define DPAD_MASK (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)

// analog stick as a DPad, with hysteresis so it doesn't chatter
static u32 psp_stick_dirs (SceCtrlData *pad, u32 previous)
{
    int dx = pad->Lx - stick_cx;
    int dy = pad->Ly - stick_cy;
    u32 dirs = 0;

    if (dx < -(previous & PSP_CTRL_LEFT ? 40 : 80))
        dirs |= PSP_CTRL_LEFT;
    else if (dx > (previous & PSP_CTRL_RIGHT ? 40 : 80))
        dirs |= PSP_CTRL_RIGHT;
    if (dy < -(previous & PSP_CTRL_UP ? 40 : 80))
        dirs |= PSP_CTRL_UP;
    else if (dy > (previous & PSP_CTRL_DOWN ? 40 : 80))
        dirs |= PSP_CTRL_DOWN;

    return dirs;
}

#define PRESSED(b) ((cur & (b)) && !(previous & (b)))

void psp_getevents (void)
{
	SceCtrlData pad;
    event_t joyevent;
    event_t mouseevent;
    short mousex, mousey;
    static u32 previous = -1;
	static int rx, ry;
	u32 cur, sel, dirs;

	sceCtrlReadBufferPositive(&pad, 1);
	if (previous == -1)
	{
		previous = pad.Buttons;
		rx = ry = abs(stick_cx - 128) > 16 || abs(stick_cy - 128) > 16 ? 32 : 24;
	}

	// movement: analog stick, or the DPad when swapped
	mousex = mousey = 0;
	if (swap_move)
	{
		if (pad.Buttons & PSP_CTRL_LEFT)
			mousex = -127;
		else if (pad.Buttons & PSP_CTRL_RIGHT)
			mousex = 127;
		if (pad.Buttons & PSP_CTRL_UP)
			mousey = 127;
		else if (pad.Buttons & PSP_CTRL_DOWN)
			mousey = -127;
	}
	else if (!stick_disabled)
	{
		// we don't use the min/max yet, and center just affects the comparison
		// rescale from the deadzone edge so full tilt = +/-127
		int dx = pad.Lx - 128;
		int dy = 128 - pad.Ly;

		if (abs(dx) > rx)
			mousex = (dx > 0 ? dx - rx : dx + rx) * 127 / (127 - rx);
		if (abs(dy) > ry)
			mousey = (dy > 0 ? dy - ry : dy + ry) * 127 / (127 - ry);
		if (mousex > 127) mousex = 127;
		if (mousex < -127) mousex = -127;
		if (mousey > 127) mousey = 127;
		if (mousey < -127) mousey = -127;
	}

	if (mousex || mousey)
	{
		mouseevent.type = ev_mouse;
		mouseevent.data1 = 0; // no mouse buttons
		mouseevent.data2 = mousex;
		mouseevent.data3 = mousey;
		D_PostEvent (&mouseevent);
	}

	// virtual pad: the DPad bits hold the direction actions, which come
	// from the analog stick when swapped
	if (swap_move)
		dirs = stick_disabled ? 0 : psp_stick_dirs(&pad, previous);
	else
		dirs = pad.Buttons & DPAD_MASK;
	cur = (pad.Buttons & ~DPAD_MASK) | dirs;

	if (cur == previous)
		return;

	sel = cur & PSP_CTRL_SELECT;

    joyevent.type = ev_joystick;
    joyevent.data1 = joyevent.data2 = joyevent.data3 = 0;

	if (menuactive)
	{
		// menu: directions navigate, CROSS = enter/yes, CIRCLE = back/no
		if (!sel)
		{
			if (cur & PSP_CTRL_LEFT)
				joyevent.data2 = -1;
			else if (cur & PSP_CTRL_RIGHT)
				joyevent.data2 = 1;
			if (cur & PSP_CTRL_UP)
				joyevent.data3 = -1;
			else if (cur & PSP_CTRL_DOWN)
				joyevent.data3 = 1;

			if (PRESSED(PSP_CTRL_CROSS))
				psp_tapkey (KEY_ENTER);
			if (PRESSED(PSP_CTRL_CIRCLE))
				psp_tapkey (KEY_BACKSPACE);
		}
	}
	else if (!sel)
	{
		// SQUARE = fire, CROSS = run, TRIANGLE/CIRCLE = use
		if (cur & PSP_CTRL_SQUARE)
			joyevent.data1 |= 1;
		if (cur & PSP_CTRL_CROSS)
			joyevent.data1 |= 4;
		if (cur & (PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE))
			joyevent.data1 |= 8;

		// LEFT/RIGHT = prev/next weapon
		if (PRESSED(PSP_CTRL_LEFT))
			psp_cycle_weapon(-1);
		if (PRESSED(PSP_CTRL_RIGHT))
			psp_cycle_weapon(1);

		// DOWN = automap, UP = automap zoom (whole map / normal)
		if (PRESSED(PSP_CTRL_DOWN))
			psp_tapkey (KEY_TAB);
		if (PRESSED(PSP_CTRL_UP) && automapactive)
			psp_tapkey ('0');
	}

	// L/R = turn, or strafe when swapped (keys released even with SELECT)
	psp_holdkey (cur, previous, PSP_CTRL_LTRIGGER,
		swap_turn ? ',' : KEY_LEFTARROW, !sel && !menuactive);
	psp_holdkey (cur, previous, PSP_CTRL_RTRIGGER,
		swap_turn ? '.' : KEY_RIGHTARROW, !sel && !menuactive);

	if (!sel)
	{
		// START = menu (single player pauses while it's up)
		if (PRESSED(PSP_CTRL_START))
			psp_tapkey (KEY_ESCAPE);
	}
	else
	{
		if (!menuactive)
		{
			// SELECT (+ R or L) + face buttons = launcher cheats
			static const int cheat_btn[4] = {
				PSP_CTRL_CIRCLE, PSP_CTRL_CROSS, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE
			};
			int bank = (cur & PSP_CTRL_RTRIGGER) ? 4 :
				(cur & PSP_CTRL_LTRIGGER) ? 8 : 0;
			int i;

			for (i=0; i<4; i++)
				if (PRESSED(cheat_btn[i]))
					psp_do_cheat(ctrl_cheat[bank + i]);

			// SELECT + RIGHT/LEFT = gamma up/down, SELECT + DOWN = detail
			if (PRESSED(PSP_CTRL_RIGHT))
				M_ChangeGamma(1);
			if (PRESSED(PSP_CTRL_LEFT))
				M_ChangeGamma(-1);
			if (PRESSED(PSP_CTRL_DOWN))
				psp_tapkey (KEY_F5);
		}
	}

    D_PostEvent (&joyevent);

	previous = cur;
}
