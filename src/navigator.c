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

#include <dirent.h>
#include <fcntl.h>
#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "frame.h"			// expert
#include "mount.h"			// trackMount
#include "partition.h"
#include "window.h"			// options, mode

extern char localmount;
extern char pbuf[];

char *getStringPtr(poolset *sel, char *str);

int itemCompare(const void *a, const void *b) {
	const item *ia = (const item *)a;
	const item *ib = (const item *)b;
	return (strcmp(ia->location,ib->location));
	}

int matchSelection(selection *sel, char *string) {
	int i;
	for(i=0;i<sel->count;i++) {
        	if (!strcmp(string,sel->sources[i].location)) return i;
        	}
	return -1;
	}

unsigned char navigateDirectory(char *dirname, selection *sel) {
	int i, n, dircount = 0;
	char tmp[MAX_FILE];
	if (sel == NULL) return 0;
	char *path = sel->currentPath;
	int fps = sel->currentPathStart;
	char imageDir = (mode == TRANSFER)?0:1; // only look for image files and directories
	if (dirname == NULL || !strlen(dirname)) return;
	if (strlen(dirname)+strlen(path)+2 + MAX_FILE > MAX_PATH) return; // too large; don't continue
	if (!strcmp(dirname,"..")) {
		for(i=strlen(path)-2;i>fps;i--) {
			if (path[i] == '/') break; // { path[i] = 0; break; }
			}
		if (i < fps) return 0; // can't go further
		if (path[i] == '/') strcpy(tmp,&path[i+1]);
		else strcpy(tmp,&path[i]);
		tmp[strlen(tmp)-1] = 0;
		path[i] = 0;
		}
	else { i=strlen(path); strcat(path,dirname); tmp[0] = 0; }
#ifdef NETWORK_ENABLED
	if (!strncmp(path,"http://",7)) {
		if (*path && path[strlen(path)-1] != '/') strcat(path,"/");
		i = strlen(path);
		if (!fps) sel->currentPathStart = i;
		populateHTTPDirectory(sel);
//		debug(INFO,5,"WEB ACCESS: %s\n",path); // expects directory
		return 1;
		}
#endif

	struct dirent *dent;
	DIR *dir = opendir(path);
	if (dir == NULL) { path[i] = (strcmp(dirname,".."))?0:'/'; return 1; } // not a directory, or can't be read
	struct stat64 stats;
	resetPool(&sel->pool,0);
	sel->count = sel->top = sel->position = 0;
	if (*path && path[strlen(path)-1] != '/') strcat(path,"/");
	i = strlen(path);
	if (!fps) sel->currentPathStart = i;
	/* pre-read directory to get an idea of the expected *item size to prevent excessive realloc calls */
	while((dent = readdir(dir)) != NULL) dircount++;
	if (sel->count + dircount >= sel->size) expandSelection(sel,dircount); // pre-allocate full set
	rewinddir(dir);
	/* read and populate actual directory selector */
	while((dent = readdir(dir)) != NULL) {
		if (!strcmp(dent->d_name,".") || !strcmp(dent->d_name,"..")) continue;
		if (sel->count >= sel->size && expandSelection(sel,0)) break; // too many files
		if (strlen(dent->d_name) + 1 >= MAX_FILE) continue; // filename too large
		sel->sources[sel->count].location = getStringPtr(&sel->pool,dent->d_name);
		sel->sources[sel->count].state = 0; // set it to 0 for now
		strcat(path,dent->d_name); // make full path
		if (stat64(path,&stats)) { path[i] = 0; continue; } // can't stat this entry
		if (stats.st_mode & S_IFDIR) {
			if (imageDir && !strcmp(dent->d_name,"lost+found")) { path[i] = 0; continue; }
			sel->sources[sel->count].identifier = NULL; // directory
			}
		else if (!imageDir) sel->sources[sel->count].identifier = getStringPtr(&sel->pool,dent->d_name); // transfer mode
		else { // dim for now
			n=(stats.st_mode & S_IFREG && stats.st_size > 11)?getType(path):-1;
			sel->sources[sel->count].identifier = "";
                        sel->sources[sel->count].state = CLR_WDIM;

			if (!n) { path[i] = 0; continue; } // skip this
			else if (n > 0 && (getImageTitle(path,pbuf,NULL))) { // has an image header
				sel->sources[sel->count].identifier = getStringPtr(&sel->pool,pbuf);
				sel->sources[sel->count].state = CLR_GB;
				}
			}
		sel->count++;
		path[i] = 0; // revert to currentPath
		}
	closedir(dir);
	if ((imageDir) && (strcmp(path,currentImage.imagePath) == 0)) { // this directory has a working image
		if ((matchSelection(sel,currentImage.imageName) != -1) || ((sel->count >= sel->size) && (expandSelection(sel,0)))) *currentImage.imagePath = 0;
		else { // add it, and match to it
			addSelection(sel,currentImage.imageName,currentImage.imageTitle,1);
			sel->sources[sel->count-1].state = CLR_RB;
			if (strcmp(dirname,"..")) strcpy(tmp,currentImage.imageName); // we didn't back into it, so go to the working image by default
			}
		}
	qsort(sel->sources,sel->count,sizeof(item),itemCompare);
	if (*tmp) {
		sel->position = matchSelection(sel,tmp);
		if (sel->position == -1) sel->position = 0;
		}
	return 1;
	}

int mountStillUsed(selection *sel, char selMap) {
	char token = trackMount & selMap; // did we mount this originally?
	trackMount &= ~selMap; // clear trackMount for this entry
	int len = strlen(sel->currentPath);
	int mcount = 0;
	if (!strncmp(sels[LIST_IMAGE].currentPath,sel->currentPath,len) && sels[LIST_IMAGE].currentPath[len] == '/') { mcount++; if (token) trackMount |= 1; }
	if (!strncmp(sels[LIST_SOURCE].currentPath,sel->currentPath,len) && sels[LIST_SOURCE].currentPath[len] == '/') { if (!mcount && token) trackMount |= 2; mcount++; }
	if (!strncmp(sels[LIST_TARGET].currentPath,sel->currentPath,len) && sels[LIST_TARGET].currentPath[len] == '/') { if (!mcount && token) trackMount |= 4; mcount++; }
	if (!mcount && !token) return 1; // something else mounted this; consider it still mounted
	return mcount;
	}

int stillBusy(char *mountPoint) {
	int len = strlen(mountPoint);
	if (!strncmp(sels[LIST_IMAGE].currentPath,mountPoint,len) && sels[LIST_IMAGE].currentPath[len] == '/') return 1;
	if (!strncmp(sels[LIST_SOURCE].currentPath,mountPoint,len) && sels[LIST_SOURCE].currentPath[len] == '/') return 1;
	if (!strncmp(sels[LIST_TARGET].currentPath,mountPoint,len) && sels[LIST_TARGET].currentPath[len] == '/') return 1;
	return 0;
	}

void showReloadWindow(char *msg, int color) {
	int slength;
	static WINDOW *win;
	if (msg != NULL) {
		slength = strlen(msg);
		win = newwin(3,slength+6,LINES/2-2,COLS/2-slength/2-3);
		box(win,0,0);
       		wattron(win,COLOR_PAIR(color) | A_REVERSE);
		mvwprintw(win,1,2," %s ",msg);
       		wattroff(win,COLOR_PAIR(color) | A_REVERSE);
		}
	else werase(win);
	wrefresh(win);
	if (msg == NULL) delwin(win);
	}

char promptYesNo(char *msg, int color) {
	int n;
	showReloadWindow(msg,color);
	while(1) {
		if (options & OPT_AUTO) n = 'y'; else n = getch();
                if (n == 'Y') n = 'y';
                if (n == 'N') n = 'n';
                if (n == 'y' || n == 'n') break;
                }
	showReloadWindow(NULL,0);
	return (n == 'y')?1:0;
	}

void rescanDrives(int selMap) {
	selection *s1, *s2;
	int i;
	if (!promptYesNo("Scanning drives will reset everything. Proceed [y/n]?",CLR_BRIGHTRED)) { allWindows(CLEAR); return; }
	unmountExit(-1);
	switch(selMap) { // order doesn't matter
		case SEL_IMAGE: s1=&sels[LIST_IMAGE]; s2=&sels[LIST_DRIVE]; break;
		case SEL_SOURCE: case SEL_TARGET: s1=&sels[LIST_SOURCE]; s2=&sels[LIST_TARGET]; break;
		default: // SEL_DRIVE
			s1 = &sels[LIST_DRIVE];
			s2 = (mode == ERASE)?&sels[LIST_ERASE]:&sels[LIST_IMAGE];
			break;
		}
	manageWindow(s1,BLANK);
	manageWindow(s2,BLANK);
	manageWindow(s1->attached,BLANK);
	manageWindow(s2->attached,BLANK);
	// wrefresh(wins[location(sel)]); // ????
	showReloadWindow("Scanning drives",CLR_BRIGHTGREEN);
        scanDrives(&dset);
        showReloadWindow(NULL,0);
        sels[LIST_IMAGE].currentPathStart = sels[LIST_SOURCE].currentPathStart = sels[LIST_TARGET].currentPathStart = 0; // reset all windows
	*sels[LIST_IMAGE].currentPath = *sels[LIST_SOURCE].currentPath = *sels[LIST_TARGET].currentPath = 0;
        populateList(NULL); // refresh all lists, just in case drives changed (should not have changed on any mounts, otherwise those will be unreadable anyway)
        for (i=0;i<sels[LIST_DRIVE].count;i++) sels[LIST_DRIVE].sources[i].state = 0; // clear disk selection states
	autoMount(); // remount as needed
	allWindows(INIT);
	refresh();
	}

void navigateLeft(selection *sel) {
	int i;
	int selMap = getSelMap(sel);
	if (!navigateDirectory("..",sel)) { // we're at the beginning; unmount and re-list drives or don't continue if only one
		char *path = sel->currentPath;
		if (path[strlen(path)-1] == '/') path[strlen(path)-1] = 0;
		if ((options & OPT_RAMDISK) && !strcmp(path,"/mnt/ram")) strcpy(path,"/ram");
		else {
			if (!strncmp(path,"/mnt/",5)) {
				if (!(selMap & localmount) && (!mountStillUsed(sel,selMap))) { // only unmount if IMAGE or SOURCE/TARGET aren't also using it
					unmountLocation(path);
					// mountStillused() will have cleared the trackMount
					}
				// else localmount ^= selMap;
				if (!(selMap & localmount)) strncpy(path,"/dev/",5);
				currentImage.mount = NULL;
				}
			// otherwise, it was a net or local directory; we'll not unmount that
			}
		sel->currentPathStart = 0; // allows for color-coding mount points
		populateList(sel);
		sel->position = -1;
		if (!*path) strcpy(path,"/");
		if (*path) { // place position at previous entry directory/mount
			for(i=0;i<sel->count;i++) {
				if (selMap & localmount) {
					if (!strcmp(LOCALFS,sel->sources[i].identifier)) { sel->position = i; break; }
					if (!strcmp(path,sel->sources[i].location)) { sel->position = i; break; }
					}
				else if (!strcmp(path,(sel->sources[i].diskNum == -2)?sel->sources[i].location:(sel->sources[i].diskNum == -1)?((disk *)sel->sources[i].location)->deviceName:((partition *)sel->sources[i].location)->deviceName)) { sel->position = i; break; }
				}
			}
		if (sel->position == -1) sel->position = 0;
		localmount &= ~selMap;
		*path = 0;
		if ((sel->count == 1) && !(options & OPT_NETWORK) && (selMap == SEL_IMAGE)) mountLocation(sel,0,0); // automatically mount
		showFunctionMenu(0);
		if (mode == TRANSFER) { // also refresh opposing window to reflect new mount color
			if (sel == &sels[LIST_SOURCE]) { manageWindow(&sels[LIST_TARGET],INIT); wrefresh(wins[WIN_TARGET]); }
			else { manageWindow(&sels[LIST_SOURCE],INIT); wrefresh(wins[WIN_SOURCE]); }
			}
		else {
			manageWindow(&sels[LIST_DRIVE],INIT);
			wrefresh(wins[(mode == BACKUP)?WIN_SOURCE:WIN_TARGET]);
			displaySelector(sels[LIST_DRIVE].attached,1);
			if (expert) {
				if (mode == RESTORE) wrefresh(wins[WIN_AUX2]);
				else wrefresh(wins[WIN_AUX1]);
				}
			if (mode == BACKUP) manageWindow(&sels[LIST_DRIVE],REFRESH);
			}
		manageWindow(NULL,INIT);
		wrefresh(wins[location(sel)]);
		}
	else {
		manageWindow(NULL,INIT);
		wrefresh(wins[location(sel)]);
		}
	}

void navigateRight(selection *sel) {
	item *itm = &(sel->sources[sel->position]);
        if (itm->identifier == NULL) { // it's a directory
        	navigateDirectory(itm->location,sel);
		manageWindow(NULL,INIT);
		wrefresh(wins[location(sel)]);
		}
	else if (sel->currentPathStart) return; // it's a file
	else { // it's a mount point
		mountLocation(sel,0,0);
		showFunctionMenu(0);
		manageWindow(NULL,INIT);
		wrefresh(wins[location(sel)]);
		if (mode == TRANSFER) { // also refresh opposing window to reflect new mount color
			if (sel == &sels[LIST_SOURCE]) { manageWindow(&sels[LIST_TARGET],INIT); wrefresh(wins[WIN_TARGET]); }
			else { manageWindow(&sels[LIST_SOURCE],INIT); wrefresh(wins[WIN_SOURCE]); }
			}
		else {
			manageWindow(&sels[LIST_DRIVE],INIT); wrefresh(wins[(mode == BACKUP)?WIN_SOURCE:WIN_TARGET]);  // determine color changes in LIST_DRIVE window
			displaySelector(sels[LIST_DRIVE].attached,1);
			if (expert) {
				if (mode == RESTORE) wrefresh(wins[WIN_AUX2]);
				else wrefresh(wins[WIN_AUX1]);
				}
			if (mode == BACKUP) manageWindow(&sels[LIST_DRIVE],REFRESH);
			}
		}
	}
