#include <pspkernel.h>
#include <pspdebug.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <netinet/in.h>

#include <pspsdk.h>

#include "i_system.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "m_swap.h"

#include "doomstat.h"

#include "i_net.h"


#define printf pspDebugScreenPrintf


void cleanup_net (void);

// set by the launcher lobby (psp_main.c) before D_DoomMain
extern int psp_net_enabled;
extern int psp_net_player1;              // our player number, 1 = host
extern int psp_adhoc_numnodes;           // players in the game, us included
extern unsigned char psp_adhoc_mac[][6]; // [0] is us, then the other players


//
// NETWORKING
//

/**********************************************************************/
/**********************************************************************/
/* Ad-Hoc stuff */

// the launcher lobby uses ADHOC_DOOMPORT + 1
#define ADHOC_DOOMPORT	5029 // 5000 + 0x1d

static int ADHOC_pdp = -1;
static int ADHOC_connected = 0;

static void (*netget) (void);
static void (*netsend) (void);


/**********************************************************************/
//
// ADHOC_PacketSend
//
static void ADHOC_PacketSend (void)
{
  int  c;
  doomdata_t sw;

  // byte swap
  sw.checksum = htonl(netbuffer->checksum);
  sw.player = netbuffer->player;
  sw.retransmitfrom = netbuffer->retransmitfrom;
  sw.starttic = netbuffer->starttic;
  sw.numtics = netbuffer->numtics;
  for (c = 0 ; c < netbuffer->numtics; c++) {
    sw.cmds[c].forwardmove = netbuffer->cmds[c].forwardmove;
    sw.cmds[c].sidemove = netbuffer->cmds[c].sidemove;
    sw.cmds[c].angleturn = htons(netbuffer->cmds[c].angleturn);
    sw.cmds[c].consistancy = htons(netbuffer->cmds[c].consistancy);
    sw.cmds[c].chatchar = netbuffer->cmds[c].chatchar;
    sw.cmds[c].buttons = netbuffer->cmds[c].buttons;
  }

  //printf ("sending %i\n",gametic);
  sceKernelDelayThread(10);
  // a full send buffer just drops the packet, the game resends
  sceNetAdhocPdpSend (ADHOC_pdp, psp_adhoc_mac[doomcom->remotenode],
                      ADHOC_DOOMPORT, &sw, doomcom->datalength, 0, 1);
}


/**********************************************************************/
//
// ADHOC_PacketGet
//
static void ADHOC_PacketGet (void)
{
  int i, c;
  unsigned char frommac[6];
  unsigned short fromport;
  int len;
  doomdata_t sw;

  len = sizeof(sw);
  sceKernelDelayThread(10);
  c = sceNetAdhocPdpRecv (ADHOC_pdp, frommac, &fromport, &sw, &len, 0, 1);
  if (c < 0) {
    // nothing waiting (or a bad packet)
    doomcom->remotenode = -1;  // no packet
    return;
  }

  // find remote node number
  for (i = 1; i < doomcom->numnodes; i++)
    if (!memcmp (frommac, psp_adhoc_mac[i], 6))
      break;

  if (i == doomcom->numnodes) {
    // packet is not from one of the players
    doomcom->remotenode = -1;  // no packet
    return;
  }

  doomcom->remotenode = i;   // good packet from a game player
  doomcom->datalength = len;

  // byte swap
  netbuffer->checksum = ntohl(sw.checksum);
  netbuffer->player = sw.player;
  netbuffer->retransmitfrom = sw.retransmitfrom;
  netbuffer->starttic = sw.starttic;
  netbuffer->numtics = sw.numtics;

  for (c = 0; c < netbuffer->numtics; c++) {
    netbuffer->cmds[c].forwardmove = sw.cmds[c].forwardmove;
    netbuffer->cmds[c].sidemove = sw.cmds[c].sidemove;
    netbuffer->cmds[c].angleturn = ntohs(sw.cmds[c].angleturn);
    netbuffer->cmds[c].consistancy = ntohs(sw.cmds[c].consistancy);
    netbuffer->cmds[c].chatchar = sw.cmds[c].chatchar;
    netbuffer->cmds[c].buttons = sw.cmds[c].buttons;
  }
  //printf("good packet returned\n");
}


/**********************************************************************/
//
// ADHOC_InitNetwork
//
static void ADHOC_InitNetwork (void)
{
  // enters with the ad-hoc group joined and the players known (launcher lobby)
  printf("ADHOC_InitNetwork: player %d of %d\n", psp_net_player1, psp_adhoc_numnodes);

  netsend = ADHOC_PacketSend;
  netget = ADHOC_PacketGet;
  netgame = true;
  ADHOC_connected = 1;

  doomcom->consoleplayer = psp_net_player1 - 1;
  doomcom->numnodes = psp_adhoc_numnodes;

  doomcom->id = DOOMCOM_ID;
  doomcom->numplayers = doomcom->numnodes;

  ADHOC_pdp = sceNetAdhocPdpCreate (psp_adhoc_mac[0], ADHOC_DOOMPORT, 0x4000, 0);
  if (ADHOC_pdp < 0)
    I_Error ("can't create ad-hoc socket: %08X", ADHOC_pdp);
}

/**********************************************************************/
static void ADHOC_Shutdown (void)
{
  if (ADHOC_pdp >= 0) {
    sceNetAdhocPdpDelete (ADHOC_pdp, 0);
    ADHOC_pdp = -1;
  }
  if (ADHOC_connected) {
    sceNetAdhocctlDisconnect ();
    ADHOC_connected = 0;
  }
}

/**********************************************************************/
/**********************************************************************/
//
// I_InitNetwork
//
void I_InitNetwork (void)
{
  printf("I_InitNetwork()\n");

  atexit(cleanup_net);

  doomcom = malloc (sizeof (*doomcom) );
  if (!doomcom)
    I_Error ("I_InitNetwork: Couldn't allocate memory for doomcom.");
  memset (doomcom, 0, sizeof(*doomcom) );

  // set up for network
  // fixed: every player needs the same value and nothing negotiates it
  doomcom->ticdup = 1;

  if (M_CheckParm ("-extratic"))
    doomcom-> extratics = 1;
  else
    doomcom-> extratics = 0;

  // the launcher clears psp_net_enabled when the lobby fails or is cancelled
  if (psp_net_enabled && psp_adhoc_numnodes > 1) {
    ADHOC_InitNetwork ();
  } else {
    // single player game: the -extratic tuning is for network games only
    netgame = false;
    doomcom->extratics = 0;
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
  }
}


/**********************************************************************/
void I_NetCmd (void)
{
  if (doomcom->command == CMD_SEND) {
    netsend ();
  } else if (doomcom->command == CMD_GET) {
    netget ();
  } else
    I_Error ("Bad net cmd: %i\n",doomcom->command);
}

/**********************************************************************/
void cleanup_net (void)
{
  ADHOC_Shutdown ();
}

/**********************************************************************/
