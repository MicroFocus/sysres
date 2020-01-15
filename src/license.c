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

#include <fcntl.h>
#include <curses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mount.h"			// globalBuf
#include "window.h"			// options

#define CONSTFILES 6

const char *filename[] = { "/usr/share/licenses/ABOUT", "/usr/share/licenses/LICENSE", "/usr/share/licenses/GPLv2",
	"/usr/share/licenses/GPLv3", "/usr/share/licenses/LGPLv2", "/usr/share/licenses/LGPLv2.1" };

char *textfiles[CONSTFILES]; // memory buffer
char **lines[CONSTFILES]; // start of each line
int lineCount[CONSTFILES];

/*
Note that if the program is linked to libpartclone, then this program is being
released under the GPLv3 license, and the corresponding source will be available.
Future versions will hopefully just be GPL'ed.
*/
#ifdef PARTCLONE
#define LLEN 12
const char *license[] = {
	"This program is free software: you can redistribute it and/or modify",
   	"it under the terms of the GNU General Public License as published by",
    	"the Free Software Foundation, either version 3 of the License, or",
    	"(at your option) any later version.",
	"",
    	"This program is distributed in the hope that it will be useful,",
    	"but WITHOUT ANY WARRANTY; without even the implied warranty of",
    	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the",
    	"GNU General Public License for more details.",
	"",
    	"You should have received a copy of the GNU General Public License",
    	"along with this program.  If not, see <http://www.gnu.org/licenses/>."
	};
#else
#define LLEN 3
const char *license[] = {
	"This program is distributed WITHOUT ANY WARRANTY; without even the implied",
	"warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the",
	"LICENSE file for more details."
	};
#endif

void version(void) {
	unsigned char opts[64];
	*opts = 0;
	strcat(opts,"\033[33m");
#ifdef NETWORK_ENABLED
        strcat(opts," NET");
#endif
#ifdef KERNEL_ARGS
	strcat(opts," CMD");
#endif
	if (options & OPT_BACKUP) strcat(opts," BKP");
	if (options & OPT_RAMDISK) strcat(opts," RAM");
	strcat(opts,"\033[0m");
#ifdef BUILD
	printf("%s version %s, build %i%s\n%s\n",PROGRAM,VERSION,BUILD,opts,COPYRIGHT);
#else
        printf("%s version %s%s\n%s\n",PROGRAM,VERSION,opts,COPYRIGHT);
#endif
        }

void showLicense(bool doExit) {
        version();
	int i;
	printf("\n");
	for(i=0;i<LLEN;i++) {
		printf("%s\n",license[i]);
		}
	printf("\n");
        if (doExit) exit(0);
        }

void initAbout(void) {
	int i;
	for (i=0;i<CONSTFILES;i++) textfiles[i] = NULL;
	}

#define TOPLINE 1

bool matchLibrary(char *name, char *ptr, int size) {
	char *match;
	if ((match = memchr(ptr,*name,size)) != NULL) {
		if (size - (match - ptr) < strlen(name)) return false;
		if (!memcmp(match,name,strlen(name))) return true;
		}
	return false;
	}

// line = the first line (returns the last line displayed on the screen)
int displayFile(int line, int file) {
	char *fileset;
	int fd;
	int i, n;
	int offset = 0;
	struct stat64 stats;
	char **lineSpace;
	char *ptr, *lastPtr;
	if (file >= CONSTFILES) return 0; // shouldn't happen
	clear();

	if (textfiles[file] == NULL) { // read into memory and parse each line
		if (stat64(filename[file],&stats)) return 0; // couldn't read size
		fileset = (char *)malloc((stats.st_size+1) * sizeof(char));
		if (fileset == NULL) return 0; // couldn't read
		if ((fd = open(filename[file],O_RDONLY)) < 0) { free(fileset); return 0; } // couldn't open
		int remaining = stats.st_size;
		while(remaining && (n = read(fd,&fileset[offset],remaining)) >= 0) {
			remaining -= n;
			offset += n;
			}
		close(fd);
		if (n < 0) { free(fileset); return 0; } // problem reading
		fileset[offset] = 0;
		i = n = 0;
		// convert to line pointers
		lastPtr = fileset;
                while((ptr = strchr(lastPtr,'\n')) != NULL) {
		       if (!file && *lastPtr == '#') { lastPtr = ++ptr; continue; } // skip comments in the ABOUT file
                       lastPtr = ++ptr;
                       i++;
                       }
		if (!file) { i+=4 + LLEN; } // copyright
		lineSpace = (char **)malloc((i+1)*sizeof(char *)); // line buffer
		if (lineSpace == NULL) { free(fileset); return 0; } // memory issue
		lastPtr = fileset;
		i = 0;
		if (!file) {
			i += 4; // copyright
			for (n=0;n<LLEN;n++) {
				lineSpace[i++] = (char *)license[n];
				}
			}
		bool pclone_group = false;
		bool curl_license = false;
		while((ptr = strchr(lastPtr,'\n')) != NULL) {
			if (!file && *lastPtr == '#') { *ptr++ = 0; lastPtr = ptr; continue; } // skip comments on the ABOUT file
#ifndef LIBLZMA
			if (matchLibrary("liblzma",lastPtr,ptr-lastPtr+1)) { *ptr++ = 0; lastPtr = ptr; continue; } // skip this line
#endif
#ifndef NETWORK_ENABLED
			if (matchLibrary("libcurl.so",lastPtr,ptr-lastPtr+1)) { *ptr++ = 0; lastPtr = ptr; continue; } // skip this line
#else
			if (!pclone_group && matchLibrary("librt",lastPtr,ptr-lastPtr+1)) { *ptr++ = 0; lastPtr = ptr; continue; } // skip this
#endif
                       if(matchLibrary("ld-linux",lastPtr,ptr-lastPtr+1)) pclone_group = true;
		       else if (pclone_group && !matchLibrary("lib",lastPtr,ptr-lastPtr+1)) pclone_group = false;
#ifdef PARTCLONE
			if (!pclone_group) {
#else
			if (pclone_group) {
#endif
				if (
#ifndef NETWORK_ENABLED
				    matchLibrary("librt",lastPtr,ptr-lastPtr+1) ||
#endif
				    matchLibrary("libpartclone",lastPtr,ptr-lastPtr+1) ||
 				    matchLibrary("libext2fs",lastPtr,ptr-lastPtr+1) ||
				    matchLibrary("libcom_err.so",lastPtr,ptr-lastPtr+1) ||
				    matchLibrary("libxfs",lastPtr,ptr-lastPtr+1) ||
				    matchLibrary("libntfs-3g",lastPtr,ptr-lastPtr+1)
				    ) { *ptr++ = 0; lastPtr = ptr; continue; }
				}
#ifndef NETWORK_ENABLED
			if (!file) {
				if (*lastPtr == ' ' && lastPtr[1] != ' ' && (strstr(lastPtr,": ") != NULL)) curl_license = (strncmp(lastPtr," libcurl: ",10))?false:true;
				if (curl_license) { *ptr++ = 0; lastPtr = ptr; continue; }
				}
#endif
			lineSpace[i++] = lastPtr;
			*ptr++ = 0;
			lastPtr = ptr;
			}
		lineSpace[i++] = lastPtr;
		textfiles[file] = fileset;
		lines[file] = lineSpace;
		lineCount[file] = i;
		}
/*
	if ((fileset = strrchr(filename[file],'/')) != NULL) fileset++;
	mvprintw(0,COLS-(strlen(fileset)+8),"FILE: %s\n",fileset);
*/
	if (line + (LINES-1-TOPLINE) > lineCount[file]) { // keep within boundary
		if (lineCount[file] < (LINES-1-TOPLINE)) line = 0;
		else line = lineCount[file] - (LINES-1-TOPLINE);
		}
	if (TOPLINE) { // display a line at the top
		char *displayName;
 		if ((displayName = strrchr(filename[file],'/')) != NULL) displayName++;
        	else displayName = (char *)(filename[file]);
		mvaddch(0,0,' ');
		clrtoeol();
		attron(COLOR_PAIR(CLR_PROGRESS) | A_REVERSE);
		// for(i=1;i<COLS;i++) printw(" ");
		mvprintw(0,(COLS-(strlen(displayName)+12))/2,"   File: %s   ",displayName);
		attroff(COLOR_PAIR(CLR_PROGRESS) | A_REVERSE);
		}
	if (line) {
		mvaddch(0,COLS-1,ACS_UARROW);
		}
	lineSpace = lines[file];
	for (i=line;i<lineCount[file];i++) {
		lastPtr = lineSpace[i];
		if ((i - line + 2 + TOPLINE) > LINES) {
			mvaddch(LINES-1,COLS-1,ACS_DARROW);
			break; // we're at the bottom of the screen
			}
		if (!file && (i < 4)) {
                        if (i == 1) mvprintw(i-line+TOPLINE,0,"%s, version %s",PROGRAM,VERSION);
                        else if (i==2) mvprintw(i-line+TOPLINE,0,COPYRIGHT);
                        }
		else if (!file && *lastPtr == ' ' && lastPtr[1] != ' ' && ((ptr = strstr(lastPtr,": ")) != NULL)) {
			strcpy(globalBuf,lastPtr);
			globalBuf[ptr - lastPtr + 2] = 0;
			ptr += 2;
			attron(COLOR_PAIR(CLR_PROGRESS) | A_REVERSE);
			mvprintw(i-line+TOPLINE,0,globalBuf);
			attroff(COLOR_PAIR(CLR_PROGRESS) | A_REVERSE);
			mvprintw(i-line+TOPLINE,strlen(globalBuf),ptr);
			}
		else mvprintw(i-line+TOPLINE,0,lastPtr);
		}
	int percentage = (i*100)/(lineCount[file]);
	if (percentage > 100) percentage = 100;
	mvprintw(LINES-1,COLS-6,"%3i%%",percentage);
	return line;
// void showFunction(int pos, char *key, char *name, int action)
	}

void showFiles(void) {
	int currentFile = 0;
	int currentLine = 0;
	int lastLine = -1;
	int n;
	char *file;
	char fnum[4];
	int i;

	while(1) {
		if (lastLine != currentLine) {
			lastLine = currentLine = displayFile(currentLine,currentFile);
			for (n=0;n<CONSTFILES;n++) {
				if (currentFile == n) continue;
				snprintf(fnum,4,"F%i",n+1);
				if ((file = strrchr(filename[n],'/')) != NULL) file++;
				else file = (char *)filename[n];
				showFunction(n * 10,fnum,file,0);
				}
        		showFunction(COLS-18,"F11","MENU",0);
			}
		if ((n = getch()) == KEY_F(11)) return;
		switch(n) {
			case KEY_F(11): return; break;
			case KEY_UP:
				if (currentLine) currentLine--;
				break;
			case KEY_DOWN:
				if (currentLine < (lineCount[currentFile]-(LINES-1-TOPLINE))) currentLine++;
				break;
                        case KEY_NPAGE:
				if (currentLine < (lineCount[currentFile]-(LINES-1-TOPLINE))) currentLine += (LINES-1-TOPLINE);
				break;
                        case KEY_PPAGE:
				if (currentLine) {
					if (currentLine < (LINES-1-TOPLINE)) currentLine = 0;
					else currentLine -= (LINES-1-TOPLINE);
					}
				break;
/*
			case KEY_HOME:
				if (currentLine) currentLine = 0;
				break;
			case KEY_END:
				if (currentLine < (lineCount[currentFile]-(LINES-1-TOPLINE))) currentLine = lineCount[currentFile];
				break;
*/
#ifdef PRODUCTION
			default:
				for(i=0;i<CONSTFILES;i++) {
					if (n != KEY_F(i+1)) continue;
					if (currentFile != i) { lastLine = -1; currentLine = 0; currentFile = i; }
					break;
					}
				break;
#else
			default:
				if (n >= '1' && n <= '9') n -= '1';
				else n = currentFile;
				if (n >= CONSTFILES) n = currentFile;
				if (n != currentFile) { lastLine = -1; currentLine = 0; currentFile = n; }
				break;
#endif
			}
		}
	}
