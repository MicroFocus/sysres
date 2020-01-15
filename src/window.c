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

#define __USE_LARGEFILE64
// #define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE

#include <limits.h>
#include <ncurses.h>
#include <limits.h>
#include <string.h>
#include <stdarg.h> 			// TEMPORARY: for variable-argument functions
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/reboot.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include "window.h"
#include "cli.h"					// add_img
#include "frame.h"				// expert
#include "image.h" 				// iset, imageSize
#include "partutil.h"			// readable
#include "position.h"			// busyMap
#include "restore.h"    	// performRestore(), locateImage(), globalPath2
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level
#include "sysres_linux.h" // SYSRES_LINUX_SetKernelLogging()

/* 336 = 1280x1024x16, 333 = 1024x768x16 [USE THIS], 340=800x600x32 */

int currentFocus = 0;
short mode = -1; // not yet set
image currentImage;
WINDOW *wins[WINMAX];
void manageWindow(selection *sel, int action);
char *getStringPtr(poolset *sel, char *str);
selection sels[SELMAX+2]; // includes two attached selectors
unsigned long totalMem = 0;
int mallocCount = 0;
#ifdef NETWORK_ENABLED
  unsigned long httpImageSize;
#endif
char *envp[] = { "TERM=linux-16color", "HOME=/tmp", NULL };
void debugDisks(int color);
/*

Add a delete F-key, and a format F-key which shows up in !restore and the selector is TARGET, and either a file or a non-partitioned drive is highlighted.

Add a 'Add drive' F-key

see if we can use partclone.org

*/

#ifdef PARTCLONE
int options = OPT_BACKUP | OPT_ERASE | OPT_RESTORE | OPT_TRANSFER; //  |  OPT_TRANSFER; //  | OPT_NETWORK; // OPT_NETWORK
#else
int options = OPT_RESTORE | OPT_ERASE;
#endif

diskset dset;

char globalPath[MAX_PATH]; // global scratch space

extern char selectedType; // current type of image displayed, if any

bool validMap = false;
char setup = 1; // prevent some things from happening (i.e., clearing of mappings) until we're done with setup
char modeset = 0;
char shellset = 0;

bool performVerify = false; // allows for verify (F10) ui from the command-line
bool performValidate = false; // allows for Ctrl-V ui from the command-line

int getPrimaryType(char location) { // 0 = current source, 1 = current target
	int res = LIST_IMAGE;
	if (location) { // current target
		switch(mode) {
			case RESTORE: res = LIST_DRIVE; break;
			case BACKUP: res = LIST_IMAGE; break;
			case ERASE: res = LIST_DRIVE; break;
			case TRANSFER: res = LIST_TARGET; break;
			}
		}
	else switch(mode) {
		case RESTORE: res = LIST_IMAGE; break;
		case BACKUP: res = LIST_DRIVE; break;
		case ERASE: res = LIST_ERASE; break;
		case TRANSFER: res = LIST_SOURCE; break;
		}
	return res;
	}

selection *currentWindowSelection(void) {
	if (currentFocus <= WIN_TARGET) return &sels[getPrimaryType(currentFocus)];
	return sels[getPrimaryType(currentFocus != WIN_AUX1)].attached;
	}

/* a source directory drive will only show up if it has a partition that is:
	1. FAT32/FAT16 (the first one available), or:
	2. ext2/3/4,xfs, and the system label
*/

void showFunction(int pos, char *key, char *name, int action) {
	if (action) return;
	attron(COLOR_PAIR(CLR_RB) | A_REVERSE);
	mvprintw(LINES-1,pos,"%s",key);
	attroff(COLOR_PAIR(CLR_RB) | A_REVERSE);
	printw("%s",name);
	}

void showCancel(char addl) {
	int i;
	char *addlEntry;
	mvaddch(LINES-1,0,' ');  clrtoeol(); // for (i=0;i<COLS;i++) addch(' '); // clear the menu line
	showFunction(COLS-(54+9),"F6","CANCEL",0);
	if (addl) {
		switch(addl) {
			case 1: addlEntry = "MKDIR"; break;
			case 2: addlEntry = "HTTP"; break;
			default: addlEntry = "CIFS"; break;
			// "BIND"
			}
		showFunction(0,"/",addlEntry,0);
		}
	}

/*
	F1:
	F2: REMOTE/SOURCE/TARGET, RESTORE, BACKUP
	F3:
	F4:
	F5: CANCEL, NEW
	F6: REMOVE
	F7: CLEAR, AUTO, /MKDIR
	F8: MODE (could be hidden)
	F9: RESCAN, RMDIR (rescan could be hidden as ctrl-r)
	Ctrl-E: BASIC, DETAIL (hidden as ctrl-e)
	F10: VERIFY
	F11: ABOUT
	F12: REBOOT
*/

unsigned long getAvailableBuffer(void) {
        struct statfs64 fsd;
        unsigned long bsize, bavail;
	// unsigned int bcount, bfree;
        if (statfs64("/mnt/ram",&fsd) < 0 ) return 0L; // couldn't read space avail
        bsize = fsd.f_bsize;
        // bcount = fsd.f_blocks;
        // bfree = fsd.f_bfree;
        // bavail = fsd.f_bavail;
	bavail = fsd.f_bavail;
	return bavail*bsize;
	}

/*
int getNextAuto(void) {
	if (auto_mode == NULL || !*auto_mode) return getch();
	if (auto_quote) { // we're in quotes
		if (*auto_mode == '\'') {
			if (auto_mode[1] == '\'') { auto_mode+=2; return '\''; } // single-quote
			auto_quote = false;
			auto_mode++;
			return getNextAuto();
			}
		return *auto_mode++;
		}
	if (*auto_mode == ',') { auto_mode++; return getNextAuto(); }
	if (auto_mode[1] == ',' || auto_mode[1] == 0) { return *auto_mode++; } // not a special char
	// handle a special char
	// F...
	// ^...
	}
*/

/*----------------------------------------------------------------------------
**
*/
void allWindows(int action)
	{
	manageWindow(&sels[getPrimaryType(0)],action);
	manageWindow(&sels[getPrimaryType(1)],action);
	manageWindow(sels[getPrimaryType(0)].attached,action);
	manageWindow(sels[getPrimaryType(1)].attached,action);
	}

void showFunctionMenu(int action) {
	int i,busy = 0;
	unsigned long flist = 0;
	selection *sel = currentWindowSelection();
	int selMap = getSelMap(sel);
	bool mounted = (sel->currentPath != NULL && *sel->currentPath)?1:0;
	char *tmp;
	bool isHTTP = (strncmp(globalPath,"http://",7))?false:true;
	if (!action) {
		mvaddch(LINES-1,0,' ');
		/*
		mvprintw(LINES-1,0," / / /";
		mvaddch(LINES-1,0,ACS_UARROW);
		mvaddch(LINES-1,2,ACS_DARROW);
		mvaddch(LINES-1,4,ACS_LARROW);
		mvaddch(LINES-1,6,ACS_RARROW);
		*/
		clrtoeol();

		} // for (i=0;i<COLS;i++) addch(' '); } // clear the menu line
//	if (options & OPT_RAMDISK) displayAvailableBuffer(); // show available space for selected mount point
	if (!mounted) {
		if (selMap & (SEL_IMAGE | SEL_SOURCE | SEL_TARGET)) {
			if (sel->count && sel->sources[sel->position].diskNum == -2 && !strcmp(sel->sources[sel->position].identifier,RAMFS)) {
				if (!stillBusy("/mnt/ram")) {
					showFunction(COLS-(54+9),"F6","ERASE",action);
					if (action == 6) { // erase contents of RAM disk by unmounting and mounting it
					        if (!promptYesNo("This will clear the RAM disk. Proceed [y/n]?",CLR_BRIGHTRED)) { allWindows(CLEAR); return; }
						allWindows(CLEAR);
						if (!unmountLocation("/mnt/ram")) { // unmounted
							if (mount("tmpfs","/mnt/ram","tmpfs",0,"size=75%")) debug(INFO, 1,"Unable to re-mount RAM disk.\n");
							}
						else debug(INFO, 1,"Unable to unmount RAM disk.\n");
						}
					}
				}
#ifdef NETWORK_ENABLED
			if (options & OPT_NETWORK) {
				showFunction(10,"F2","REMOTE",action);
				if (action == 2) { addNetworkPoint(sel); return; }
				if (mountStillUsed(sel,selMap)) { ; }
				if (sel->count && sel->sources[sel->position].diskNum == -2 && strcmp(sel->sources[sel->position].identifier,LOCALFS) && strcmp(sel->sources[sel->position].identifier,RAMFS)) {
					if (!stillBusy(sel->sources[sel->position].location)) {
						showFunction(COLS-(54+9),"F6","REMOVE",action);
						if (action == 6) {
							tmp = sel->sources[sel->position].location;
							if (strlen(tmp) > 7 && !strncmp(tmp,"http://",7)) { // remove HTTP mount point
								removeHTTPPoint(sel);
								i = sel->position;
								populateList(sel);
								if (i >= sel->count) i = sel->count-1; // in case more than one got removed
                                                                sel->position = i;
                                                                manageWindow(sel,CLEAR);
								debug(INFO, 5,"Remove HTTP mount point.\n");
								}
						 	else if (!unmountLocation(sel->sources[sel->position].location)) { // unmount CIFS entry
								i = sel->position;
								populateList(sel);
								if (i >= sel->count) i = sel->count-1; // in case more than one got removed
								sel->position = i;
								manageWindow(sel,CLEAR);
								debug(INFO, 5,"Removed CIFS mount point.\n");
								}  // unmount this mount, and re-load the drive selector
							}
						}
					}
				}
#endif
			// if (currentFocus == WIN_SOURCE && (mode == RESTORE || mode == TRANSFER)) showFunction(10,"F2","ADD SOURCE",action);
			// else if (currentFocus == WIN_TARGET && (mode == BACKUP || mode == TRANSFER)) showFunction(10,"F2","ADD TARGET",action);
			}
		}
	else if ((options & OPT_RAMDISK) && (selMap & SEL_IMAGE) && (mode == RESTORE) && strncmp(sel->currentPath,"/mnt/ram/",9) && sel->count && (sel->sources[sel->position].state == CLR_GB)) {
#ifdef NETWORK_ENABLED
		if (strncmp(sel->currentPath,"http://",7)) httpImageSize = imageSize;
		if (httpImageSize < getAvailableBuffer()) { // enough space in RAM disk to buffer this
#else
		if (imageSize < getAvailableBuffer()) {
#endif
			showFunction(COLS-(45+9),"F7","BUFFER",action);
			if (action == 7) {
				bool isAuto = (options & OPT_AUTO);
				options |= OPT_AUTO; // don't prompt when done
				prepareOperation("BUFFERING ARCHIVE TO RAM");
				locateImage();
				strcpy(globalPath2,"/mnt/ram/");
				if ((tmp = strrchr(globalPath,'/')) != NULL) strcpy(&globalPath2[9],&tmp[1]);
				else strcpy(&globalPath2[9],globalPath);
				debug(INFO, 1,"Copying %s to RAM buffer.\n",globalPath);
				copyImage("/mnt/ram",globalPath,globalPath2,false);
				if (options & OPT_APPEND) customFilesAppend(globalPath,globalPath2);
				if (!isAuto) { options &= ~OPT_AUTO; mvprintw(0,COLS-19,"    "); debug(INFO, 1,"Disabling auto mode after buffering.\n"); } // turn off auto-acknowledge
				// get out of this mount, and go into /mnt/ram after buffering
				sel->currentPath[sel->currentPathStart] = 0;
				navigateLeft(sel); // auto-unmounts the current mount
				sel->position = 0;
				for(i = 0;i<sel->count;i++) { // find RAM disk
					if (sel->sources[i].diskNum == -2 && !strcmp(sel->sources[i].identifier,RAMFS)) { sel->position = i; break; }  // find RAM Disk
					}
				if (sel->count && sel->sources[sel->position].diskNum == -2 && !strcmp(sel->sources[sel->position].identifier,RAMFS)) { // select the image we just copied
					navigateRight(sel);
					for (i=0;i<sel->count;i++) { // find image we just copied
						if (!strcmp(&globalPath2[9],sel->sources[i].location)) sel->position = i;
						}
					displaySelector(sel,1);
					populateImage(sel,0);
					manageWindow(sel->attached,CLEAR); manageWindow(sel,CLEAR);
					}
				showFunctionMenu(0);
				}
			}
		else if (action == 7) { options &= ~OPT_AUTO; debug(INFO, 1,"Disabling auto mode; insufficient RAM.\n"); } // turn off auto if we can't buffer it
		}
	if ((mode == BACKUP || mode == RESTORE) && selectedType == 2) {
//		if (!isHTTP) {
			showFunction(COLS-(18+10),"F10","VERIFY",action);
		// showFunction(COLS-(8+10),"F11","VERIFY",action);
			if (action == 10) {
				prepareOperation("VERIFYING ARCHIVE INTEGRITY");
				locateImage(); // set globalPath
				verifyImage(globalPath);
				}
//			}
		if (action == 98) {
			prepareOperation("VALIDATING ARCHIVE SIGNATURE");
			locateImage();
			progressBar(0,0,PROGRESS_INIT | 1); // no global values for this
#ifdef NETWORK_ENABLED
if (isHTTP) {
		i = getHTTPInfo(0); // return segment count after verifying files
		debug(INFO, 5,"Number of segments to verify: %i\n",i);
		}
else
#endif
			copyImage(NULL,globalPath,NULL,true);
			}
		}
	if (mode == BACKUP) {
		if (currentFocus == WIN_TARGET && mounted) {
#ifdef NETWORK_ENABLED
			if (strncmp(sel->currentPath,"http://",7)) // make sure it's not http://
#endif
			if ((*currentImage.imagePath && (!strcmp(currentImage.imagePath,sel->currentPath))) || ((sel->count < sel->size) || (!expandSelection(sel,0)))) (action == 6)?addImageName(sel,true):showFunction(COLS-(54+9),"F6","NEW",action);
                if (*currentImage.imagePath && (sel->sources[sel->position].state == CLR_RB)) {
                        (action == 7)?removeCurrent(sel):showFunction(COLS-(45+9),"F7","REMOVE",action);
                        }
                else { // delete a file/dir
                        if (!sel->count && (strlen(sel->currentPath) != sel->currentPathStart)) (action == 7)?removeDir(sel):showFunction(COLS-(45+9),"F7","RMDIR",action);
                        }
                }
		busy = checkBusyState();
		if (busy) {
			showFunction(0,"F1","CLEAR",action);
			if (selectedType == 1) {
				if (action == 2) {
					prepareOperation("CREATING DRIVE ARCHIVE");
					createBackup(0);
					}
				else showFunction(10,"F2","BACKUP",action);
				}
			}
		else if (sels[LIST_DRIVE].count) {
			showFunction(0,"F1","SELECT",action);
			}
		if (action == 1) {
			if (busy) clearBusy();
			else autoSelectDrive();
			displaySelector(sels[LIST_DRIVE].attached,1);
			fillAttached(&sels[LIST_DRIVE]);
			manageWindow(sels[LIST_DRIVE].attached,CLEAR); manageWindow(&sels[LIST_DRIVE],CLEAR);
			}
		}
	else if (busyMap) {
		showFunction(0,"F1","CLEAR",action);
		if (action == 1) { clearBusy(); fillAttached(&sels[LIST_DRIVE]); manageWindow(sels[LIST_DRIVE].attached,CLEAR); manageWindow(&sels[LIST_DRIVE],CLEAR); }
		if (action == 2) {
			if (promptYesNo("Proceed with restore [y/n]?",CLR_BRIGHTRED)) { // TODO: clear some stuff
			prepareOperation("RESTORING DRIVE ARCHIVE");
			performRestore();
			} else { allWindows(CLEAR); return ; }
			}
		else showFunction(10,"F2","RESTORE",action);
		}
	else if (mode == RESTORE && selectedType == 2) {
		showFunction(0,"F1",(currentFocus == WIN_SOURCE)?"AUTOSEL":"SELECT",action);
		if (action == 1) { // automatically select possible first matches
			autoSelectMap(0);
			if (busyMap && (currentFocus == WIN_SOURCE)) { currentFocus = WIN_TARGET;  manageWindow(&sels[getPrimaryType(WIN_SOURCE)],CLEAR); manageWindow(NULL,CLEAR); }
			}
		}
	if (options & OPT_PRE) { // mount/unmount before and after this is run into a special /mnt/{drive}-pre/ section
		showFunction(20,"F3","PRESCRIPT",action);
		}
	if (modeset && (options | (OPT_BACKUP | OPT_ERASE | OPT_TRANSFER))) showFunction(COLS-(37+8),"F8","MODE",action);
	if (shellset) showFunction(COLS-(28+9),"F9","SHELL",action);
//	if (expert) showFunction(60,"F10","BASIC",action);
//	else showFunction(60,"F10","DETAIL",action);
	if (action == 99) {
		expert=(expert)?false:true;
		if (!expert && currentFocus != WIN_TARGET && currentFocus != WIN_SOURCE) { currentFocus = WIN_TARGET; manageWindow(&sels[getPrimaryType(WIN_SOURCE)],CLEAR); manageWindow(NULL,CLEAR); }
		manageWindow(&sels[LIST_AUX1],CLEAR); manageWindow(&sels[LIST_AUX2],CLEAR);
		// showCurrentFocus();
		}
	if (expert) {
        	attron(COLOR_PAIR(CLR_RB) | A_BOLD);
		mvprintw(0,COLS-11,"EXPERT MODE");
        	attroff(COLOR_PAIR(CLR_RB)| A_BOLD);
		}
	else mvprintw(0,COLS-11,       "           ");
	if (options & OPT_AUTO) {
		attron(COLOR_PAIR(CLR_YB) | A_BOLD);
		mvprintw(0,COLS-19,"AUTO");
		attroff(COLOR_PAIR(CLR_YB) | A_BOLD);
		}
	else mvprintw(0,COLS-19,"    "); // turn off auto mode
	if (add_img && mode == RESTORE) {
		attron(A_BOLD);
		mvprintw(0,COLS-14,"AI");
		attroff(A_BOLD);
		}
	else mvprintw(0,COLS-14,"  ");
	if (action == 12) {
		if (!promptYesNo((options & OPT_HALT)?"Power off appliance [y/n]?":"Reboot appliance [y/n]?",CLR_BRIGHTRED)) { allWindows(CLEAR); return; }
#ifdef PRODUCTION
		reboot((options & OPT_HALT)?LINUX_REBOOT_CMD_POWER_OFF:LINUX_REBOOT_CMD_RESTART);
#endif
		endwin();
		exit(1);
		}
	// showFunction(COLS-(18+10),"F10","REBOOT",action);
	// showFunction(COLS-8,"F12","ABOUT",action);
	showFunction(COLS-9,"F12",(options & OPT_HALT)?"HALT  ":"REBOOT",action);
	showFunction(COLS-(8+10),"F11","ABOUT",action);
	}

/*
void showCurrentFocus(void) {
	char *annunciator = "status";
	switch(mode) {
		case BACKUP: if (currentFocus == WIN_SOURCE) annunciator = "drives";
			else if (currentFocus == WIN_TARGET) annunciator = "images";
			else if (currentFocus == WIN_AUX1) annunciator =   "dskaux";
			else if (currentFocus == WIN_AUX2) annunciator =   "imgaux";
			break;
		case RESTORE:
			if (currentFocus == WIN_SOURCE) annunciator = "images";
			else if (currentFocus == WIN_TARGET) annunciator = "drives";
			else if (currentFocus == WIN_AUX1) annunciator = "imgaux";
			else if (currentFocus == WIN_AUX2) annunciator = "dskaux";
			break;
		case ERASE:
			if (currentFocus == WIN_SOURCE) annunciator =      "eraser";
			else if (currentFocus == WIN_TARGET) annunciator = "drives";
			else if (currentFocus == WIN_AUX1) annunciator = "ersaux";
			else if (currentFocus == WIN_AUX2) annunciator = "dskaux";
			break;
		default: // transfer
			if (currentFocus == WIN_SOURCE) annunciator =      "source";
                        else if (currentFocus == WIN_TARGET) annunciator = "target";
                        else if (currentFocus == WIN_AUX1) annunciator = "srcaux";
                        else if (currentFocus == WIN_AUX2) annunciator = "trgaux";
			break;
		}
	mvprintw(0,0,annunciator);
	}
*/

/*----------------------------------------------------------------------------
**
*/
static void titleBar(void)
	{
	char titleBuf[128];
	const char* blank="                 ";

	sprintf(titleBuf,"  Micro Focus System %s %s  ",(mode == RESTORE)?"Restore":(mode == BACKUP)?"Backup":(mode == ERASE)?"Erase":"Transfer",VERSION);
	mvprintw(0,(COLS-strlen(titleBuf))/2,titleBuf);
	// showCurrentFocus();
	showFunctionMenu(0);

	return;
	}

int location(selection *sel) {
	int res = WIN_AUX2;
	if (sel == &sels[getPrimaryType(0)]) res= WIN_SOURCE;
	if (sel == &sels[getPrimaryType(1)]) res= WIN_TARGET;
	if (sel == sels[getPrimaryType(0)].attached) res= WIN_AUX1;
	return res;
	}

void modeMap(void) {
	sels[LIST_AUX1].currentPath = sels[LIST_AUX2].currentPath = NULL;
	switch(mode) {
		case RESTORE: case BACKUP: sels[LIST_IMAGE].attached->currentPath = " Part  Type    Size Image Label";
		case ERASE: sels[LIST_DRIVE].attached->currentPath = " Part  Type    Size Label";
		default: break;
		}
	}

char hasSelections(selection *sel) {
	int i;
	for (i=0;i<sel->count;i++) if (sel->sources[i].state & ST_SEL) {
		// mvprintw(0,0,"** %i %i [%c]**",i,sel->sources[i].state,(sel == &sels[LIST_AUX1])?'Y':'N');
		return 1;
		}
	return 0;
	}

void clearDisksetState(diskset *ds) {
	int i, j;
	disk *d;
	partition *p;
	for (i=0;i<ds->dcount;i++) {
		d = &ds->drive[i];
		d->state = 0;
		d->map = NULL;
		for (j=0;j<d->partCount; j++) {
			p = &d->parts[j];
			p->state = 0;
			p->map = NULL;
			}
		}
	}

void nextMode(void) {
        selection *sel;
	int n;
	if (hasSelections(&sels[LIST_AUX2])) { // applies only to drive list (ignoring for transfer mode)
		if (!promptYesNo("Switching mode will clear selection state. Proceed [y/n]?",CLR_BRIGHTRED)) { allWindows(CLEAR); return; }
		}
	busyMap = false;
	clearDisksetState(&dset);
	clearDisksetState(&iset);
	sels[LIST_AUX1].horiz = sels[LIST_AUX2].horiz = 0;
	do {
        	switch(mode) {
			case RESTORE: mode = BACKUP; break;
			case BACKUP: mode = ERASE; break;
			case ERASE: mode = TRANSFER; break;
			default: mode = RESTORE;
			}
		if ((mode == RESTORE) && (options & OPT_RESTORE)) break;
		else if ((mode == BACKUP) && (options & OPT_BACKUP)) break;
		else if ((mode == ERASE) && (options & OPT_ERASE)) break;
		else if ((mode == TRANSFER) && (options & OPT_TRANSFER)) break;
		} while(1);
	currentFocus = 0; // switch cursor to top
/*
        sel = currentWindowSelection();
        if (currentFocus > WIN_TARGET && sel->count <= 1) {
                currentFocus = (currentFocus == WIN_AUX2)?WIN_AUX1:WIN_AUX2;
                sel = currentWindowSelection();
                if (sel->count <= 1) currentFocus = WIN_SOURCE;
                }
*/
	modeMap();
        titleBar();
        manageWindow(&sels[getPrimaryType(0)],INIT);
        manageWindow(&sels[getPrimaryType(1)],INIT);

        // allWindows(CLEAR); // CLEAR
        doupdate();
        // refresh();
        }

#define COLOR_CUSTOM1  COLOR_CYAN		// 8
#define COLOR_CUSTOM2  COLOR_MAGENTA		// 9
#define COLOR_CUSTOM3  10
#define COLOR_CUSTOM4  11
#define COLOR_CUSTOM5  12
#define COLOR_CUSTOM6  13

/*----------------------------------------------------------------------------
**
*/
static void initializeColors(void)
	{
  start_color();
	debug(INFO, 5,"Color support: %s (re-assignment %spermitted)\n",(has_colors())?"yes":"no",(can_change_color())?"":"not ");
	if(can_change_color())
		{
		init_color(COLOR_CUSTOM1,330,330,330); // unselected/unavailable dim
		init_pair(CLR_WDIM,COLOR_CUSTOM1,COLOR_BLACK);
		init_color(COLOR_CUSTOM2,150,150,150); // frame dim
		init_pair(CLR_BDIM,COLOR_CUSTOM2,COLOR_BLACK);

		init_color(COLOR_RED,500,100,100);
		init_color(COLOR_GREEN,100,700,700); // aqua
		init_color(COLOR_YELLOW,700,700,100);
		init_color(COLOR_CUSTOM3,1000,0,0); // bright red
		init_color(COLOR_CUSTOM4,0,1000,0); // bright green
		init_color(COLOR_CUSTOM5,100,50,50); // progress dim
		init_color(COLOR_CUSTOM6,500,500,1000); // progress blue
		}
	else
		{ // shouldn't happen, but add it here anyway
		init_pair(CLR_WDIM,COLOR_BLACK,COLOR_BLACK);
		init_pair(CLR_BDIM,COLOR_YELLOW,COLOR_BLACK);
		}

	init_pair(0,COLOR_WHITE,COLOR_BLACK); // default color
	init_pair(CLR_RB,COLOR_RED,COLOR_BLACK);
	init_pair(CLR_YB,COLOR_YELLOW,COLOR_BLACK);
	init_pair(CLR_BLB,COLOR_BLUE,COLOR_BLACK);
	init_pair(CLR_GB,COLOR_GREEN,COLOR_BLACK);
	if(COLORS < 16)
		{
		init_pair(CLR_BRIGHTRED,COLOR_RED,COLOR_BLACK);
		init_pair(CLR_BRIGHTGREEN,COLOR_GREEN,COLOR_BLACK);
		init_pair(CLR_PROGRESSDIM,COLOR_YELLOW,COLOR_BLACK);
		init_pair(CLR_PROGRESS,COLOR_BLUE,COLOR_WHITE);
		}
	else
		{
		init_pair(CLR_BRIGHTRED,COLOR_CUSTOM3,COLOR_BLACK);
		init_pair(CLR_BRIGHTGREEN,COLOR_CUSTOM4,COLOR_BLACK);
		init_pair(CLR_PROGRESSDIM,COLOR_CUSTOM5,COLOR_WHITE);
		init_pair(CLR_PROGRESS,COLOR_CUSTOM6,COLOR_BLACK);
		}
	}

int getSelMap(selection *sel) {
        if (sel == &sels[LIST_IMAGE]) return SEL_IMAGE;
        if (sel == &sels[LIST_SOURCE]) return SEL_SOURCE;
        if (sel == &sels[LIST_TARGET]) return SEL_TARGET;
	if (sel == &sels[LIST_DRIVE]) return SEL_DRIVE;
	if (sel == &sels[LIST_ERASE]) return SEL_ERASE;
	if (sel == &sels[LIST_AUX1]) {
		switch(mode) {
			case RESTORE: return SEL_IMAGE_AUX;
			case BACKUP: return SEL_DRIVE_AUX;
			case ERASE: return SEL_ERASE_AUX;
			default: return SEL_SOURCE_AUX;
			}
		}
	else { // aux2
		switch(mode) {
			case RESTORE: return SEL_DRIVE_AUX;
			case BACKUP: return SEL_IMAGE_AUX;
			case ERASE: return SEL_DRIVE_AUX;
			default: return SEL_TARGET_AUX;
			}
		}
        }

void getPoolSize(int i) {
	selection *sel = &sels[i];
	poolset *ps = &(sel->pool);
	int pcount = 0;
	strpool *pool = ps->start;
	char *title;
	int size = sel->size * sizeof(item);
	while(pool != NULL) {
		if (pool->next == NULL) pcount += ps->index;
		else pcount += ps->size;
		pool = pool->next;
		}
	if (i < SELMAX) {
		switch(i) {
			case LIST_IMAGE: title = "IMAGE"; break;
			case LIST_DRIVE: title = "DRIVE"; break;
			case LIST_ERASE: title = "ERASE"; break;
			case LIST_SOURCE: title = "SOURCE"; break;
			default: title = "TARGET"; break;
			}
		if (i < 2) {
			printf("%s: %i [%i] /",title,pcount,size);
			getPoolSize(SELMAX+i);
			printf("\n");
			}
		else printf("%s: %i [%i]\n",title,pcount,size);
		}
	else printf(" %i (%i) ",pcount,size);
	}

void getDiskSize(char *name, diskset *ds) {
	poolset *ps = &(ds->pool);
	int pcount = 0;
	int i, j=0;
	strpool *pool = ps->start;
	while(pool != NULL) {
		if (pool->next == NULL) pcount += ps->index;
		else pcount += ps->size;
		pool = pool->next;
		}
	for (i=0;i<ds->size;i++) j += ds->drive[i].psize * sizeof(partition);
	j += ds->size * sizeof(disk);
	printf("Disk %s: %i [%i]\n",name,j,pcount);
	}

/*----------------------------------------------------------------------------
** automatically mounts image directory if it's the only option available
*/
void autoMount(void)
	{
	if(!(options & OPT_NETWORK) && (options & (OPT_BACKUP | OPT_RESTORE)) && (sels[LIST_IMAGE].count == 1) && (!sels[LIST_IMAGE].currentPathStart))
		{
		sels[LIST_IMAGE].position = 0;
		navigateRight(&sels[LIST_IMAGE]);
		}
	}

void debugCurrentDrive(void) {
	selection *sel = &sels[LIST_DRIVE];
	disk *d = sel->sources[sel->position].location;
	printf("\033[31mDisk is: %s\033[0m\n",d->deviceName);
	driveComplete(d);
	printf("\033[31mEND\033[0m\n");
	}

/*----------------------------------------------------------------------------
**
*/
void navigator(void)
	{
	bool toosmall = false;
	int i, ch, autofocus;
	int showmode = 0;
	selection *sel;
	item *itm;
	int selMap;
	bool tmpAuto;

	for(ch=0;ch < WINMAX ; ch++)
		wins[ch] = NULL; // reset

	for(ch=0;ch < SELMAX+2; ch++)
		sels[ch].horiz = 0; // reset

	initscr();							/* Start curses mode 		*/
	cbreak();								/* Line buffering disabled, pass everything */
	noecho();								/* I'll echo my own getch() characters if you don't mind. */
	keypad(stdscr, TRUE);		/* I need that nifty F1 	*/
	initializeColors();
	curs_set(0);						/* Cursor off (invisible) */

	/* attach *image1, *image2 accordingly, and navigate through */
	autoMount();
	allWindows(INIT);
	setup = 0;
	titleBar();
	doupdate();							/* Update (refresh) screen (curses.h)  */
	// refresh();

	if(currentFocus > WIN_TARGET)
		{ // see if this is a valid focus and call execute function (execute functions imply WIN_AUX{1,2}
		if(currentWindowSelection()->count < 1)
			currentFocus = 0;
		else
			switch(mode)
				{
				case RESTORE: break;
				case BACKUP: break; // showFunctionMenu(6); break;
				case ERASE: break;
				case TRANSFER: break;
				}
		}

	sel = currentWindowSelection();

	/* AUTO MODES */

	if(options & OPT_BUFFER)
		{  // mode = RESTORE by definition
//ABJ TODO
		currentFocus = WIN_SOURCE;
		if (checkBusyMap()) { clearBusy(); fillAttached(&sels[LIST_DRIVE]); manageWindow(sels[LIST_DRIVE].attached,CLEAR); manageWindow(&sels[LIST_DRIVE],CLEAR); }
		allWindows(CLEAR);
		showFunctionMenu(0);
		doupdate();
		showFunctionMenu(7); // move file to RAM buffer
		setup = 1;
		toggleDrives();
		if (!checkBusyMap()) setup = 0;
		populateImage(&sels[LIST_IMAGE],0); // need this again after toggleDrives()
		fillAttached(&sels[LIST_DRIVE]); // make sure the selected drive is properly populated
		if (busyMap) currentFocus = WIN_TARGET; // second window on restore mode
		sel = currentWindowSelection();
		allWindows(CLEAR);
		setup = 0;
		showFunctionMenu(0);
		doupdate();
		options &= ~OPT_BUFFER; // turn this off so we can auto-exit OPT_AUTO as needed
		}

	// while((ch = getch()) != KEY_F(1)) { // F1 also == NUMLOCK
	if (options & OPT_AUTO) {
		debug(INFO, 1,"Auto mode enabled\n");
		// if (mode == RESTORE && !busyMap && !checkBusyState()) showFunctionMenu(1); // only auto-select drives in restore mode
		if (performVerify || performValidate) {
			if (!((mode == BACKUP || mode == RESTORE) && selectedType == 2)) {
				debug(INFO, 1,"Disabling auto mode; no valid archive selected.\n");
				options &= ~OPT_AUTO; // no valid file selected
				}
			}
		else { // see if something is actionable (basically, if F2 is highlighted)
			if (mode == BACKUP) {
				if (!checkBusyState() || selectedType != 1) { debug(INFO, 1,"Disabling auto mode; no valid backup available.\n"); options &= ~OPT_AUTO; }
				}
			else if (!busyMap) { debug(INFO, 1,"Disabling auto mode; no valid restore available.\n"); options &= ~OPT_AUTO; }
			// if (mvgetch(LINES-1,11) != '2') { // make sure F2 is displayed
			}
		if (options & OPT_AUTO && options & OPT_PRE) { options &= ~OPT_PRE; }
		if (options & OPT_AUTO) {
			options &= ~OPT_PRE;
			debug(INFO, 1,"Performing primary function in auto mode.\n");
			showFunctionMenu((performVerify)?10:(performValidate)?98:2); // F2, F10, or ctrl-V
			}
		if (options & OPT_AUTO && options & OPT_POST) { options &= ~OPT_POST; } // could do a --post bit here
		if (options & OPT_AUTO) {
			if (options & (OPT_REBOOT | OPT_HALT)) {
#ifdef PRODUCTION
				reboot((options & OPT_HALT)?LINUX_REBOOT_CMD_POWER_OFF:LINUX_REBOOT_CMD_RESTART);
#endif
				endwin();
				printf((options & OPT_HALT)?"Powering down.\n":"Rebooting.\n");
				exit(0);
				}
			// REBOOT/HALT here in auto mode
			options &= ~(OPT_POST | OPT_AUTO);
			}
		mvprintw(0,COLS-19,"    "); // out of auto mode
		}
	else if (performVerify) showFunctionMenu(10);
	else if (performValidate) showFunctionMenu(98);

	/* END OF AUTO MODES */

	while(1) {  // use [F1]shell to exit, or ctrl-backslash
		ch = getch(); // getNextAuto();
		sel = currentWindowSelection();
		selMap = getSelMap(sel);

		if (toosmall && ch != KEY_RESIZE) continue;
		// mvprintw(0,5,"%i:%i:[%i]  ",currentFocus,mode,ch);
		if (showmode &&  ((!modeset && showmode < 5) || (!shellset))) {
			switch(showmode) {
				case 1: showmode = (ch == 'm' || ch == 'M')?2:0; break;
				case 2: showmode = (ch == 'o' || ch == 'O')?3:0; break;
				case 3: showmode = (ch == 'd' || ch == 'D')?4:0; break;
				case 4: if (ch == 'e' || ch == 'E') {
					modeset = 1;
					showFunctionMenu(0);
					}
					showmode = 0;
					break;
				case 5: showmode = (ch == 's' || ch == 'S')?6:0; break;
				case 6: showmode = (ch == 'h' || ch == 'H')?7:0; break;
				case 7: showmode = (ch == 'e' || ch == 'E')?8:0; break;
				case 8: showmode = (ch == 'l' || ch == 'L')?9:0; break;
				case 9: if (ch == 'l' || ch == 'L') {
					shellset = 1;
					showFunctionMenu(0);
					}
					showmode = 0;
					break;
				default: showmode = 0; break;
				}
			}

		switch(ch)
		{
			case KEY_LEFT:
				if ((selMap & SEL_MOUNTMASK) && *sel->currentPath) navigateLeft(sel);
				// also remount if there are no drives
				if ((selMap & (SEL_IMAGE_AUX | SEL_DRIVE_AUX)) && mode == RESTORE) positionDrive(sel,0);
				if ((selMap & SEL_DRIVE) && mode == RESTORE && selectedType == 2) autoSelectMap(1);
				break;
			case KEY_RIGHT:
				if ((selMap & SEL_MOUNTMASK) && sel->position >= 0 && sel->count > 0) navigateRight(sel);
				if ((selMap & (SEL_IMAGE_AUX | SEL_DRIVE_AUX)) && mode == RESTORE) positionDrive(sel,1);
				if ((selMap & SEL_DRIVE) && mode == RESTORE && selectedType == 2) autoSelectMap(2);
				break;
			case KEY_UP: manageWindow(NULL,UP); doupdate(); break;
			case KEY_NPAGE: manageWindow(NULL,PGDOWN); doupdate(); break;
			case KEY_PPAGE: manageWindow(NULL,PGUP); doupdate(); break;
			case KEY_DOWN: manageWindow(NULL,DOWN); doupdate(); break;
			case KEY_BTAB:
				autofocus = currentFocus;
				do {
					if (!currentFocus) currentFocus = (expert)?3:1; else currentFocus--;
					if (busyMap && !currentFocus) currentFocus = (expert)?3:1; // skip restore window
					else if (!currentFocus && mode == RESTORE) clearBusy(); // just in case
					sel = currentWindowSelection();
					if ((currentFocus <= WIN_TARGET) || (sel->count >= 1)) break; // (sels[currentFocus-2].attached->count > 1)) break;
				} while(autofocus != currentFocus);
				allWindows(CLEAR);
				showFunctionMenu(0);
				// showCurrentFocus();
				doupdate();
			        sel = currentWindowSelection();
        			selMap = getSelMap(sel);
				// refresh();
				break;
			case '\t':
				autofocus = currentFocus;
				do {
					currentFocus++;
					if (!expert && currentFocus > 1) currentFocus = 0;
					else if (expert && currentFocus >= 4) currentFocus = 0;

					if (busyMap && !currentFocus) currentFocus = 1;
					else if (!currentFocus && mode == RESTORE) clearBusy(); // just in case, clear any pre-existing maps
					sel = currentWindowSelection();
					if ((currentFocus <= WIN_TARGET) || (sel->count >= 1)) break;
				} while(autofocus != currentFocus);

				allWindows(CLEAR);
				showFunctionMenu(0);
				// showCurrentFocus();
				doupdate();
			        sel = currentWindowSelection();
        			selMap = getSelMap(sel);
				// refresh();
				break;
			case KEY_RESIZE:
				wclear(stdscr);
				if (COLS < 80 || LINES < 24) { refresh(); toosmall = true; mvprintw(0,0,"Window size too small.");
; continue; }
				toosmall=false;
				for (ch=0;ch < WINMAX ; ch++) { delwin(wins[ch]); wins[ch] = NULL; } // wins[ch] = NULL;
				for (ch=0;ch < SELMAX+2; ch++) sels[ch].horiz = 0;
		 		titleBar();
				allWindows(CLEAR);
				doupdate();
				// refresh();
				curs_set(0);
				break;
			case 18: rescanDrives(selMap); break; // hidden feature; re-scans drives (CTRL-R)
			case 5: showFunctionMenu(99); break; // ctrl-E, expert mode
			case 22: showFunctionMenu(98); break; // ctrl-V, sha1sum validate
			case KEY_F(9): // shell
				if (shellset) {
					clear();
					refresh();
					endwin();
					// const char *envp[] = { "TERM=linux", "HOME=/tmp", NULL };
					unmountExit(-1); // clean up mounts
					SYSRES_LINUX_SetKernelLogging(SYSRES_LINUX_KERN_LOGMODE_EXIT_D);
					if (getppid() == 1) execve("/bin/bash",NULL,envp);
					exit(0);
					}
				else showmode = 5;
				break;
			case KEY_F(6): showFunctionMenu(6); break; // new/cancel
			case KEY_F(1): case 1: showFunctionMenu(1); break; // autosel/select/clear (ctrl-A)
			case KEY_F(2): case 2: showFunctionMenu(2); break; // backup/restore (ctrl-B)
			case KEY_F(7): showFunctionMenu(7); break; // rmdir, remove pending archive, buffer archive
			case KEY_F(8):
				if (modeset) { nextMode(); sel = currentWindowSelection(); selMap = getSelMap(sel); }
				else showmode = 1;
				break;
			case KEY_F(10): showFunctionMenu(10); break; // verify
			case KEY_F(12): showFunctionMenu(12); break; // reboot
			case 16: if (mode == RESTORE) { add_img = (add_img)?0:1; showFunctionMenu(0); } break;  // ctrl-P for --addimg annunciator
#ifdef NETWORK_ENABLED
			case 14: // ctrl-n
				networkSettings();
				allWindows(CLEAR);
				doupdate();
				// refresh();
				break;
#endif
			case ' ': // toggle a selector, as appropriate
				// mvprintw(0,5,"%i:%i:[%i]",currentFocus,mode,ch);
				switch(mode) {
					case TRANSFER: break; // (currentFocus == WIN_SOURCE || currentFocus == WIN_TARGET)
					case BACKUP:
						if (currentFocus == WIN_AUX1) { toggleBackup(sel,0,0);  manageWindow(&sels[LIST_DRIVE],REFRESH); }
						else if (currentFocus == WIN_SOURCE) { toggleBackup(sel,0,0); manageWindow(sels[LIST_DRIVE].attached,REFRESH); }
						break;
					case RESTORE: case ERASE:
						if (currentFocus == WIN_AUX2 && mode == ERASE) toggleBackup(sel,0,1);
						else if (currentFocus == WIN_AUX2 || currentFocus == WIN_AUX1) toggleMap(validMap,1);
						else if (currentFocus == WIN_TARGET) { toggleBackup(sel,0,0); manageWindow(sels[LIST_DRIVE].attached,REFRESH); }
						break;
					default: break;
					}
				break;
			case 0: // ctrl-space
				switch(mode) {
                                        case BACKUP:
                                                if (currentFocus == WIN_AUX1) { toggleBackup(sel,1,0);  manageWindow(&sels[LIST_DRIVE],REFRESH); }
                                                else if (currentFocus == WIN_SOURCE) { toggleBackup(sel,1,0); manageWindow(NULL,INIT); }
                                                break;
                                        case RESTORE: case ERASE:
						if (currentFocus == WIN_AUX2 && mode == ERASE) toggleBackup(sel,1,1);
                                                else if (currentFocus == WIN_TARGET) { toggleBackup(sel,1,0); manageWindow(NULL,INIT); }
                                                break;
                                        default: break;
                                        }
                                break;
			case KEY_F(11):  // about
				wclear(stdscr);
				showFiles();
				wclear(stdscr);
				refresh();
				allWindows(CLEAR);
				titleBar();

				doupdate();
/*
				endwin();
				printf("\033[;f\033[2J");
				license(false);
				printf("\n\033[32mPress 'return' to return to SysRes.\033[0m\n");
				getch();
				noecho();
				curs_set(0);
				refresh();
*/
				break;
/*
			case 'z':  // access/show shell
				endwin();
execve("/bin/bash",NULL,NULL);
exit(0);
				system("bash");

			// 	system("sh");
		//		debugDisks(0);
		//		debugCurrentDrive();
				printf("=============================\nMemory usage [%i allocations]: %li\n",mallocCount,totalMem);
				for(i=0;i<SELMAX;i++) getPoolSize(i);
				getDiskSize("dset",&dset);
				getDiskSize("iset",&iset);
				printf("=============================\n");
				printf("Press 'return' to return to SysRes.\n");
				getch();
				noecho();
				curs_set(0);
				refresh();
				break;
*/
		}
	}

	endwin();			/* End curses mode		  */
}
