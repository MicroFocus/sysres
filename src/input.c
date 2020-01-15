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

#include <curses.h>
#include <string.h>

#include "partition.h"
#include "window.h"     // globalPath, mode

extern char pathBuf[];
bool remoteEntry = false;


int mkdirCount;

int itemCompare(const void *a, const void *b);
char *getStringPtr(poolset *sel, char *str);

void removeSelection(selection *sel, int pos) {
	pos = (pos == -1)?sel->position:pos;
	if (!sel->count) return; // no entry to remove
	sel->count--;
	if (pos < sel->count) memmove(&(sel->sources[pos]),&(sel->sources[pos+1]),sizeof(item)*(sel->count - pos));
	if (sel->position >= sel->count) sel->position = sel->count-1; // last position
	if (!sel->count) sel->position = 0; // avoid issues
	}

void removeCurrent(selection *sel) {
	removeSelection(sel,-1);
	*currentImage.imagePath = 0;
	manageWindow(sel,INIT);
	}

bool removeDir(selection *sel) {
	if (!rmdir(sel->currentPath)) { navigateLeft(sel); return true; }
	else return false;
	}

int readLine(WINDOW *win, int vpos, int hpos, int limit, char *fbuf, char *prefix, int linelimit, selection *sel, bool hidden) {
	int canceled = -1;
        int cbuf = 0;
        int ch = 0;
        int fieldLength = 0;
	int lastFunc = 0;
	int msel = -1;
	int i, tmp;
	char *entry;
	if ((sel != NULL) && (!strcmp(currentImage.imagePath,sel->currentPath))) canceled = matchSelection(sel,currentImage.imageName); // current dir contains 'live' image
        cbuf = strlen(fbuf); // pre-populate the field we're editing
	if (cbuf) lastFunc = 1;
        if (cbuf >= linelimit) { cbuf = linelimit; fbuf[cbuf] = 0; }
	int cpos = cbuf;
	int fieldMax = limit - ((prefix == NULL)?0:strlen(prefix));
        do {
		// mvwprintw(win,2,2,"%i   ",ch); // temporary; display code
                if ((cbuf < linelimit) && ((cpos == cbuf) || (cbuf < fieldMax)) && (ch >= 32 && ch <= 126) && ((sel == NULL) || ch != '/')) {
			if (remoteEntry && (cpos == cbuf) && (cpos == 0)) {
				if (ch == '/') return -4; // switch remote type
				showCancel(0);
				}
			if (cpos != cbuf) memmove(&fbuf[cpos+1],&fbuf[cpos],cbuf-cpos+1);
			fbuf[cpos++] = ch;
			fbuf[++cbuf] = 0;
			}
                else { // parse special characters
                        switch(ch) {
                                case KEY_LEFT: if (!hidden && (cpos && (cbuf <= fieldMax))) cpos--; break;
				case KEY_BTAB: if (!hidden && (cpos && (cbuf <= fieldMax))) cpos = 0; break;
				case KEY_RIGHT: if ((cbuf > cpos) && (cbuf <= fieldMax)) cpos++; break;
				case '\t': if ((cbuf > cpos) && (cbuf <= fieldMax)) cpos = cbuf; break;
					// could further auto-complete filenames/directories here
				case KEY_BACKSPACE:
					if (cpos) {
						if (cpos < cbuf) memmove(&fbuf[cpos],&fbuf[cpos+1],cbuf-cpos);
						fbuf[--cbuf] = 0;
						cpos--;
if (remoteEntry && !cpos && !cbuf) return -5;
						break;
						}
					else msel = -2;
					if (sel == NULL) break; // can't break here unless it's a title
					// can't break here; otherwise the '/' backspace won't work
				case '/': if (cpos != cbuf) break; // cursor must be at end of line (fall through from backspace)
					if (lastFunc == 1) { canceled = -3; goto DONE; }  // mkdir
					if (msel == -3) { // navigate to it
						tmp = strlen(sel->currentPath);
						navigateDirectory(fbuf,sel);
						manageWindow(NULL,INIT);
						displaySelector(sel,0);
                                                wrefresh(wins[location(sel)]);
						if (tmp != strlen(sel->currentPath)) { cpos = cbuf = *fbuf = 0; }
						showCancel(0);
						if (!strcmp(currentImage.imagePath,sel->currentPath)) canceled = matchSelection(sel,currentImage.imageName); // current dir contains 'live' image
						}
					else if (msel == -2) {
						if (cpos == 1 || (strlen(sel->currentPath) == sel->currentPathStart)) { cpos = cbuf = 0; *fbuf = 0; } // "./", or at beginning
						else { // "../"
							if (!cpos) {
                						for(i=strlen(sel->currentPath)-2;i>=sel->currentPathStart;i--) { if (sel->currentPath[i] == '/') break; }
								i++;
								if (i >= sel->currentPathStart) {
									strcpy(fbuf,&sel->currentPath[i]);
									fbuf[strlen(fbuf)-1] = 0; // get rid of trailing '/'
									cpos = cbuf = strlen(fbuf);
									}
								else cpos = cbuf = *fbuf = 0;
								} else cpos = cbuf = *fbuf = 0;
							if (mkdirCount && removeDir(sel)) mkdirCount--;
							else { mkdirCount = 0; navigateLeft(sel); }
							if (!*sel->currentPath) return 0;  // shouldn't happen; same as cancel
							manageWindow(NULL,INIT);
							displaySelector(sel,0);
							wrefresh(wins[location(sel)]);
							showCancel(0);
							if (!strcmp(currentImage.imagePath,sel->currentPath)) canceled = matchSelection(sel,currentImage.imageName); // current dir contains 'live' image
							}
						}
					break;
				case 8: // SHIFT_BACKSPACE clears everything to the left
                                        if (cpos) {
                                                if (cpos < cbuf) { // clear part of the line to the left of the cursor
                                                        cbuf -= cpos;
                                                        memmove(fbuf,&fbuf[cpos],cbuf);
                                                        fbuf[cbuf] = 0;
                                                        cpos = 0;
                                                        }
                                                else { cbuf = *fbuf = 0; }
                                                cpos = 0;
						if (remoteEntry && !cbuf) return -5;
                                                }
                                        break;
                                case 330: // KEY_DELETE?
                                        if (cpos < cbuf) { memmove(&fbuf[cpos],&fbuf[cpos+1],cbuf-cpos); fbuf[--cbuf] = 0; }
					if (remoteEntry && !cpos && !cbuf) return -5;
                                        break;
                                case KEY_ENTER: case 10: goto DONE; break; // not sure if we'll accept an empty entry as 'cancel'
                                case KEY_F(6): cbuf = 0; fbuf[cbuf] = 0; canceled = -2; goto DONE; break; // cancel; don't use 'esc' in ncurses
                                // case KEY_F(6): if (lastFunc == 1) canceled = -3; goto DONE; break; // create the directory
                                default: break;
                                }
			}
		// compare against a fileset and provide options, if any
		if (sel != NULL) {
			if (!strcmp(fbuf,".") || !strcmp(fbuf,"..")) msel = -2;
			else if ((msel = matchSelection(sel,fbuf)) != -1) { if (sel->sources[msel].identifier == NULL) msel = -3; }
			if (cbuf && msel == -1) {
				if (lastFunc == 0) { showCancel(1); lastFunc = 1; }
				}
			else if (lastFunc) { showCancel(0); lastFunc = 0; }
                        }
                fieldLength = trimLine(limit,prefix,fbuf,0);
		if (hidden) memset(pathBuf,'*',fieldLength);
		mvwprintw(win,vpos,hpos,"%s ",pathBuf); // clears cursor at rightmost location, if any
		if (cpos < cbuf) {
                	wattron(win,COLOR_PAIR(CLR_YB) | A_REVERSE);
			mvwprintw(win,vpos,hpos+fieldLength-(cbuf-cpos),"%c",fbuf[cpos]);
			wattroff(win,COLOR_PAIR(CLR_YB) | A_REVERSE);
			}
		else {
			wattron(win,COLOR_PAIR(CLR_GB) | A_REVERSE);
			mvwprintw(win,vpos,hpos+fieldLength," ");
			wattroff(win,COLOR_PAIR(CLR_GB) | A_REVERSE);
			}
                wrefresh(win);
                } while((ch = getch()) || 1); // accept ctrl-space
DONE:
        mvwprintw(win,vpos,hpos+fieldLength," "); // clear cursor
        return canceled;
        }

void addImageName(selection *sel, bool clear) {
	char fbuf[MAX_FILE];
	char dbuf[256];
	WINDOW *win = wins[location(sel)];
	char *desc = (mode == BACKUP)?"TARGET IMAGE":"TARGET DIRECTORY";
	populateImage(sel,1);
	manageWindow(sel->attached,CLEAR);
	displaySelector(sel,0);
	mvwprintw(win,1,1,"%s: ",desc);
	int xlen = strlen(desc)+3;
	int prevPos = sel->position;
	int livePos = -1;
	if (*currentImage.imagePath) strcpy(fbuf,currentImage.imageName); // TODO: Switch to a structure-based entry and bypass fbuf with imageName directly.
	else *fbuf = 0;
	showCancel(0);
	if (clear) mkdirCount = 0;
	livePos = readLine(win,1,xlen,COLS-4-xlen,fbuf,sel->currentPath,MAX_FILE-1,sel,false); // filename
	if (livePos == -3) { // try to make the directory; otherwise cancel it
		sprintf(globalPath,"%s%s",sel->currentPath,fbuf);
		if (!mkdir(globalPath,0)) {
			mkdirCount++;
			navigateDirectory(fbuf,sel);
			manageWindow(NULL,INIT);
			addImageName(sel,false);
			return;
			}
		*fbuf = 0; // mkdir didn't work
		}
	if (!strcmp(fbuf,".") || !strcmp(fbuf,"..")) *fbuf = 0; // these two not allowed
	if (*fbuf) { // add an entry
		if ((sel->position = matchSelection(sel,fbuf)) == -1) { // entry does not yet exist
			if (livePos >= 0) removeSelection(sel,livePos);
			addSelection(sel,fbuf,"",1);
			qsort(sel->sources,sel->count,sizeof(item),itemCompare);
			sel->position = matchSelection(sel,fbuf); // should never be -1
		//	if (*currentImage.imagePath && !strcmp(fbuf,currentImage.imageName)) strcpy(dbuf,currentImage.imageTitle); // keep title if we're using the same name from a different directory
			if (*currentImage.imagePath) strcpy(dbuf,currentImage.imageTitle);
			else *dbuf = 0;
			}
		else {
			if (strcmp(currentImage.imageName,sel->sources[sel->position].location)) { // image is not the current edit; select it
				displaySelector(sel,1);
				manageWindow(sel,INIT);
				return;
				}
			strcpy(dbuf,currentImage.imageTitle); // image is current edit; retain the previous title
			}
		manageWindow(sel,REFRESH); // recalculate boundaries and clear space
		displaySelector(sel,0);
		showCancel(0);
		if (readLine(win,sel->vert+(sel->position-sel->top),sel->horiz,sel->width-sel->horiz-1,dbuf,NULL,255,NULL,false) != -2) {  // title line
			sel->sources[sel->position].location = currentImage.imageName;
			sel->sources[sel->position].identifier = currentImage.imageTitle;
			sel->sources[sel->position].state = CLR_RB;
			strcpy(currentImage.imagePath,sel->currentPath); // set it to the current working directory
			strcpy(currentImage.imageName,fbuf); // set it to the current filename
			strcpy(currentImage.imageTitle,dbuf);
			}
		else { // remove the descriptor from the list
			removeSelection(sel,-1);
			if (*currentImage.imagePath && (strcmp(sel->currentPath,currentImage.imagePath) == 0)) { // revert to the previous image
				addSelection(sel,currentImage.imageName,currentImage.imageTitle,1);
				sel->sources[sel->count-1].state = CLR_RB;
				qsort(sel->sources,sel->count,sizeof(item),itemCompare);
				sel->position = matchSelection(sel,currentImage.imageName);
				}
			else sel->position = prevPos;
			manageWindow(sel,CLEAR);
			}
		displaySelector(sel,1);
		showFunctionMenu(0);
                wrefresh(win);
		}
	manageWindow(sel,INIT);
	// else manageWindow(sel,REFRESH); // we could also cancel the entry here (optional, really, since we can only have one image at a time)
	}
