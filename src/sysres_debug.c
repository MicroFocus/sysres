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

/*----------------------------------------------------------------------------
** Compiler setup.
*/
#define _LARGEFILE64_SOURCE		// stat64()
#define _GNU_SOURCE 					// vasprintf()

#include <errno.h>						// errno
#include <fcntl.h>						// open()
#include <stdarg.h>						// vsnprintf()
#include <stdbool.h>					// bool
#include <stdio.h>						// vasprintf()
#include <stdlib.h>						// exit()
#include <string.h>						// strlen()
#include <sys/types.h>				// stat64() open()
#include <sys/stat.h>					// stat64()	open()
#include <unistd.h>						// stat64() exit()
#include <termios.h>					// initial_settings()
#include <sys/ioctl.h>        // ioctl()


#include "cli.h"							// ui_mode
#include "operator.h"					// abortOperation(), exitOperation()
#include "partition.h"				// ABORT
#include "sysres_debug.h"			// Self-aware
#include "sysres_filesys.h"		// SYSRES_FILESYS_FileExists()
#include "sysres_string.h"		// SYSRES_STRING_CatA()
#include "sysres_xterm.h"			// SYSRES_XTERM_CURSOR_ON

/*----------------------------------------------------------------------------
** Storage
*/
SYSRES_DEBUG_level_T SYSRES_DEBUG_level = SYSRES_DEBUG_level_DEFAULT_D; // was: debugLevel

/*----------------------------------------------------------------------------
** Read a single key from keyboard.
*/
int SYSRES_DEBUG_GetChar(
		int *_O_char
		)
	{
  int n;
	struct termios initial_settings, new_settings;

  unsigned char key;

  tcgetattr(0,&initial_settings);

  new_settings = initial_settings;
  new_settings.c_lflag &= ~ICANON;
  new_settings.c_lflag &= ~ECHO;
  new_settings.c_lflag &= ~ISIG;
//  new_settings.c_cc[VMIN] = 0;
  new_settings.c_cc[VTIME] = 0;

  tcsetattr(0, TCSANOW, &new_settings);

   while(1)
  {
    n = getchar();

    if(n != EOF)
    {
      key = n;

    if(_O_char)
			*_O_char = key;

		break;
    }
  }

  tcsetattr(0, TCSANOW, &initial_settings);

	return(0);
	}

/*----------------------------------------------------------------------------
** Cat a file.
*/
int SYSRES_DEBUG_Cat(
		const char *I__filePath
		)
	{
	int rCode=EXIT_SUCCESS;
	char *buf_A = NULL;
	struct winsize w;
	FILE *fp = NULL;
	int line;
	bool done=false;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

	printf("\033[2J");  // Clear screen
	printf("\033[H");		// Home cursor
	fflush(stdout);

	errno=EXIT_SUCCESS;
	buf_A = malloc(w.ws_col + 1);
	if(!buf_A)
		{
		rCode=errno;
		goto CLEANUP;
		}

	errno=EXIT_SUCCESS;
	fp=fopen(I__filePath, "r");
	if(!fp)
		{
		rCode=errno;
		goto CLEANUP;
		}

	do
		{
		for(line=0; line < w.ws_row -1; ++line)
			{
			char *cp;

			errno=EXIT_SUCCESS;
			if(!fgets(buf_A, w.ws_col + 1, fp))
				{
				done=true;
				break;
				}

			cp=strchr(buf_A, '\n');
			if(cp)
				*cp = '\0';

			printf("%s\n", buf_A);
			}

		printf("[SPACE]=%s [Q]=Quit", done?"End":"More");
  	printf(" (L[%d] C[%d])", w.ws_row, w.ws_col);

  	fflush(stdout);

		for(;;)
			{
			int ch;

			SYSRES_DEBUG_GetChar(&ch);
			switch(ch)
				{
				case 'q':
				case 'Q':
					done=true;
					printf("%c", ch);
					fflush(stdout);
					break;

				case ' ':
					printf("%c", ch);
					fflush(stdout);
					break;

				default:
					continue;
				}

			break;
			}

		} while(!done);

CLEANUP:

	if(fp)
		{
		errno=EXIT_SUCCESS;
		if(EOF == fclose(fp))
			{
			if(!rCode)
				rCode=errno;
			}
		}

	if(buf_A)
		{
		free(buf_A);
		}

	return(rCode);
	}

/*----------------------------------------------------------------------------
**
*/
static int SYSRES_DEBUG_RotateIfNeeded(void)
	{
	int           rCode = EXIT_SUCCESS;
	struct stat64 statBuf;

	if((-1) == stat64(SYSRES_DEBUG_FILEPATH_SYSRES_D, &statBuf))
		{
		rCode=errno;
		goto CLEANUP;
		}

	if(statBuf.st_size < 100 * 1024)
		goto CLEANUP;

	// file exists, > 100K
	if((-1) == rename(SYSRES_DEBUG_FILEPATH_SYSRES_D, SYSRES_DEBUG_FILEPATH_SYSRES_D ".1"))
		{
		rCode=errno;
		goto CLEANUP;
		}

CLEANUP:

	return(rCode);
	}

/*----------------------------------------------------------------------------
** level == 0, only print on debug mode
*/
int SYSRES_DEBUG_Debug(
		SYSRES_DEBUG_exitLevel_T  exitLevel,
		int                       level,
		char                     *format,
		...
		)
	{
	int      rCode=EXIT_SUCCESS;
	va_list  args;
	char    *dbuf_A = NULL;
	int      fd = (-1);

	va_start(args, format);
	if((-1) == vasprintf(&dbuf_A, format, args))
    {
    rCode = EXIT_FAILURE;
    goto CLEANUP;
    }

	va_end(args);

	SYSRES_DEBUG_RotateIfNeeded();

	// provide feedback to file
	errno = EXIT_SUCCESS;
	fd = open(SYSRES_DEBUG_FILEPATH_SYSRES_D,O_CREAT|O_WRONLY|O_APPEND|O_LARGEFILE,S_IRUSR | S_IWUSR);
	if((-1) == fd)
		{
		rCode=errno;
		goto CLEANUP;
		}

  if((-1) == write(fd, dbuf_A, strlen(dbuf_A)))
    {
    rCode = errno;
    goto CLEANUP;
    }

  if(ABORT == exitLevel)
    {
    if((-1) == write(fd,".\n",2))
      {
      rCode = errno;
      goto CLEANUP;
      }
    }

	// provide feedback to user
	if(!ui_mode)
		{
		if(ABORT == exitLevel)
			{
			rCode=SYSRES_STRING_CatA(&dbuf_A, ".\n");
			if(rCode)
				goto CLEANUP;
			}

		if(INFO != exitLevel)
			printf("%s", SYSRES_XTERM_CURSOR_ON);

		if(level && (ABORT == exitLevel))
			progressBar(0, 0, PROGRESS_FAIL);

		if(SYSRES_DEBUG_level > level || (EXIT == exitLevel) || (ABORT == exitLevel))
			fprintf(stderr, "%s", dbuf_A);

		if(INFO != exitLevel)
			exit((ABORT == exitLevel) ? 1 : level);

    goto CLEANUP;
		}

	if(exitLevel == EXIT)
		{ // ncurses exit, reboot option
		exitOperation(dbuf_A,level);
    goto CLEANUP;
		}

	if(exitLevel == ABORT)
		{ // should also set an 'E' annunciator to identifiy previous error
		abortOperation(dbuf_A,level);
    goto CLEANUP;
		}

CLEANUP:

  if((-1) != fd)
    {
    int rc = close(fd);
    if((-1) == rc)
      {
      if(!rCode)
        rCode=errno;
      }

    fd = (-1);
    }

	if(dbuf_A)
		{
		free(dbuf_A);
		dbuf_A = NULL;
		}

	return(rCode);
	}


