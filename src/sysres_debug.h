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

#ifndef _SYSRES_DEBUG_H_
 #define _SYSRES_DEBUG_H_

/*----------------------------------------------------------------------------
** Memory structures.
*/
typedef enum SYSRES_DEBUG_level_E
	{
	SYSRES_DEBUG_level_DEFAULT_D	=	0,
	SYSRES_DEBUG_level_10_D				= 10
	} SYSRES_DEBUG_level_T;

typedef enum SYSRES_DEBUG_exitLevel_E
	{
	SYSRES_DEBUG_exitLevel_INFO_D	 = 0,
  SYSRES_DEBUG_exitLevel_ABORT_D = 1,
	SYSRES_DEBUG_exitLevel_EXIT_D  = 2
	} SYSRES_DEBUG_exitLevel_T;

/*----------------------------------------------------------------------------
** Macros
*/
#define SYSRES_DEBUG_FILEPATH_SYSRES_D "/tmp/sysres_debug.txt"
#define SYSRES_DEBUG_FILEPATH_PCLONE_D "/tmp/pclone_debug.txt"

#define SYSRES_DEBUG(exitLevel, level, format, ...) \
	SYSRES_DEBUG_Debug(exitLevel, level, format, ##__VA_ARGS__)

#define SYSRES_DEBUG_INFO(level, format, ...) \
	SYSRES_DEBUG_Debug(SYSRES_DEBUG_exitLevel_INFO_D, level, format, ##__VA_ARGS__)

#define SYSRES_DEBUG_ABORT(level, format, ...) \
	SYSRES_DEBUG_Debug(SYSRES_DEBUG_exitLevel_ABORT_D, level, format, ##__VA_ARGS__)

#define SYSRES_DEBUG_EXIT(level, format, ...) \
	SYSRES_DEBUG_Debug(SYSRES_DEBUG_exitLevel_EXIT_D, level, format, ##__VA_ARGS__)

// ABJ TODO Temporary compatibility...
#define debug(exitLevel, level, format, ...) \
  SYSRES_DEBUG_Debug(exitLevel, level, format, ##__VA_ARGS__)

/*----------------------------------------------------------------------------
** Storage
*/
extern SYSRES_DEBUG_level_T SYSRES_DEBUG_level; // was: debugLevel

/*----------------------------------------------------------------------------
** Function prototypes.
*/
extern int SYSRES_DEBUG_Debug(
		SYSRES_DEBUG_exitLevel_T  exitLevel,
		int                       level,
		char                     *format,
		...
		);

extern int SYSRES_DEBUG_Cat(
		const char *I__filePath
		);

#endif /* _SYSRES_DEBUG_H_ */
