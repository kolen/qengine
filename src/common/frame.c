/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Generic frame handling.
 *
 * =======================================================================
 */

#include "header/common.h"
#include "header/zone.h"
#include <setjmp.h>

cvar_t *developer;
cvar_t *modder;
cvar_t *timescale;
cvar_t *fixedtime;
cvar_t *cl_maxfps;
cvar_t *dedicated;

extern cvar_t *logfile_active;
extern jmp_buf abortframe; /* an ERR_DROP occured, exit the entire frame */
extern zhead_t z_chain;

#ifndef DEDICATED_ONLY
FILE *log_stats_file;
cvar_t *cl_async;
cvar_t *vid_maxfps;
cvar_t *host_speeds;
cvar_t *log_stats;
cvar_t *showtrace;
#endif

// Forward declarations
#ifndef DEDICATED_ONLY
int gfx_get_refresh_rate();
qboolean R_IsVSyncActive(void);
#endif

/* host_speeds times */
int time_before_game;
int time_after_game;
int time_before_ref;
int time_after_ref;

// Used in the network- and input pathes.
int curtime;

#ifndef DEDICATED_ONLY
void Key_Init(void);
void SCR_EndLoadingPlaque(void);
#endif

// ----

void Qcommon_Init(int argc, char **argv)
{
  // Jump point used in emergency situations.
  if (setjmp(abortframe)) {
    Sys_Error("Error during initialization");
  }

  // Initialize zone malloc().
  z_chain.next = z_chain.prev = &z_chain;

  // Start early subsystems.
  COM_InitArgv(argc, argv);
  Swap_Init();
  Cbuf_Init();
  Cmd_Init();
  Cvar_Init();

#ifndef DEDICATED_ONLY
  Key_Init();
#endif

  /* we need to add the early commands twice, because
     a basedir or cddir needs to be set before execing
     config files, but we want other parms to override
     the settings of the config files */
  Cbuf_AddEarlyCommands(false);
  Cbuf_Execute();

  // The filesystems needs to be initialized after the cvars.
  FS_InitFilesystem();

  // Add and execute configuration files.
  Cbuf_AddText("exec default.cfg\n");
  Cbuf_AddText("exec config.cfg\n");
  Cbuf_AddText("exec autoexec.cfg\n");
  Cbuf_AddEarlyCommands(true);
  Cbuf_Execute();

  // Zone malloc statistics.
  Cmd_AddCommand("z_stats", Z_Stats_f);

  // cvars

  cl_maxfps = Cvar_Get("cl_maxfps", "60", CVAR_ARCHIVE);

  developer = Cvar_Get("developer", "0", 0);
  fixedtime = Cvar_Get("fixedtime", "0", 0);

  logfile_active = Cvar_Get("logfile", "1", CVAR_ARCHIVE);
  modder = Cvar_Get("modder", "0", 0);
  timescale = Cvar_Get("timescale", "1", 0);

  char *s;
  s = va("%s %s", ENGINE_VERSION, BUILD_DATE);
  Cvar_Get("version", s, CVAR_SERVERINFO | CVAR_NOSET);

#ifndef DEDICATED_ONLY
  cl_async = Cvar_Get("cl_async", "1", CVAR_ARCHIVE);
  dedicated = Cvar_Get("dedicated", "0", CVAR_NOSET);
  vid_maxfps = Cvar_Get("vid_maxfps", "95", CVAR_ARCHIVE);
  host_speeds = Cvar_Get("host_speeds", "0", 0);
  log_stats = Cvar_Get("log_stats", "0", 0);
  showtrace = Cvar_Get("showtrace", "0", 0);
#else
  dedicated = Cvar_Get("dedicated", "1", CVAR_NOSET);
#endif

  // We can't use the clients "quit" command when running dedicated.
  if (dedicated->value) {
    Cmd_AddCommand("quit", Com_Quit);
  }

  // Start late subsystem.
  Sys_Init();
  NET_Init();
  Netchan_Init();
  SV_Init();
#ifndef DEDICATED_ONLY
  CL_Init();
#endif

  // Everythings up, let's add + cmds from command line.
  if (!Cbuf_AddLateCommands()) {
    if (dedicated->value) {
      Cbuf_AddText("dedicated_start\n");
    } else {
      Cbuf_AddText("start\n");
    }

    Cbuf_Execute();
  }
#ifndef DEDICATED_ONLY
  else {
    SCR_EndLoadingPlaque();
  }
#endif
}

#ifndef DEDICATED_ONLY
void Qcommon_Frame(int msec)
{
  // Used for the dedicated server console.
  char *s;

  // Statistics.
  int time_before = 0;
  int time_between = 0;
  int time_after;

  // Target packetframerate.
  int pfps;

  // Target renderframerate.
  int rfps;

  // Time since last packetframe in microsec.
  static int packetdelta = 1000000;

  // Time since last renderframe in microsec.
  static int renderdelta = 1000000;

  // Accumulated time since last client run.
  static int clienttimedelta = 0;

  // Accumulated time since last server run.
  static int servertimedelta = 0;

  /* A packetframe runs the server and the client,
     but not the renderer. The minimal interval of
     packetframes is about 10.000 microsec. If run
     more often the movement prediction in pmove.c
     breaks. That's the Q2 variant if the famous
     125hz bug. */
  qboolean packetframe = true;

  /* A rendererframe runs the renderer, but not the
     client. The minimal interval is about 1000
     microseconds. */
  qboolean renderframe = true;

  /* In case of ERR_DROP we're jumping here. Don't know
     if that' really save but it seems to work. So leave
     it alone. */
  if (setjmp(abortframe)) {
    return;
  }

  if (log_stats->modified) {
    log_stats->modified = false;

    if (log_stats->value) {
      if (log_stats_file) {
        fclose(log_stats_file);
        log_stats_file = 0;
      }

      log_stats_file = fopen("stats.log", "w");

      if (log_stats_file) {
        fprintf(log_stats_file, "entities,dlights,parts,frame time\n");
      }
    } else {
      if (log_stats_file) {
        fclose(log_stats_file);
        log_stats_file = 0;
      }
    }
  }

  // Timing debug crap. Just for historical reasons.
  if (fixedtime->value) {
    msec = (int) fixedtime->value;
  } else if (timescale->value) {
    msec *= timescale->value;
  }

  if (showtrace->value) {
    extern int c_traces, c_brush_traces;
    extern int c_pointcontents;

    Com_Printf("%4i traces  %4i points\n", c_traces, c_pointcontents);
    c_traces = 0;
    c_brush_traces = 0;
    c_pointcontents = 0;
  }

  // vid_maxfps > 1000 breaks things, and so does <= 0
  // so cap to 1000 and treat <= 0 as "as fast as possible", which is 1000
  if (vid_maxfps->value > 1000 || vid_maxfps->value < 1) {
    Cvar_SetValue("vid_maxfps", 1000);
  }

  if (cl_maxfps->value > 250) {
    Cvar_SetValue("cl_maxfps", 130);
  } else if (cl_maxfps->value < 1) {
    Cvar_SetValue("cl_maxfps", 60);
  }

  // Save global time for network- und input code.
  curtime = Sys_Milliseconds();

  // Calculate target packet- and renderframerate.
  if (R_IsVSyncActive()) {
    rfps = gfx_get_refresh_rate();

    if (rfps > vid_maxfps->value) {
      rfps = (int) vid_maxfps->value;
    }
  } else {
    rfps = (int) vid_maxfps->value;
  }

  pfps = (cl_maxfps->value > rfps) ? rfps : cl_maxfps->value;

  // Calculate timings.
  packetdelta += msec;
  renderdelta += msec;
  clienttimedelta += msec;
  servertimedelta += msec;

  if (cl_async->value) {
    // Network frames..
    if (packetdelta < (1000000.0f / pfps)) {
      packetframe = false;
    }

    // Render frames.
    if (renderdelta < (1000000.0f / rfps)) {
      renderframe = false;
    }
  } else {
    // Cap frames at target framerate.
    if (renderdelta < (1000000.0f / rfps)) {
      renderframe = false;
      packetframe = false;
    }
  }

  // Dedicated server terminal console.
  do {
    s = Sys_ConsoleInput();

    if (s) {
      Cbuf_AddText(va("%s\n", s));
    }
  } while (s);

  Cbuf_Execute();

  if (host_speeds->value) {
    time_before = Sys_Milliseconds();
  }

  // Run the serverframe.
  if (packetframe) {
    SV_Frame(servertimedelta);
    servertimedelta = 0;
  }

  if (host_speeds->value) {
    time_between = Sys_Milliseconds();
  }

  // Run the client frame.
  if (packetframe || renderframe) {
    CL_Frame(packetdelta, renderdelta, clienttimedelta, packetframe, renderframe);
    clienttimedelta = 0;
  }

  if (host_speeds->value) {
    int all, sv, gm, cl, rf;

    time_after = Sys_Milliseconds();
    all = time_after - time_before;
    sv = time_between - time_before;
    cl = time_after - time_between;
    gm = time_after_game - time_before_game;
    rf = time_after_ref - time_before_ref;
    sv -= gm;
    cl -= rf;
    Com_Printf("all:%3i sv:%3i gm:%3i cl:%3i rf:%3i\n", all, sv, gm, cl, rf);
  }

  // Reset deltas if necessary.
  if (packetframe) {
    packetdelta = 0;
  }

  if (renderframe) {
    renderdelta = 0;
  }
}
#else

void Qcommon_Frame(int msec)
{
  // For the dedicated server terminal console.
  char *s;

  // Target packetframerate.
  int pfps;

  // Time since last packetframe in microsec.
  static int packetdelta = 1000000;

  // Accumulated time since last server run.
  static int servertimedelta = 0;

  /* A packetframe runs the server and the client,
     but not the renderer. The minimal interval of
     packetframes is about 10.000 microsec. If run
     more often the movement prediction in pmove.c
     breaks. That's the Q2 variant if the famous
     125hz bug. */
  qboolean packetframe = true;

  /* In case of ERR_DROP we're jumping here. Don't know
     if that' really save but it seems to work. So leave
     it alone. */
  if (setjmp(abortframe)) {
    return;
  }

  // Timing debug crap. Just for historical reasons.
  if (fixedtime->value) {
    msec = (int) fixedtime->value;
  } else if (timescale->value) {
    msec *= timescale->value;
  }

  // Save global time for network- und input code.
  curtime = Sys_Milliseconds();

  // Target framerate.
  pfps = (int) cl_maxfps->value;

  // Calculate timings.
  packetdelta += msec;
  servertimedelta += msec;

  // Network frame time.
  if (packetdelta < (1000000.0f / pfps)) {
    packetframe = false;
  }

  // Dedicated server terminal console.
  do {
    s = Sys_ConsoleInput();

    if (s) {
      Cbuf_AddText(va("%s\n", s));
    }
  } while (s);

  Cbuf_Execute();

  // Run the serverframe.
  if (packetframe) {
    SV_Frame(servertimedelta);
    servertimedelta = 0;
  }

  // Reset deltas if necessary.
  if (packetframe) {
    packetdelta = 0;
  }
}

#endif

void Qcommon_Shutdown(void)
{
  Cvar_Fini();
}
