#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "psp.h"

#define printf pspDebugScreenPrintf


#define MAXFILES 1000
#define PAGESIZE 16
#define NAMELEN 256
#define DIRLEN 512


static struct fileentry {
	char name[NAMELEN];
	int isdir;
} thefiles[MAXFILES];

static int maxfiles;
static char curdir[DIRLEN]; // directory listed in thefiles, ends in '/'


/****************************************************************************
 * get_buttons
 *
 ****************************************************************************/

static unsigned int get_buttons()
{
	SceCtrlData pad;

	sceCtrlReadBufferPositive(&pad, 1);
	return pad.Buttons;
}

// directories first, then by name
static int compare_entries (const void *a, const void *b)
{
	const struct fileentry *fa = a, *fb = b;

	if (fa->isdir != fb->isdir)
		return fb->isdir - fa->isdir;
	return strcasecmp(fa->name, fb->name);
}

/****************************************************************************
 * parse_dir
 *
 * List path into thefiles and make it the current directory.
 * Returns the number of entries, -1 if the directory can't be opened
 * (the current listing is then left alone).
 ****************************************************************************/

static int parse_dir (const char *path)
{
	DIR *dir;
	struct dirent *dirent;
	struct stat fstat;
	char file_name[DIRLEN + NAMELEN];
	char newdir[DIRLEN];
	int len;

	// path may point into curdir
	len = snprintf(newdir, sizeof(newdir) - 1, "%s", path);
	if (len >= (int)sizeof(newdir) - 1)
		return -1;
	if (len && newdir[len-1] != '/')
		strcat(newdir, "/");

	if ( ( dir = opendir( newdir ) ) == 0 )
		return -1;

	strcpy(curdir, newdir);
	maxfiles = 0;

	while ( maxfiles < MAXFILES && ( dirent = readdir( dir ) ) != 0 )
	{
		if ( dirent->d_name[0] == '.' ) continue;
		if ( strlen( dirent->d_name ) >= NAMELEN ) continue;
		snprintf( file_name, sizeof(file_name), "%s%s", curdir, dirent->d_name );
		if ( stat( file_name, &fstat ) == -1 ) continue;
		if ( !S_ISDIR( fstat.st_mode ) && !S_ISREG( fstat.st_mode ) ) continue;

		strcpy(thefiles[maxfiles].name, dirent->d_name);
		thefiles[maxfiles].isdir = S_ISDIR( fstat.st_mode ) ? 1 : 0;
		maxfiles++;
	}
	closedir( dir );

	qsort(thefiles, maxfiles, sizeof(thefiles[0]), compare_entries);

	return maxfiles;
}

// list the parent of the current directory; stays put at the device root
static void parse_parent (void)
{
	char parent[DIRLEN];
	char *colon, *slash;
	int root;

	strcpy(parent, curdir);
	colon = strchr(parent, ':');
	root = colon ? (colon - parent) + 2 : 1; // "ms0:/" or "/"

	if ((int)strlen(parent) > root)
	{
		parent[strlen(parent) - 1] = 0; // trailing '/'
		slash = strrchr(parent, '/');
		if (slash && slash - parent + 1 >= root)
			slash[1] = 0;
		else
			strcpy(parent, curdir);
	}
	parse_dir(parent);
}

/****************************************************************************
 * ShowFiles
 *
 * Support function for FileSelector
 ****************************************************************************/

static void ShowFiles( int offset, int selection )
{
	int i,j;
	char text[80];

	gui_PrePrint();

	if ( maxfiles == 0 )
		gui_Print("(empty)", 0xFFAAAAAA, 0, 240 - gui_PrintWidth("(empty)")/2, 16);

	j = 0;
	for ( i = offset; i < ( offset + PAGESIZE ) && i < maxfiles ; i++ )
	{
		snprintf(text, 69, thefiles[i].isdir ? "[%.66s]" : "%s", thefiles[i].name);

		gui_Print(text, j == (selection-offset) ? 0xFFFFFFFF : 0xFFAAAAAA, 0, 240 - gui_PrintWidth(text)/2, (i - offset + 1)*16);

		j++;
	}

	gui_PostPrint();
}

/****************************************************************************
 * FileSelector
 *
 * Press X to select, O to cancel, and Triangle to go back a level
 ****************************************************************************/

static int FileSelector()
{
	int offset = 0;
	int selection = 0;
	int havefile = 0;
	int redraw = 1;
	unsigned int p = get_buttons();

	while ( havefile == 0 && !(p & psp_btn_back) )
	{
		if ( redraw )
			ShowFiles( offset, selection );
		redraw = 0;

		while (!(p = get_buttons()) && !quit_requested)
			sceKernelDelayThread(10000);
		while (p == get_buttons() && !quit_requested)
			sceKernelDelayThread(10000);
		if ( quit_requested )
			break; // HOME -> Quit: the launcher exits

		// nothing to move over or pick in an empty directory
		if ( maxfiles == 0 )
			p &= ~(PSP_CTRL_DOWN | PSP_CTRL_UP | PSP_CTRL_RIGHT | PSP_CTRL_LEFT | psp_btn_ok);

		if ( p & PSP_CTRL_DOWN )
		{
			selection++;
			if ( selection == maxfiles )
				selection = offset = 0;	// wrap around to top

			if ( ( selection - offset ) == PAGESIZE )
				offset += PAGESIZE; // next "page" of entries

			redraw = 1;
		}

		if ( p & PSP_CTRL_UP )
		{
			selection--;
			if ( selection < 0 )
			{
				selection = maxfiles - 1;
				offset = maxfiles > PAGESIZE ? selection - PAGESIZE + 1 : 0; // wrap around to bottom
			}

			if ( selection < offset )
			{
				offset -= PAGESIZE; // previous "page" of entries
				if ( offset < 0 )
					offset = 0;
			}

			redraw = 1;
		}

		if ( p & PSP_CTRL_RIGHT )
		{
			selection += PAGESIZE;
			if ( selection >= maxfiles )
				selection = offset = 0;	// wrap around to top

			if ( ( selection - offset ) >= PAGESIZE )
				offset += PAGESIZE; // next "page" of entries

			redraw = 1;
		}

		if ( p & PSP_CTRL_LEFT )
		{
			selection -= PAGESIZE;
			if ( selection < 0 )
			{
				selection = maxfiles - 1;
				offset = maxfiles > PAGESIZE ? selection - PAGESIZE + 1 : 0; // wrap around to bottom
			}

			if ( selection < offset )
			{
				offset -= PAGESIZE; // previous "page" of entries
				if ( offset < 0 )
					offset = 0;
			}

			redraw = 1;
		}

		if ( p & psp_btn_ok )
		{
			if ( thefiles[selection].isdir )	/*** This is directory ***/
			{
				char fname[DIRLEN + NAMELEN];

				snprintf(fname, sizeof(fname), "%s%s/", curdir, thefiles[selection].name);
				// can't be opened: stay where we are
				if ( parse_dir(fname) >= 0 )
					offset = selection = 0;
			}
			else
				return selection;

			redraw = 1;
		}

		if ( p & PSP_CTRL_TRIANGLE )
		{
			offset = selection = 0;
			parse_parent();

			redraw = 1;
		}
	}

	return -1; // no file selected
}

/****************************************************************************
 * RequestFile
 *
 * return pointer to filename selected
 ****************************************************************************/

char *RequestFile (char *initialPath)
{
	int selection;
	static char fname[DIRLEN + NAMELEN];

	if (parse_dir(initialPath) < 0)
		return 0;

	selection = FileSelector ();
	if (selection < 0)
		return 0;

	snprintf (fname, sizeof(fname), "%s%s", curdir, thefiles[selection].name);

	return fname;
}
