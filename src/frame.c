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

#include <ncurses.h>
#include <string.h>
#include "image.h" 			// iset
#include "mount.h"			// globalBuf
#include "partition.h"
#include "position.h"		// busyMap
#include "window.h"			// mode, currentFocus

char pathBuf[LINELIMIT];

extern bool validMap;

bool expert = false;


void manageWindow(selection *sel, int action);
bool checkMapping(bool verify, bool ui);

void setAttr(WINDOW *win, int winspec, int set1, int set2) {
	static int last;
	if (winspec != -1) {
		last = (currentFocus == winspec)?set1:set2;
		wattron(win,last);
		}
	else wattroff(win,last);
	}

void highlightPane(WINDOW *win, int winspec, int width) {
	static int last;
	if (winspec != -1) {
		if (currentFocus == winspec) {
			mvwprintw(win,1,width,"*");
			last = COLOR_PAIR(0); // COLOR_PAIR(CLR_RB);
			}
		else last = COLOR_PAIR(CLR_BDIM);
		// last = COLOR_PAIR(CLR_WDIM)|A_BOLD; // not likely (can't change color)
		wattron(win,last);
		}
	else wattroff(win,last);
	}

/* code to fill all the space on a particular line */
int trimLine(int limit, char *prefix, char *entry, char hasSuffix) { // , int *fl) {
	int overflow = 1;
	int fieldLength;
	int entryLength = strlen(entry) + hasSuffix;
	memset(pathBuf,' ',LINELIMIT);
	pathBuf[0] = 0;
	int ll = limit;
	if (ll >= LINELIMIT) ll = LINELIMIT-1;
	if (prefix == NULL) prefix = "";
	while((strlen(prefix)+entryLength) >= (ll+overflow)) {
		if (overflow) { ll -= 3; overflow = 0; }
		if (*prefix == 0) { prefix = NULL; break; }
		if ((prefix = strchr(prefix+1,'/')) == NULL) break;
		prefix++;
		}
	if (prefix == NULL) {
		strcat(pathBuf,"...");
		if (entryLength >= ll) strcat(pathBuf,&entry[entryLength-ll]);
		else { ll--; strcat(pathBuf,"/"); strcat(pathBuf,entry); }
		}
	else {
		if (!overflow) { strcat(pathBuf,".../"); }
		strcat(pathBuf,prefix);
		strcat(pathBuf,entry);
		}
	if (hasSuffix) strcat(pathBuf,"/");
	fieldLength = strlen(pathBuf);
	pathBuf[fieldLength] = ' '; // clear the end-of-string null
	ll = limit;
	if (ll >= LINELIMIT) ll = LINELIMIT-1;
	pathBuf[ll] = 0;
	// if (fl != NULL) *fl = fieldLength;
	return fieldLength;
	}

void modifyHighlight(char scanDrv) {
        bool isValid = checkMapping(true,true);
	displaySelector(&sels[LIST_AUX1],(isValid)?3:2);
	displaySelector(&sels[LIST_AUX2],(isValid)?3:2);
	wrefresh((scanDrv)?wins[WIN_AUX2]:wins[WIN_AUX1]);
        }

void createWindow(int winspec) {
	switch(winspec) {
#ifdef LINEAR_WINDOWS
		case WIN_SOURCE: wins[winspec] = newwin(LINES/4,COLS,1,0); break;
		case WIN_TARGET: wins[winspec] = newwin(LINES/4,COLS,LINES/4+1,0); break;
		case WIN_AUX1: wins[winspec] = newwin(LINES-2*(LINES/4)-2,COLS/2,2*(LINES/4)+1,0); break;
		case WIN_AUX2: wins[winspec] = newwin(LINES-2*(LINES/4)-2,COLS-COLS/2-1,2*(LINES/4)+1,COLS/2+1); break;
		case WIN_PROGRESS: wins[winspec] = newwin(LINES-2*(LINES/4)-2,COLS,2*(LINES/4)+1,0); break;
#else
		case WIN_SOURCE: wins[winspec] = newwin(LINES/4,COLS,1,0); break;
		case WIN_TARGET: wins[winspec] = newwin(LINES/4,COLS,LINES-LINES/4-1,0); break;
		case WIN_AUX1: wins[winspec] = newwin(LINES-2*(LINES/4)-2,COLS/2,LINES/4+1,0); break;
		case WIN_AUX2: wins[winspec] = newwin(LINES-2*(LINES/4)-2,COLS-COLS/2-1,LINES/4+1,COLS/2+1); break;
		case WIN_PROGRESS: wins[winspec] = newwin(LINES-2*(LINES/4)-2,COLS,LINES/4+1,0); break;
#endif
		}
 	refresh();
//	wnoutrefresh(wins[winspec]);
//	doupdate();
	}

void fillAttached(selection *sel) {
	int i;
	int highPos = 0;
	bool currentPos;
	if (sel->count && sel->sources[sel->position].diskNum >= 0) {
		/* check if there is a locked entry */
		sel->attached->count = 0;
		for (i=0;i<sel->count;i++) {  // enumerate the locked entries
			currentPos = (i == sel->position);
			if (currentPos || (sel->sources[i].state & ST_SEL)) { // highlighted or selected entry; add to partition list
				if (sel->attached->count) addEntry(sel->attached," ","",ST_NOSEL);
				if (currentPos) highPos = sel->attached->count;
				// if (dset.drive[sel->sources[i].diskNum].map == NULL)
					// populateAttached(&(dset.drive[sel->sources[i].diskNum]),sel->attached,0,sel->sources[i].diskNum);
					populateAttached(sel->sources[i].location,sel->attached,0,sel->sources[i].diskNum);
				}
			}
		sel->attached->position = highPos;
// mvprintw(1,1,"Pos [%i]\n",highPos);
		/* TODO: position the attached frame selector and top on the disk currently selected */
		}
	}

void limitDisplay(WINDOW *win, int vert, int horiz, int width, char *string) {
	char selBuf[LINELIMIT];
	if (width >= LINELIMIT) width=LINELIMIT-1;
	if (strlen(string) > width) { strncpy(selBuf,string,width); selBuf[width] = 0; mvwprintw(win,vert,horiz,selBuf); }
	else mvwprintw(win,vert,horiz,string);
	}

void managePrimary(bool rescan, WINDOW *win, int winspec,bool blank, selection *sel) {
        char *fpath = NULL;
        char *desc = NULL;
        char *tmp;
        item *itm;
	int i=0;
	if (winspec == WIN_SOURCE) {
		switch(mode) {
			case RESTORE: desc = "SOURCE IMAGE"; fpath = sel->currentPath; i=1; break;
			case BACKUP: desc = "SOURCE DISK"; fpath = ""; break;
			case ERASE: desc = "ERASE ALGORITHM"; break;
			case TRANSFER: desc = "SOURCE FILES"; fpath = sel->currentPath; i=1; break;
			}
                }
	else {
		switch(mode) {
			case RESTORE: desc = "TARGET DISK"; fpath = ""; break;
			case BACKUP: desc = "TARGET IMAGE"; fpath = sel->currentPath; i=1; break;
			case ERASE: desc = "TARGET DISK"; fpath = ""; break;
			case TRANSFER: desc = "TARGET DIRECTORY"; fpath = sel->currentPath; i=1; break;
			}
		}
	setAttr(win,winspec,A_BOLD,0);
	if ((fpath == NULL) || (blank)) mvwprintw(win,1,1,desc);
	else {
		itm = (sel->count)?&(sel->sources[sel->position]):NULL;
		if (itm == NULL) tmp = "";
		else if (*fpath) tmp = itm->location;
                else if (i) { // -1 == (char *), otherwise (part *) (special case only)
                	if (itm->diskNum == -2) tmp = itm->location;
			else if (itm->diskNum == -1) tmp = ((disk *)itm->location)->deviceName;
                	else tmp = ((partition *)itm->location)->deviceName; // party
                	}
                else { // diskNum >= 0 is still a mapped drive for DRIVE_LIST
                	if (itm->diskNum == -2) tmp = itm->location;
                        else tmp = ((disk *)itm->location)->deviceName; // party
                	}
                trimLine(COLS-7-strlen(desc),fpath,tmp,(sel->count && itm->identifier == NULL));
                mvwprintw(win,1,1,"%s: %s",desc,pathBuf);
                }
        setAttr(win,-1,0,0);

			highlightPane(win,winspec,sel->width);
                        box(win,0,0);
			highlightPane(win,-1,0);
#ifdef HARMONY
                        if (winspec == WIN_SOURCE) {
                                mvwhline(win,LINES/4-1,1,'-',COLS/2-2);
                                mvwaddch(win,LINES/4-1,0,ACS_VLINE);
                                mvwaddch(win,LINES/4-1,COLS/2-1,ACS_ULCORNER);
                                }
                        else {
                                mvwhline(win,0,COLS/2+2,'-',COLS-COLS/2-3);
                                mvwaddch(win,0,COLS/2+1,ACS_LRCORNER);
                                mvwaddch(win,0,COLS-1,ACS_VLINE);
                                }
#endif
                        wstandend(win);
                        if (rescan) {
                                if ((winspec == WIN_SOURCE && mode == BACKUP) || (winspec == WIN_TARGET && (mode == RESTORE || mode == ERASE))) {
					fillAttached(sel);
					if (mode == RESTORE) busyMap = checkBusyMap();
					}
                                else if ((winspec == WIN_SOURCE && mode == RESTORE) || (winspec == WIN_TARGET && mode == BACKUP)) populateImage(sel,0);
                                else sel->attached->count = 0;
                                if (expert) manageWindow(sel->attached,CLEAR);
                                }
                        if (!blank) displaySelector(sel,1);
	}

void manageSecondary(WINDOW *win, int winspec,bool blank, selection *sel) {
			highlightPane(win,winspec,sel->width);
#ifdef HARMONY
                        if (winspec == WIN_AUX1) wborder(win,ACS_VLINE,ACS_VLINE,' ',ACS_HLINE,ACS_VLINE,ACS_VLINE,ACS_LLCORNER,ACS_LRCORNER);
                        else wborder(win,ACS_VLINE,ACS_VLINE,ACS_HLINE,' ',ACS_ULCORNER,ACS_URCORNER,ACS_VLINE,ACS_VLINE);
#else
                        box(win,0,0);
#endif
			highlightPane(win,-1,0);
                        wstandend(win);
                        if (!blank) {
                        	if ((winspec == WIN_AUX1 && mode == RESTORE) || (winspec == WIN_AUX2 && mode == BACKUP)) displayImage(win,sel);
                      	 	if (sel->count) {
                                	if (!strncmp(sel->sources[0].identifier," blank ",7) || !strncmp(sel->sources[0].identifier,"  ???",5)) { // if there's no partitions on a disk
                                        	mvwprintw(win,sel->vert-1,5," Disk");
                                        	}
                                	if (sel->currentPath != NULL) limitDisplay(win,sel->vert-1,5,sel->width-sel->horiz,sel->currentPath);
// mvwprintw(win,sel->vert-1,5,sel->currentPath);
                                	}
                        	displaySelector(sel,1);
				if (mode == RESTORE && sels[LIST_AUX1].count && sels[LIST_AUX2].count) modifyHighlight(winspec == WIN_AUX1);

                        	} // action != BLANK
#ifdef DEBUGPART
                        // show full partition info
                        if (winspec == WIN_AUX1 && mode == BACKUP) {
                                showParts(wins[WIN_AUX2],&(dset.drive[sels[LIST_DRIVE].sources[sels[LIST_DRIVE].position].diskNum]));
                                }
                        if (winspec == WIN_AUX2 && mode == RESTORE) {
                                showParts(wins[WIN_AUX1],&(dset.drive[sels[LIST_DRIVE].sources[sels[LIST_DRIVE].position].diskNum]));
                                }
#endif
	}

void setSizes(selection *sel, int winspec) {
	sel->horiz = 5;
	switch(winspec) {
		case WIN_SOURCE: case WIN_TARGET: sel->vert = 3; sel->width = COLS-3; sel->limit = LINES/4-sel->vert-2; break;
		case WIN_AUX1: case WIN_AUX2: sel->vert = (sel == &sels[LIST_AUX1])?6:3; sel->width = COLS/2-4; sel->limit = LINES-2*(LINES/4)-sel->vert-4; break;
		}
	}

/* match precise start/length of both partitions to make sure we can restore MBR over an occupied space */
// returns true if there are mount issues (source = NULL if map set and to be tested, SOURCE = something if we're checking if it can match)
bool old_hasMountMismatch(disk *source, disk *target) {
	int i;
	bool hasMount = false;
	bool reflected = false;
	partition *sourceParts;
	if (source == NULL) {
		reflected = true;
		source = (disk *)target->map;
		sourceParts = target->auxParts;
		}
	else sourceParts = source->parts;
	for (i=0;i<target->partCount;i++) { // see if we need to make sure the entire table is identical
		if (verifyQuickMount(target->parts[i].deviceName)) {
			if (source->partCount != target->partCount) return true;
			hasMount = true;
			if (reflected) {
				sourceParts[i].deviceName = target->parts[i].deviceName; // point mounted to existing section
				// sourceParts[i].size = target->parts[i].size;
				sourceParts[i].type = target->parts[i].type;
				sourceParts[i].diskLabel = target->parts[i].diskLabel;
				sourceParts[i].state = CLR_RB;
				sourceParts[i].map = NULL;
				}
			}
		}
	if (!hasMount) return false; // no mounts, so we can put another partition table over it
	for (i=0;i<target->partCount;i++) {
		if (target->parts[i].num != source->parts[i].num) return true;
		if (((source->sectorSize * sourceParts[i].length) == (target->sectorSize * target->parts[i].length)) &&
			((source->sectorSize * sourceParts[i].start) == (target->sectorSize * target->parts[i].start))) continue;
		return true;
		}
	return false;
	}

int checkOverlap(long startNew, long startExisting, long sizeNew, long sizeExisting) {
	if (startNew == startExisting) {
		if (sizeNew == sizeExisting) return 1; // identical location
		if (sizeNew > sizeExisting) return 2; // encompassed
		return 3; // smaller than original (requires RAM offload for restore partition to be accepted)
		}
	if (startNew >= (startExisting + sizeExisting)) return 0; // no problem
	if (startExisting >= (startNew + sizeNew)) return 0; // no problem
	return 3;
	}

// 0 = nothing mounted, 1 = something else mounted, 2 = current image mount (needn't be sysres), 4 = restore would get destroyed on remount/image too large/drive busy, 8 = must reload, 16 = something is mounted on the free space area
int hasMountMismatch(disk *source, disk *target) {
	int i, j, n, m = 1;
	bool reflected = false;
	bool isRestore;
	int restorePart = 0;
	int state = 0;
	int offset = 0;
	partition *sourceParts;
	if (source == NULL) {
		reflected = true;
		source = (disk *)target->map;
		sourceParts = target->auxParts;
		}
// TODO: Add checks for areas in between the active partitions to see if there are any mounts that might interfere with the partition table empty space blocks
	else sourceParts = source->parts;
	if ((target->sectorSize * target->deviceLength) < (source->sectorSize * source->deviceLength)) return 4; // image too large
	for (i=0;i<target->partCount;i++) {
		isRestore = false;
	// we can also check the target label here, such as /opt/data, etc., to offer an upgrade vs. restore option. Data part can also be encompassing. [future work]
	// or, we could just have images w/empty data partitions; in factory, could install add'l parts from same image
		if (verifyQuickMount(target->parts[i].deviceName)) { // is mounted; see what it is
			if (currentImage.mount == NULL || strcmp(target->parts[i].deviceName,(currentImage.isDisk)?((disk *)currentImage.mount)->deviceName:((partition *)currentImage.mount)->deviceName)) state |= 1; // other mounted
			else isRestore = true;
			if ((!(source->diskType & DISK_TABLE)) || source->state != ST_IMG) return 4; // can't write MBR
			m = 1;
			for (j=0;j<source->partCount;j++) { // see if map/potential map has overlap
				n = checkOverlap(source->sectorSize * sourceParts[j].start,target->sectorSize*target->parts[i].start,
					source->sectorSize * sourceParts[j].length, target->sectorSize * target->parts[i].length);
				if (!n) continue; // no overlap
				if (n == 1 || n == 2) m = 0;
				if (reflected) { // unmap this existing partition
					if (n == 1) { // identical; display existing mount info
						sourceParts[j].deviceName = target->parts[i].deviceName; // point mounted to existing section
                                		// sourceParts[j].size = target->parts[i].size;
                                		sourceParts[j].type = target->parts[i].type;
                                		sourceParts[j].diskLabel = target->parts[i].diskLabel;
						}
                                	sourceParts[j].state = CLR_RB; // mark as busy
                                	sourceParts[j].map = NULL; // unmap
					}
				if (isRestore) {
					if (n == 3) state |= 4; // can't remount restore
					else { // keep track of new mount NUMBER if we remount this to read the new table (can be larger than original)
						state |= 2;
						currentImage.newPartNum = sourceParts[j].num;
						}
					}
				} // for j=0
			if (m) state |= 4; // new table would clear/overwrite existing parts
			if (isRestore && !(state & 2)) state |= 4; // restore not available if we reload table
			}
		} // for i=0
	if ((target->partCount == source->partCount) && !(state & 4)) { // see if we'd have to reload the table to reflect the name table
		for (i=0;i<target->partCount;i++) {
			if (target->parts[i].num != sourceParts[i].num) { state |= 8; break; }
			if (checkOverlap(source->sectorSize * sourceParts[i].start, target->sectorSize * target->parts[i].start,
				source->sectorSize * sourceParts[i].length, target->sectorSize * target->parts[i].length) != 1) { state |= 8; break; }
			}
		}
	else if (source->partCount) state |= 8; // don't need to reload if it's a target loop device
	if ((state & 8) && (state & 1)) return 4; // can't reload a table if it's busy
	return state;
	}

void manageWindow(selection *sel, int action) {
	int i; // generic index
	if (sel == NULL) sel = currentWindowSelection();
	if (sel->position == -1) sel->position = 0;
	int winspec = location(sel);
	bool rescan = false;
	char *fpath = NULL;
	char *desc = NULL;
	char *tmp;
	item *itm;
	int origPos = sel->position; // used when there are no selectable choices
	bool normalSelector = true;
	if (wins[winspec] == NULL) createWindow(winspec);
	WINDOW *win = wins[winspec];
	if (!sel->horiz) setSizes(sel,winspec);
	switch(action) {
		case DISKUP: normalSelector = false; action = UP;
		case UP: if (sel->position > 0) { sel->position--; rescan = true; } break;
		case DISKDOWN: normalSelector = false; action = DOWN;
		case DOWN: if (sel->position < (sel->count -1)) { sel->position++; rescan = true; } break;
		case PGUP:
			if (sel->position >= sel->limit) { sel->position -= sel->limit; rescan = true; }
                	else if (sel->position > 0) { sel->position = 0; rescan = true; }
			action = UP;
			break;
		case PGDOWN:
	                if (sel->position < (sel->count - sel->limit - 1)) { sel->position += sel->limit; rescan = true; }
			else if (sel->position < (sel->count-1)) { sel->position = sel->count - 1; rescan = true; }
			action = DOWN;
			break;
		case INIT: rescan = true; action = CLEAR; break;
		}
	if (rescan && sel->count) { // movement occurred; make sure it didn't land in a non-selectable spot
		char hasTurned = 0;
		while(true) { //  || (sel->sources[sel->position].state == CLR_WDIM)) { // nosel, or grayed-out
			if ((sel->sources[sel->position].state != ST_NOSEL) && (normalSelector || checkMapping(true,true))) break;
			if (action == UP) {
				if (sel->position > 0) sel->position--;
				else if (hasTurned) { sel->position = origPos; break; }
				else { action=DOWN; hasTurned = 1; }
				}
			else if (action == DOWN) {
				if (sel->position < (sel->count-1)) sel->position++;
				else if (hasTurned) { sel->position = origPos; break; }
				else { action=UP; hasTurned = 1; }
				}
			else break; // shouldn't happen; rescan can only be set with UP/DOWN/PGUP/PGDOWN
			}
		if (!normalSelector && !validMap) return; // couldn't select anything else
		} // if (rescan)
	if (sel->position >= sel->count) sel->position = 0; // shouldn't happen
	if (sel->position < sel->top) { sel->top = sel->position; action = CLEAR; }
        if (sel->position >= sel->top + sel->limit) { sel->top = sel->position - sel->limit + 1; action = CLEAR; }
	if (action == CLEAR || action == BLANK) wclear(win);
	switch(winspec) {
		case WIN_SOURCE: case WIN_TARGET: managePrimary(rescan, win, winspec, (action == BLANK), sel);
			break;
		case WIN_AUX1: case WIN_AUX2: if (expert) manageSecondary(win, winspec, (action == BLANK), sel);
			break;
		}
	showFunctionMenu(0);
	wnoutrefresh(win);
	if (action == REFRESH) doupdate(); // wrefresh(win);
	}
