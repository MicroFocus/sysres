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
#include <stdlib.h> // EXIT_SUCCESS
#include <errno.h> 	// EINVAL
#include <string.h>	// strlen() strcat()

/*----------------------------------------------------------------------------
** strcat() like function for allocated memory
*/
int SYSRES_STRING_CatA(
		char 				**IO_string_A,
		const char 	 *I__stringTail
		)
	{
	int rCode=EXIT_SUCCESS;
	size_t stringTailLen;
	size_t stringLen = 0;
	char *string_A   = NULL;

	if(!I__stringTail || !IO_string_A)
		{
		rCode=EINVAL;
		goto CLEANUP;
		}

	if(*IO_string_A)
		stringLen = strlen(*IO_string_A);

	stringTailLen = strlen(I__stringTail);
	if(!stringTailLen)
		goto CLEANUP;

	errno=EXIT_SUCCESS;
	string_A=realloc(IO_string_A, stringLen + stringTailLen + 1);
	if(!string_A)
		{
		rCode=errno;
		goto CLEANUP;
		}

	strcat(string_A, I__stringTail);

//RESULTS:
	*IO_string_A = string_A;
	string_A = NULL;

CLEANUP:

	return(rCode);
	}

