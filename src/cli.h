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

#ifndef _CLI_H_
 #define _CLI_H_

#include <pthread.h>    // pthread_t

#include "partition.h"  // navImage

/*----------------------------------------------------------------------------
** Global storage.
*/
extern char      driveSelected;
extern navImage  image1;
extern navImage  image2;
extern char      ui_mode;
extern char      add_img;
extern char      testMode;
extern char      validOperation;
extern volatile pthread_t threadTID;

/*----------------------------------------------------------------------------
** Function prototypes.
*/
extern int debug(char exitLevel, int level, char *format, ...);

#endif /* _CLI_H_ */
