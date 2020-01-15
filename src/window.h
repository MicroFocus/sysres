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

#ifndef _SYSRES_WINDOW_H_
 #define _SYSRES_WINDOW_H_

#include <curses.h>    // WINDOW
#include "partition.h" // diskset

#ifdef PRODUCTION
 #define VERSION "1.3.0/PC:0.3.12"
#else
 #define VERSION "dev"
#endif

#define COPYRIGHT "(C) Copyright 2018-2019 Micro Focus or one if its affiliates"
#define PROGRAM "Micro Focus System Restore"
#define LINELIMIT 256
#define MAXPDEF 80
#define SELMAX 5		// plus 2 attached selectors, added in code
#define MAX_FILE 256
#define MAX_PATH 4096
#define MAX_ERASE 6


#define BUFSIZE 1024

// Window location definitions
#define WINMAX 5
#define WIN_SOURCE 0
#define WIN_TARGET 1
#define WIN_AUX1 2
#define WIN_AUX2 3
#define WIN_PROGRESS 4

// Selection list definitions
#define LIST_IMAGE 0
#define LIST_DRIVE 1
#define LIST_ERASE 2
#define LIST_SOURCE 3
#define LIST_TARGET 4
#define LIST_AUX1 5
#define LIST_AUX2 6

// selMap definitions
#define SEL_IMAGE 1
#define SEL_SOURCE 2
#define SEL_TARGET 4
#define SEL_MOUNTMASK 7 // masks 1,2,4
#define SEL_DRIVE 8
#define SEL_ERASE 16
#define SEL_IMAGE_AUX 32
#define SEL_DRIVE_AUX 64
#define SEL_ERASE_AUX 128
#define SEL_SOURCE_AUX 256
#define SEL_TARGET_AUX 512

// Operating mode definitions
#define RESTORE 0
#define BACKUP 1
#define ERASE 2
#define TRANSFER 3
#define LIST 4

#define CLEAR 1
#define REFRESH 2
#define UP 3
#define DOWN 4
#define INIT 5
#define PGUP 6
#define PGDOWN 7
#define BLANK 8
#define DISKUP 9
#define DISKDOWN 10

#define OPT_NETWORK 1
#define OPT_BACKUP 2
#define OPT_ERASE 4
#define OPT_TRANSFER 8
#define OPT_RAMDISK 16
#define OPT_RESTORE 32
#define OPT_ANYPART 64
#define OPT_REBOOT 128
#define OPT_HALT 256
#define OPT_AUTO 512
#define OPT_PRE 1024
#define OPT_POST 2048
#define OPT_BUFFER 4096
#define OPT_APPEND 8192

// colors (1-7 are part of item state)
#define CLR_BUSY 1	// used to identify occupied MBRs (actual color is default)
#define CLR_WDIM 2	// B,B|A_BOLD == dim text
#define CLR_RB 3	// mounted partition
#define CLR_YB 4	// mounted parts
#define CLR_BLB 5	// ram disk
#define CLR_GB 6	// valid partition

#define CLR_MASK 7	// 00000111

#define CLR_BDIM 	8 // dim border, COLOR_CYAN(modified),COLOR_BLACK; not saved to state
#define CLR_BRIGHTRED	12
#define CLR_BRIGHTGREEN 13
#define CLR_PROGRESSDIM 14
#define CLR_PROGRESS	15

// states
// #define ST_GROUP 8   // ...1000 'X' has been applied, selector only (does not affect MBR flag)
// #define ST_SEL 16    // 0010000	'-' image/selected	/ partImage / mbr + track 1
// #define ST_FILE 48   // 0110000 	'+' fsarchive, full disk as images   / mbr only
// #define ST_FULL 80   // 1010000	'x' dd 	/ mbr + dd of empty space
// #define ST_MASK 112  // 1110000  'X' dd the entire disk (mbr only; invalidates all other partition selections)
// #define ST_NEGATE 96 // 110....   toggles state on/off
// #define ST_NOSEL 128 // 1.......
// #define ST_BLOCK 248  //  11111000

#define ST_SEL 8	// ....1000
#define ST_FILE 24	// ...11000		'+'
#define ST_FULL 40	// ..101000		'x'
#define ST_IMG 56	// ..111000		'*'
#define ST_CLONE 72	// .1001000		'#'
#define ST_SWAP 88	// .1011000		's'
#define ST_MAX 152	// 10011000
#define ST_AUTO 168     // 10101000
#define ST_BLOCK 248    // 11111000
#define ST_NOSEL 247    // 11110111
#define ST_GROUP 128    // 10000000
#define ST_NEGATE 112	// 01110000
#define ST_GRPNEG 240   // 11110000
#define ST_GROUPED 136	// 10001000

// image assignment states

#define SI_PART 24
#define SI_MBR 40		// could also be a raw drive of some type (i.e., no partitions)
#define SI_LOOP 56
#define SI_AUTO 168

#define MAP_LOOP 1
#define MAP_PARTIAL 2
#define MAP_COMPLETE 3
#define MAP_RESTORE 4
#define MAP_INCOMPLETE 5
#define MAP_CUSTOM 6
#define MAP_DIRECT 7
#define MAP_BACKUP 8

/*----------------------------------------------------------------------------
** Global storage.
*/
extern diskset dset;
extern selection sels[];
extern WINDOW *wins[];
extern image currentImage;
extern char globalPath[];
extern int options;
extern short mode;
extern int currentFocus;
extern char setup;
extern char modeset;
extern char shellset;
extern bool performVerify;
extern bool performValidate;

/*----------------------------------------------------------------------------
** Function prototypes.
*/
extern selection *currentWindowSelection(void);
extern void showFunction(int pos, char *key, char *name, int action);
extern void autoMount(void);
extern void navigator(void);
extern void allWindows(int action);
extern unsigned long getAvailableBuffer(void);

#endif /* _SYSRES_WINDOW_H_ */
