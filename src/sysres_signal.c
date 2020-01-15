/*****************************************************************************
* sysres "System Restore" Partition backup and restore utility.
* Copyright © 2019-2020 Micro Focus or one of its affiliates.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

// (C) Copyright 2013 Hewlett-Packard Development Company, L.P.

/*----------------------------------------------------------------------------
** Compiler setup.
*/
  /* ANSI/POSIX */

  /* System Restore */
  #include "backup.h"         // backupPID
  #include "cli.h"            // threadTID
  #include "partition.h"      // INFO
  #include "sysres_debug.h"   // debug()
  #include "sysres_signal.h"  // Validate self-compatibility.

/*----------------------------------------------------------------------------
** Storage
*/
volatile sig_atomic_t SYSRES_SIGNAL_hasInterrupted = 0;

/*----------------------------------------------------------------------------
** catch Ctrl-C and Ctrl-Z activity (if there is a child process, just
** terminate that so the primary can read the interruption)
*/
void SYSRES_SIGNAL_SIGINT_Handler(int I__signal)
	{
	SYSRES_SIGNAL_hasInterrupted = 1;

  debug(INFO, 5,"CTRL-C [%i, %i, %i]\n", backupPID, threadTID, pthread_self() );
	if(backupPID)
		kill(backupPID,SIGQUIT); // interrupt pipe

	if(threadTID && threadTID == pthread_self())
		pthread_exit(0); // interrupt thread

	signal(I__signal, SYSRES_SIGNAL_SIGINT_Handler); // re-install handler

	return;
	}

