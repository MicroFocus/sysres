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
  #include <stdio.h>

  /* System Restore */
  #include "sysres_linux.h"          // Validate self-compatibility.

/*----------------------------------------------------------------------------
** Storage
*/

/*----------------------------------------------------------------------------
**
*/
void SYSRES_LINUX_SetKernelLogging(char *I__levels)
	{
	FILE *kfile = NULL;

	if(getuid())
		return;

	kfile = fopen("/proc/sys/kernel/printk","w");

	if(!kfile)
		return;

	fprintf(kfile, I__levels);
	fclose(kfile);

	return;
	}

