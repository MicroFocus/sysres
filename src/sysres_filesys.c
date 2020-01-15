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
#include <errno.h>						// errno
#include <stdio.h>						// FILE, fopen(), printf(), fclose(), EOF
#include <stdlib.h>           // EXIT_SUCCESS
#include <string.h>						// strchr()
#include <unistd.h>						// F_OK, access()
#include "sysres_filesys.h"		// Self-aware

/*----------------------------------------------------------------------------
**
*/
int SYSRES_FILESYS_FileExists(
		const char *I__filePath
		)
	{
	int rCode=EXIT_SUCCESS;

	errno=EXIT_SUCCESS;
	if((-1) == access(I__filePath, F_OK))
		rCode=errno;

	return(rCode);
	}

/*----------------------------------------------------------------------------
**
*/
int SYSRES_FILESYS_More(
		const char *I__filePath
		)
	{
	int    rCode   = EXIT_SUCCESS;
	FILE  *fp      = NULL;
	size_t lineCnt = 0;
	char   line[79+1];


mvprintw(10, 1, "XXXXXXXXXXXX TOP XXXXXXXXXXXXXXXX");
refresh();
getch();

	if(!I__filePath)
		{
		rCode=EINVAL;
		goto CLEANUP;
		}

mvprintw(11, 1, "XXXXXXXXXXXX TP01 XXXXXXXXXXXXXXXX");
refresh();
getch();

	if(SYSRES_FILESYS_FileExists(I__filePath))
		goto CLEANUP;

mvprintw(12, 1, "XXXXXXXXXXXX EXISTS XXXXXXXXXXXXXXXX");
refresh();
getch();

	errno=EXIT_SUCCESS;
	fp=fopen(I__filePath, "r");
	if(!fp)
		{
		rCode=errno;
		goto CLEANUP;
		}

//mvprintw(13, 1, "XXXXXXXXXXXX READ XXXXXXXXXXXXXXXX");
//refresh();


	while(fgets(line, sizeof(line), fp))
		{
		char *cp = strchr(line, '\n');
		if(cp)
			*cp = '\0';

		printf("%s\n");
		++lineCnt;
		if(!(lineCnt % 20))
			{
			printf("More...");
			getchar();
			printf("\r        \r");
			}
		}

	printf("Done...");
	getchar();
	printf("\r        \r");

CLEANUP:

	if(fp)
		{
		errno=0;
		if(EOF == fclose(fp))
			{
			if(!rCode)
				rCode=errno;
			}
		}

	printf("EXIT...");
	getch();
	printf("\r        \r");

	return(rCode);
	}


