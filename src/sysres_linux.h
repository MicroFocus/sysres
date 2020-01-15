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

#ifndef _SYSRES_LINUX_H_
 #define _SYSRES_LINUX_H_

/*----------------------------------------------------------------------------
** Global storage.
*/

/*----------------------------------------------------------------------------
** Macro values
*/

  #define SYSRES_LINUX_KERN_LOGMODE_EMERG_D     " 0 "   /*  Emergency messages, system is about to crash or is unstable.                            */
  #define SYSRES_LINUX_KERN_LOGMODE_ALERT_D     " 1 "   /*  Something bad happened and action must be taken immediately.                            */
  #define SYSRES_LINUX_KERN_LOGMODE_CRIT_D      " 2 "   /*  A critical condition occurred like a serious hardware/software failure.                 */
  #define SYSRES_LINUX_KERN_LOGMODE_ERR_D       " 3 "   /*  An error condition, often used by drivers to indicate difficulties with the hardware.   */
  #define SYSRES_LINUX_KERN_LOGMODE_WARNING_D   " 4 "   /*  A warning, meaning nothing serious by itself but might indicate problems.               */
  #define SYSRES_LINUX_KERN_LOGMODE_NOTICE_D    " 5 "   /*  Nothing serious, but notably nevertheless. Often used to report security events.        */
  #define SYSRES_LINUX_KERN_LOGMODE_INFO_D      " 6 "   /*  Informational message e.g. startup information at driver initialization.                */
  #define SYSRES_LINUX_KERN_LOGMODE_DEBUG_D     " 7 "   /*  Debug messages.                                                                         */
  #define SYSRES_LINUX_KERN_LOGMODE_DEFAULT_D   " d "   /*  The default kernel loglevel.                                                            */
  #define SYSRES_LINUX_KERN_LOGMODE_CONT_D      ""      /*  "continued" line of log printout (only done after a line that had no enclosing \n).     */

  #define SYSRES_LINUX_KERN_LOGMODE_EXIT_D                           \
    SYSRES_LINUX_KERN_LOGMODE_INFO_D         /* Console log level */ \
    SYSRES_LINUX_KERN_LOGMODE_WARNING_D      /* Default log level */ \
    SYSRES_LINUX_KERN_LOGMODE_ALERT_D        /* Minimum log level */ \
    SYSRES_LINUX_KERN_LOGMODE_DEBUG_D        /* Maximum log level */

  #define SYSRES_LINUX_KERN_LOGMODE_INIT_D                           \
    SYSRES_LINUX_KERN_LOGMODE_ERR_D          /* Console log level */ \
    SYSRES_LINUX_KERN_LOGMODE_NOTICE_D       /* Default log level */ \
    SYSRES_LINUX_KERN_LOGMODE_NOTICE_D       /* Minimum log level */ \
    SYSRES_LINUX_KERN_LOGMODE_DEBUG_D        /* Maximum log level */

/*----------------------------------------------------------------------------
** Function prototypes.
*/
extern void SYSRES_LINUX_SetKernelLogging(char *I__levels);

#endif /* _SYSRES_LINUX_H_ */
