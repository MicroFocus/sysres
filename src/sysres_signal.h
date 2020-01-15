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

#ifndef _SYSRES_SIGNAL_H_
 #define _SYSRES_SIGNAL_H_

/*----------------------------------------------------------------------------
** Compiler setup.
*/
#include <signal.h>  // sig_atomic_t

/*----------------------------------------------------------------------------
** Global storage.
*/
extern volatile sig_atomic_t SYSRES_SIGNAL_hasInterrupted;

/*----------------------------------------------------------------------------
** Macro values
*/

/*----------------------------------------------------------------------------
** Function prototypes.
*/
extern void SYSRES_SIGNAL_SIGINT_Handler(int I__signal);

#endif /* _SYSRES_SIGNAL_H_ */
