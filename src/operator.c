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
#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/reboot.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "cli.h"
#include "drive.h"			// types
#include "fileEngine.h"
#include "mount.h"			// globalBuf
#include "partition.h"
#include "partutil.h"		// readable
#include "window.h"			// options, currentFocus

#define FEEDBACK_SOURCE		4
#define FEEDBACK_TARGET		5
#define FEEDBACK_ENGINE		6
#define FEEDBACK_PARTITION	7
#define FEEDBACK_TOTALSIZE	8
#define FEEDBACK_STATUS		9
#define FEEDBACK_LOCALSIZE     10
#define FEEDBACK_PROGRESS      20
#define FEEDBACK_TIME	       11
#define FEEDBACK_PERFORMANCE   12
#define FEEDBACK_TITLE	        1
#define FEEDBACK_COMPLETE      13
#define FEEDBACK_SEGMENTCOUNT 14
#define FEEDBACK_FILESIZE 15

int termWidth = 0;

extern char compression;

int lastFocus;

unsigned long httpBytesRead;

extern unsigned int opCount; // current backup operation
extern unsigned int globalOps; // total number of backup operations

extern volatile sig_atomic_t has_interrupted;

unsigned long *archPointer;
unsigned long *filePointer;

unsigned int progressLoc;
unsigned int lastProgress;
#define VALUE_COLUMN 20

/*
        init_pair(0,COLOR_WHITE,COLOR_BLACK);
        init_pair(1,COLOR_WHITE,COLOR_RED);
        init_pair(2,COLOR_WHITE,COLOR_GREEN);
        init_pair(3,COLOR_WHITE,COLOR_YELLOW);
        init_pair(4,COLOR_WHITE,COLOR_BLUE);
        init_pair(5,COLOR_WHITE,COLOR_MAGENTA);
        init_pair(6,COLOR_WHITE,COLOR_CYAN);
        init_pair(7,COLOR_WHITE,COLOR_WHITE);
        init_pair(8,COLOR_BLACK,COLOR_WHITE); // 1
        init_pair(9,COLOR_BLACK,COLOR_RED); // 2,4
        init_pair(10,COLOR_RED,COLOR_BLACK); // 3
*/

bool progressBar(unsigned long completed, unsigned long stored, unsigned char state);

/* fancy way of displaying two color backgrounds in a string */
void colorBand(WINDOW *win, int y, int x, int start, char *val, int c1, int c2) {
        wmove(win,y,x);
        int i;
        start -= x;
        if (start > 0 && start <= strlen(val)) {
                for(i=0;i<strlen(val);i++) {
                        if (start < i) { wattron(win,COLOR_PAIR(c2) | A_REVERSE); waddch(win,val[i]); wattroff(win,COLOR_PAIR(c2) | A_REVERSE); }
                        else { wattron(win,COLOR_PAIR(c1) | A_REVERSE); waddch(win,val[i]); wattroff(win,COLOR_PAIR(c1) | A_REVERSE); }
                        }
                return;
                }
        else if (start <= 0) c1 = c2;
        wattron(win,COLOR_PAIR(c1) | A_REVERSE); wprintw(win,"%s",val); wattroff(win,COLOR_PAIR(c1) | A_REVERSE);
        }

#define PROG_ARCHSIZE	 "  Archive: "
#define PROG_FILESIZE	 " Compress: "
#define PROG_FILEORIG    "  Current: "
#define PROG_EXPECTED    " Expected: "
#define PROG_BANDWIDTH   "Bandwidth: "

void showBarTitle(char *title, bool globalPos) {
	if (!ui_mode) return;
	WINDOW *win = wins[WIN_PROGRESS];
        int x,y;
        getmaxyx(win,y,x);
	x = (x-strlen(title))/2;
	int vert = y - ((globalPos)?4:7);
	mvwprintw(win,vert,x,title);
	}

// generally display rolling bandwidth, but at the end display the average bandwidth
void readableBandwidth(unsigned long size, bool reset) { // 0 will reset it
	static struct timeval prevTime;
	static struct timeval firstTime;
	static unsigned long lastCount;
	unsigned long bandwidth;
	unsigned long countDifference;
	unsigned long elapsed;
	struct timeval tval;
	gettimeofday(&tval,NULL);
	if (reset) {
		httpBytesRead = 0;
		lastCount = 0;
		firstTime = prevTime = tval;
		sprintf(readable,"0KB");
		return;
		}
	if (!size || size == lastCount) { prevTime = firstTime; countDifference = lastCount; }
	else if (size < lastCount) { sprintf(readable,"0KB"); return; } // shouldn't happen
	else countDifference = size - lastCount;
	elapsed = (tval.tv_sec - prevTime.tv_sec) * 1000000;
	if (tval.tv_usec != prevTime.tv_usec) { // add microseconds
		if (tval.tv_usec > prevTime.tv_usec) {
			elapsed += tval.tv_usec - prevTime.tv_usec;
			}
		else {
			elapsed += tval.tv_usec;
			elapsed -= prevTime.tv_usec;
			}
		}
	lastCount = size;
	bandwidth = (elapsed)?(countDifference * 1000000)/elapsed:0;
	prevTime = tval;
	readableSize(bandwidth);
	}

void showProgressInfo(unsigned long expectedBlocks, unsigned long currentBlock) {  // compressedSize, archiveSize are global
	int hpos = COLS-23;
	int hpos2 = hpos+11;
	WINDOW *win = wins[WIN_PROGRESS];
	if (!(expectedBlocks | currentBlock)) { // both zero; clear progress indicators
		mvwprintw(win,4,hpos,"%20s","");
		if (archPointer != NULL) {
			if (LINES > 24) mvwprintw(win,5,hpos,"%20s","");
			if (LINES > 25) mvwprintw(win,6,hpos,"%20s","");
			}
		// mvwprintw(win,4,hpos,"%s%9s",PROG_BANDWIDTH,"");
		// mvwprintw(win,5,hpos,"%20s","");
		return;
		}
	if (archPointer != NULL) {
		mvwprintw(win,3,hpos,PROG_ARCHSIZE);
		mvwprintw(win,4,hpos,PROG_FILESIZE);
		mvwprintw(win,5,hpos,PROG_FILEORIG);
		if (LINES > 24) mvwprintw(win,6,hpos,PROG_EXPECTED);
		if (LINES > 25) mvwprintw(win,7,hpos,PROG_BANDWIDTH);
		}
	else {
		mvwprintw(win,3,hpos,PROG_ARCHSIZE);
		mvwprintw(win,4,hpos,PROG_EXPECTED);
		mvwprintw(win,5,hpos,PROG_BANDWIDTH);
		}

	if (archPointer != NULL) {
		readableSize(*archPointer);
		mvwprintw(win,3,hpos2,"%8s",readable);
		readableSize(*filePointer);
		mvwprintw(win,4,hpos2,"%8s",readable);
		readableSize(currentBlock);
		mvwprintw(win,5,hpos2,"%8s",readable);
		if (LINES > 24) {
			readableSize(expectedBlocks);
			mvwprintw(win,6,hpos2,"%8s",readable);
			}
		if (LINES > 25) { // calculate bandwidth of data being read/written
			readableBandwidth((httpBytesRead)?httpBytesRead:*filePointer,false); // numbers don't look accurate here...
			mvwprintw(win,7,hpos2-1,"%7s/s",readable);
			}
		}
	else {
		readableSize(currentBlock);
		mvwprintw(win,3,hpos2,"%8s",readable);
		readableSize(expectedBlocks);
		mvwprintw(win,4,hpos2,"%8s",readable);
		readableBandwidth((httpBytesRead)?httpBytesRead:currentBlock,false);
		mvwprintw(win,5,hpos2-1,"%7s/s",readable);
		}
	}

void drawProgressBar(int num, int cols, int xpos, int ypos, int fg, int bg, char *s1, char *s2) {
	int x,y;
	WINDOW *win = wins[WIN_PROGRESS];
	getmaxyx(win,y,x);
	cols += x;
	ypos += y;
	char progress[5];
	int offset;
	int i;
	if (num < 0) {
		if (num < -100) num = -100;
		offset = (-num * (cols))/100;
		sprintf(progress,"SYNC");
		}
	else {
		if (num > 100) num = 100;
		offset = (num * (cols))/100;
		sprintf(progress,"%3i%%",num);
		}
	wattron(win,COLOR_PAIR(fg) | A_REVERSE);
        wmove(win,ypos,xpos);
        for(i=0;i<offset;i++) waddch(win,' ');
        wattroff(win,COLOR_PAIR(fg) | A_REVERSE);
        wattron(win,COLOR_PAIR(bg) | A_REVERSE);
        for(i=offset;i<cols;i++) waddch(win,' ');
        wattroff(win,COLOR_PAIR(bg) | A_REVERSE);
	// text calculations
        colorBand(win,ypos,xpos+cols/2-2,offset+xpos,progress,fg,bg);       // middle; 2 = 100% (size 4) / 2
        colorBand(win,ypos,xpos,offset+xpos,s1,fg,bg);   // left
        colorBand(win,ypos,xpos+cols-strlen(s2),offset+xpos,s2,fg,bg); // right
	wrefresh(win);
	}

#include <pthread.h>
char tchar;
pthread_t tid;

void *showTimer(void *b) {
	sigset_t set; sigemptyset(&set); sigaddset(&set,SIGINT);
	sigprocmask(SIG_BLOCK,&set,NULL); // don't allow interrupts
        char *buf = b;
        int count = 0;
	int microcount = 0;
        int min, sec;
	mvprintw(0,0,"           ");
	switch(*buf) {
		case 2: mvprintw(0,0,"SYNC "); break;
		case 3: mvprintw(0,0,"HTTP "); break;
		case 4: mvprintw(0,0,"CIFS "); break;
		default: mvprintw(0,0,"WAIT "); break;
		}
	refresh();
        while(*buf) {
                // sleep(1);
		usleep(10000); // faster response times than sleep
		microcount++;
		if (microcount == 100) {
			microcount = 0;
			count++;
			min = count/60;
			sec = count%60;
			mvprintw(0,5,"%i:%02i",min,sec);
			refresh();
			}
                }
        mvprintw(0,0,"           ");
        refresh();
        // pthread_exit((void *) 1);
        }

void startTimer(int type) {
	if (!ui_mode) return;
	tchar = type;
	pthread_create(&tid,NULL,showTimer,&tchar);
	}

void stopTimer() {
	long status;
	if (!ui_mode) return;
	tchar = 0;
	pthread_join(tid,(void **)&status);
	}

#define ANYKEY "Press ENTER to continue"

/*----------------------------------------------------------------------------
**
*/
void feedbackComplete(char *msg)
	{
	int c;
	bool done;

	if(!ui_mode)
		return; // nothing to show

	WINDOW *win = wins[WIN_PROGRESS];
	if(!*msg && lastProgress == PROGRESS_VERIFY)
		{
		globalBuf[80] = 0;
		mvwprintw(win,progressLoc-1,VALUE_COLUMN,&globalBuf[40]);
		}

	if(!*msg)
		showProgressInfo(0,0); // clear all but the first entry

	mvwprintw(win,progressLoc,VALUE_COLUMN,"%-40s",(*msg)?msg:"Completed");
	mvprintw(LINES-1,(COLS-strlen(ANYKEY))/2,ANYKEY);
	// showFunction(COLS-(18+10),"F10","DONE",0);
	// showFunction(COLS-9,"F12","RETURN",0);
	showFunction(COLS-9,"F12",(options & OPT_HALT)?"HALT  ":"REBOOT",0);
	// mvaddch(LINES-1,0,' ');
	wrefresh(win);
	done = (*msg)?false:(options & OPT_AUTO)?true:false; // normally false; definitely false if an error message was included
	if(*msg)
		options &= ~OPT_AUTO; // remove AUTO mode if something went wrong

	if(options & OPT_AUTO && !(options & (OPT_POST | OPT_REBOOT | OPT_HALT | OPT_BUFFER | OPT_PRE)))
		{
		options &= ~OPT_AUTO;
		done = false;
		} // wait for ACK even in auto mode if there's no post/reboot/shutdown

	// check for reboot with auto_mode here; force reboot if met
	while(!done && (c = getch()))
		{
		switch(c)
			{
			case KEY_F(12):
				showFunctionMenu(12);
				done = true;
				break; // reboot

			case KEY_ENTER:
			case 10:
				done = true;
				break; // enter key

			default:
				break;
			}
		}

	wclear(win);
	wrefresh(win);
	currentFocus = lastFocus;
	allWindows(CLEAR);
	showFunctionMenu(0);
	// showCurrentFocus();
	doupdate();

	return;
	}

/*

void feedbackWindow(char parameter, char *value) {
	if (!ui_mode) return;
        char progress[18]; // elapsed: remaining:
	WINDOW *win = wins[WIN_PROGRESS];
	const int hpos = 4;

        int x,y;
        int i;
        getmaxyx(win,y,x);
        int offset;
        switch(parameter) {
                case FEEDBACK_TITLE:
                        if (value == NULL) return;
                        mvwprintw(win,1,(x-strlen(value))/2,value);
                        break;
                case FEEDBACK_SOURCE:
                        mvwprintw(win,3,hpos,    "Source file: ");
                        wprintw(win,value);
                        break;
                case FEEDBACK_STATUS:
                        mvwprintw(win,4,hpos,    "     Status: ");
                        wprintw(win,value);
                        break;
		case FEEDBACK_SEGMENTCOUNT:
			mvwprintw(win,5,hpos,   "   Segments: ");
			// wprintw(win,"%i",num);
			break;
		case FEEDBACK_FILESIZE:
			mvwprintw(win,4,hpos,   "       Size: ");
                        wprintw(win,value);
                        break;
		case FEEDBACK_COMPLETE:
			break;
                }
        // wrefresh(win);
        }
*/

int getTermWidth(void) {
        struct winsize ws;
	// commenting out to avoid controlling tty mess
       	int fd = open("/dev/tty",O_RDWR);
       	if (fd < 0) err(1, "/dev/tty");
	// 0 => fd
       	if (ioctl(fd,TIOCGWINSZ, &ws) < 0) err(1,"/dev/tty");
	termWidth = ws.ws_col;
	return termWidth;
        }

// PROGRESS_COPY, PROGRESS_VERIFY, PROGRESS_BACKUP, PROGRESS_RESTORE

#define PROG_SOURCEFILE "   Source file: "
#define PROG_SIZE       "          Size: "
#define PROG_PARTITION  "   Image index: "
#define PROG_SEGMENT    "       Segment: "
#define PROG_SOURCEDEV  " Source device: "
#define PROG_STATUS     "        Status: "
#define PROG_ABORT      "       Failure: "
#define PROG_ENGINE     "Archive engine: "
#define PROG_LABEL      "      FS label: "
#define PROG_TARGET     " Target device: "
#define PROG_OPERATION  "     Operation: "
#define PROG_COMPRESS   "   Compression: "
#define PROG_SHA1SUM    "SHA1 Signature: Calculating"

void setProgress(char progressType, unsigned char *src, unsigned char *trg, unsigned int major, unsigned int minor, unsigned char type, unsigned char *label, unsigned long size, unsigned int state, archive *arch) {
	lastProgress = progressType;
	unsigned char *prog = &globalBuf[40];
	unsigned char *basefile = &trg[5];
	WINDOW *win = wins[WIN_PROGRESS];
	if (label == NULL) label = "";
	const int hpos = 4;
	int posCount = 3;
	archPointer = (arch == NULL)?NULL:&arch->totalOffset;
	filePointer = (arch == NULL)?NULL:&arch->fileBytes;
	if (!*label && ui_mode) label = "<none>";

        if (ui_mode) { // clear any artifacts if we're doing multiple operations per operation call
		mvwprintw(win,2,0,"");
		wclrtobot(win);
                box(win,0,0);
		}

	switch(progressType) {
		case PROGRESS_COPY:
			readableSize(size);
			if (major == 1) sprintf(prog,"img => %s (%s, 1 segment)",basefile,readable);
			else sprintf(prog,"img => %s (%s, %i of %i segments)",basefile,readable,minor,major);
			if (ui_mode) {
				mvwprintw(win,posCount++,hpos,"%s%i of %i",PROG_SEGMENT,minor,major);
				progressLoc = posCount;
				mvwprintw(win,progressLoc,hpos,"%sReading              ",PROG_STATUS);
				}
			break;
		case PROGRESS_VERIFY: case PROGRESS_VERIFYCOPY: case PROGRESS_VALIDATE:
			readableSize(size);
			if ((basefile = strrchr(trg,'/')) == NULL) basefile = trg; else basefile++;
			if (major == 1) sprintf(prog,"%s %s (%s, 1 segment)",(progressType == PROGRESS_VERIFY)?"Verifying":"img =>",basefile,readable);
			else sprintf(prog,"%s %s (%s, %i of %i segments)",(progressType == PROGRESS_VERIFY)?"Verifying":"img =>",basefile,readable,minor,major);
			if (ui_mode) { // 25, 26, 27, 28, 29
				if (LINES > 25) mvwprintw(win,posCount++,hpos,"%s%s   ",PROG_SOURCEFILE,basefile);
			        if (LINES > 24)	mvwprintw(win,posCount++,hpos,"%s%s   ", PROG_SIZE,readable);
				if (progressType == PROGRESS_VALIDATE) { // only in UI mode
					mvwprintw(win,posCount++,hpos,"%s%i:%i   ",PROG_PARTITION,major,minor);
					}
				else {
					mvwprintw(win,posCount++,hpos,"%s%i of %i   ",PROG_SEGMENT,minor,major);
					mvwprintw(win,posCount++,hpos,PROG_SHA1SUM);
					}
				progressLoc = posCount++;
				mvwprintw(win,progressLoc,hpos,"%sProcessing",PROG_STATUS);
				}
			break;
		// TODO: modify posCount to fit into smaller windows
		case PROGRESS_BACKUP:
			sprintf(prog,"%-4s => %i %6s %-5s %-16s",basefile,minor,getEngine(type & TYPE_MASK,state),(type & DISK_MASK)?"disk":types[type & TYPE_MASK],label);
			if (ui_mode) {
				if (LINES > 25) mvwprintw(win,posCount++,hpos,"%s%i of %i   ",PROG_OPERATION,opCount,globalOps);
				mvwprintw(win,posCount++,hpos,PROG_SOURCEDEV); wprintw(win,basefile); wprintw(win,"      "); // src ==
				if (LINES > 27) mvwprintw(win,posCount++,hpos,"%s%-20s",PROG_LABEL,label);
				mvwprintw(win,posCount++,hpos,"%s%s (img %i)   ",PROG_TARGET,src,major); // ida, etc.
				if (LINES > 24) mvwprintw(win,posCount++,hpos,"%s%i   ",PROG_PARTITION,minor);
				if (LINES > 26) mvwprintw(win,posCount++,hpos,"%s%s %s",PROG_ENGINE,getEngine(type & TYPE_MASK,state),(type & DISK_MASK)?"disk":types[type & TYPE_MASK]);
				if (LINES > 28) mvwprintw(win,posCount++,hpos,"%s%s",PROG_COMPRESS,(compression == GZIP)?"zlib":(compression)?"lzma":"no");
				progressLoc = posCount++;
                                mvwprintw(win,progressLoc,hpos,"%sProcessing      ",PROG_STATUS);
				}
			break;
		case PROGRESS_RESTORE:
			sprintf(prog,"%-4s => %-4s %6s %-5s %-16s",src,basefile,getEngine(type & TYPE_MASK,state),(type & DISK_MASK)?"disk":types[type & TYPE_MASK],label);
			if (ui_mode) {
				mvwprintw(win,posCount++,hpos,"%s%s (img %i)      ",PROG_SOURCEDEV,src,major);
				if (LINES > 24) mvwprintw(win,posCount++,hpos,"%s%i   ",PROG_PARTITION,minor);
				mvwprintw(win,posCount++,hpos,"%s%s      ",PROG_TARGET,basefile);
				if (LINES > 26) mvwprintw(win,posCount++,hpos,"%s%-20s",PROG_LABEL,label);
				if (LINES > 25) mvwprintw(win,posCount++,hpos,"%s%s %s",PROG_ENGINE,getEngine(type & TYPE_MASK,state),(type & DISK_MASK)?"disk":types[type & TYPE_MASK]);
				progressLoc = posCount++;
				mvwprintw(win,progressLoc,hpos,"%s%-30s",PROG_STATUS,"Processing");
				}
			break;
		default: break;
		}
	}

void setStatus(char *status) {
	if (ui_mode) {
		WINDOW *win = wins[WIN_PROGRESS];
		if (status == NULL) return;
		mvwprintw(win,progressLoc,VALUE_COLUMN,"%-40s",status);
		wrefresh(win);
		}
	else if (status == NULL) printf("\n");
	else { printf("%s %s",&globalBuf[40],status); fflush(stdout); }
	}


/*

          Source: /dev/sda [3.4TB]
          Target: <filename>
	  Engine: [partimage,partclone,fsarchiver] GZIP		(clear when done)
       Partition: mbr of 4, 1 of 4, etc.			(clear when done)
      Image Size:

	  Status: Initializing the operation
            Size: 343MB
	Progress: 45% complete
	    Time: 00:00:01 elapsed, 00:00:01 remaining
     Performance: 453MB/s

[.............................................................]
[.............................................................]
[.............................................................]
[.............................................................]

_____________
___ DONE ____



    mbr msdos  500GB                .
.                                      ..        1  ext3  524MB /boot          .
.                                      ..        2  ext3 85.9GB /
     3  ext3 85.9GB /opt           .
.                                      ..        4  ext3  328GB /home


/*
partimage:

partimage: status: initializing the operation.
partimage: status: Partimage: 0.6.1
partimage: status: Image type: GZIP
partimage: status: Saving partition to the image file...
partimage: status: reading partition properties
partimage: status: writing header
partimage: status: copying used data blocks
File Name    Size     T:Elapsed/Estimated  Rate/min     Progress
part1.000    S: 484 MiT:00:00:41/00:00:00  R: 718 MiB/min  P: 99%

partimage: status: commiting buffer cache to disk.
partimage: Success [OK]
partimage:  Operation successfully finished:

Time elapsed: 41sec
Speed: 719.38 MiB/min
Data copied: 491.58 MiB



partclone:



fsarchiver:




*/


#define WIDTH COLS-12
#define HEIGHT LINES-12
/*
void performOperation2(char OPERATION_TYPE, char *source, char *target) {
        int ch;
        WINDOW *network = newwin(HEIGHT,WIDTH,6,6);
        box(network,0,0);
	feedbackWindow(network,FEEDBACK_PROGRESS,"Howdy there",10);
        while((ch = getch())) {
                break;
                }
        wborder(network,' ',' ',' ',' ',' ',' ',' ',' ');
        wrefresh(network);
	mvvline(6,COLS/2,' ', HEIGHT);
	refresh();
        delwin(network);
        }
*/

/*----------------------------------------------------------------------------
**
*/
void abortOperation(
		char *val,
		int   level
		)
	{ // abort is only during an operation, so progress window would be active
	WINDOW *win = wins[WIN_PROGRESS];

	if(level)
		progressBar(0,0,PROGRESS_ABORT); // change color of existing progress bar
	else
		{
		progressBar(0,PROGRESS_GREEN, PROGRESS_INIT);
		progressBar(0,0,PROGRESS_ABORT);
		}

	mvwprintw(win,progressLoc,4,PROG_ABORT);
	feedbackComplete(val);
	}

/*----------------------------------------------------------------------------
**
*/
void exitOperation(
		char *val,
		int   level
		)
	{ // no longer allow functioning of program; something is wrong
  bool done = false;
	int  c = strlen(val) + 20;

	clear();         																																				// curses.h
	mvprintw(LINES/2,(COLS-c)/2,"FATAL SYSTEM ERROR: %s",val);															// curses.h
  showFunction(COLS-9,"F12",(options & OPT_HALT)?"HALT  ":"REBOOT",0);										// windows.h
  while(!done && (c = getch()))
		{
    switch(c)
			{
      case KEY_F(12): // reboot
#ifdef PRODUCTION
				reboot((options & OPT_HALT)?LINUX_REBOOT_CMD_POWER_OFF:LINUX_REBOOT_CMD_RESTART); // linux/reboot.h
#endif
        endwin();																																					// endwin()
				exit(1);
				break;

      default:
				break;
      }
    }
	}

void prepareOperation(char *val) {
	if (!ui_mode) return;
	has_interrupted = 0;
	lastFocus = currentFocus;
	currentFocus = WIN_PROGRESS;
	allWindows(CLEAR);
	// showCurrentFocus();
	mvaddch(LINES-1,0,' '); clrtoeol(); // for (i=0;i<COLS;i++) addch(' '); // clear menu
	if (wins[WIN_PROGRESS] == NULL) createWindow(WIN_PROGRESS);
        WINDOW *win = wins[WIN_PROGRESS];
        box(win,0,0);
        wrefresh(win);
        refresh();
        int x,y;
	progressLoc = 3;
        getmaxyx(win,y,x);
        if (val == NULL) return;
        mvwprintw(win,1,(x-strlen(val))/2,val);
	}

/*
void performOperation(char OPERATION_TYPE) {
	if (wins[WIN_PROGRESS] == NULL) createWindow(WIN_PROGRESS);
        WINDOW *win = wins[WIN_PROGRESS];
	box(win,0,0);
	wrefresh(win);
	refresh();


        feedbackWindow(FEEDBACK_TITLE,"VERIFYING DISK IMAGE");
*/

/* test */
/*
        feedbackWindow(NULL,FEEDBACK_SOURCE,"Some file",0);
        int c = 0;
	int ch;
        while(ch = getch()) {
                feedbackWindow(NULL,FEEDBACK_PROGRESS,NULL,c);
                if (c < 100) c++;
                else break;
                }

	getch();
*/
/*
	wclear(win);
	wrefresh(win);
	}
*/

void calculateValues(unsigned long total, unsigned long completed, unsigned int *percentage, time_t start, time_t current, time_t *elapsed) {
	time_t estimated;
	*percentage = (total)?(unsigned int)((100 * completed)/total):0;
	if (*percentage > 100) *percentage = 100;
	*elapsed = current - start;
	estimated = (completed)?(time_t) (((unsigned long)*elapsed * total)/completed):0;
	readableTime(*elapsed,0);
	readableTime(estimated-*elapsed,1);
	}

// completed: determines % calculation, stored: determines value (size) to display (also, color spec on INIT)
bool progressBar(unsigned long completed, unsigned long stored, unsigned char state) {
	unsigned char msg = state & PROGRESS_MASK;
	unsigned char num = state & PROGRESS_NUM;
	unsigned char displayBuf[113]; // maximum of 100 per line, plus 5+7 escape codes, plus \0
	time_t currentTime = time(NULL);
        time_t elapsedTime;
        int i, tmp;
        int endANSI = 0;
        int remspace = 0;
        char *displayRight;
        unsigned int percentage;
	static char shortStop; // applies only to text-based feedback
	static int maxWidth; // applies only to text-based feedback
	static char *startColor; // applies only to text-based feedback
	static int colorSpace;
	static unsigned long globalAccumulatedCompleted, globalLastCompleted;
	static unsigned long globalAccumulatedStored, globalLastStored;
	int bg = CLR_PROGRESSDIM, fg = CLR_PROGRESS;
	static bool hasDelim = false;

	static eventSpec progress[2];
	eventSpec *event = progress;
	if (has_interrupted) {
		if ((msg == PROGRESS_UPDATE) && (num != 1)) msg = PROGRESS_CANCEL;
		// else has_interrupted = 0; // reset previous interrupt
		}
	bool hasGlobal = (progress[1].total && LINES > 30);
	bool complete = (msg == PROGRESS_COMPLETE);
	bool nowait = false;
	if (msg == PROGRESS_COMPLETE_NOWAIT) { msg = PROGRESS_COMPLETE; complete = true; nowait = true; }
	// if (complete && !ui_mode) return false; // don't handle this in non-graphical mode
	// if (complete && !ui_mode) return; // doesn't apply to text mode

	if (msg == PROGRESS_COMPLETE) msg = PROGRESS_OK;

	if (num == 1) { // #1 is the global total
		event = &progress[1];
		if (msg == PROGRESS_OK) { globalAccumulatedCompleted += completed; globalAccumulatedStored += stored; return false; } // add to global total
		else if (msg == PROGRESS_INIT) {
			globalLastCompleted = globalAccumulatedCompleted = globalAccumulatedStored = globalLastStored = 0;
			}
		else if (!hasGlobal) return false;
		if (!ui_mode) return false; // multiple bars not supported in text mode
		completed += globalAccumulatedCompleted;
		stored += globalAccumulatedStored;
		globalLastStored = stored;
		globalLastCompleted = completed;
		}
	if (msg == PROGRESS_INIT) {
		readableBandwidth(0,true); // reset bandwidth counter
		hasDelim = false;
		switch(stored) { // only relevant for text-based colors; only one can be set at any given time
			case PROGRESS_YELLOW: startColor = "\033[43;30m"; break;
			case PROGRESS_BLUE: startColor = "\033[44;38m"; break;
			case PROGRESS_RED: startColor = "\033[41;38m"; break;
			case PROGRESS_GREEN: startColor = "\033[42;30m"; break;
			case PROGRESS_LIGHT: startColor = "\033[47;30m"; break;
			default: startColor = ""; break;
			}
		colorSpace = strlen(startColor);
		maxWidth = (termWidth < 107)?termWidth-6:100; // max width of progress bar
		if (maxWidth > 60 && stored == PROGRESS_LIGHT) maxWidth = 60;
		if (!ui_mode) {
			tmp = strlen(&globalBuf[40]);
			if ((tmp > maxWidth) || (maxWidth - tmp < 27)) shortStop = 2; // display only est. time remaining
			else if (maxWidth - tmp < 46) shortStop = 1; // display short form 00:00/00:00 on right
			else shortStop = 0;
			}
		event->total = completed; event->start = event->last = currentTime;
		event->percentage = 101;
		if (!event->total && !num) event->total = 1;
		return false;
		}

	if (msg == PROGRESS_OK) { completed = event->total; event->last = 0; startColor = ""; colorSpace = 0; } // completed is not relevant
	else if  (msg == PROGRESS_SYNC) event->last = 0;
	if (msg == PROGRESS_FAIL || msg == PROGRESS_ABORT) {
		startColor = "\033[41;40m";
		colorSpace = strlen(startColor);
		event->last = 0;
		}
	calculateValues(event->total,completed,&percentage,event->start,currentTime,&elapsedTime);
	if ((msg != PROGRESS_UPDATE) || event->last != currentTime || percentage != event->percentage) {
		if (!ui_mode) {
			readableSize(stored);
			memset(displayBuf,' ',110); // clear the buffer
			i = sprintf(displayBuf,"%s%s %8s [%3i%%]",startColor,&globalBuf[40],readable,percentage);
			if (msg == PROGRESS_OK) { // move elapsed to the right
				displayRight = globalBuf;
				}
			else if (!shortStop) {
				i += sprintf(&displayBuf[i]," %s elapsed",globalBuf);
				displayRight = &globalBuf[10];
				}
			else displayRight = &globalBuf[10];
			displayBuf[i] = ' ';
			tmp = (shortStop == 2)?0:10; // length of ' remaining'
			if (shortStop == 1) {
				if (msg == PROGRESS_OK) tmp = 0;
				else tmp = 1+strlen(globalBuf);
				}
        	        i = maxWidth - (strlen(displayRight) + tmp - colorSpace); // "remaining: "
                	if (i < 0) i = 0; // small screen
			if (shortStop == 2) sprintf(&displayBuf[i],"%s",displayRight); // only display elapsed/remaining
			else if (shortStop == 1) {
				if (msg == PROGRESS_OK) sprintf(&displayBuf[i],"%s",displayRight);
				else sprintf(&displayBuf[i],"%s/%s",globalBuf,displayRight);
				}
			else sprintf(&displayBuf[i],"%s%s %s",(msg == PROGRESS_OK)?"  ":"",displayRight,(msg == PROGRESS_OK)?"elapsed":"remaining"); // change to elapsed when done?
			if (colorSpace) { // insert the color bits
				endANSI = (maxWidth * percentage) / 100 + colorSpace;
				remspace = strlen(displayBuf);
				if (endANSI >= remspace) strcat(displayBuf,"\033[0m");
				else {
					remspace -= endANSI;
					memmove(&displayBuf[endANSI+4],&displayBuf[endANSI],remspace+1); // move to right
					memmove(&displayBuf[endANSI],"\033[0m",4);
					}
				}
			printf("\r\033[?25l%s\033[K",displayBuf); // 25h => show cursor
			if (msg == PROGRESS_OK)        printf("\033[32m  OK \033[0m\033[?25h\n");
			else if (msg == PROGRESS_FAIL) printf("\033[31m FAIL\033[0m\033[?25h\n");
			else if (msg == PROGRESS_CANCEL) { printf("\033[33m%sINTR\033[0m\033[?25h\n",(maxWidth == 60)?"   ":" ");
				return true;
				}
			else {
				if (msg == PROGRESS_SYNC) printf("\033[33m SYNC\033[0m");
				fflush(stdout);
				}
			}
		else { // display something on the progress window
			if (!hasDelim) {
		                if (hasGlobal) {
					showBarTitle("  Total Progress  ",true);
					showBarTitle("Partition Progress",false);
					}
                		else showBarTitle(   "     Progress     ",true);
				hasDelim = true;
				}
			if (msg == PROGRESS_FAIL || msg == PROGRESS_CANCEL || msg == PROGRESS_ABORT) { fg = CLR_BRIGHTRED; bg = CLR_BRIGHTRED; }
			else if (complete) { fg = CLR_BRIGHTGREEN; bg = CLR_BRIGHTGREEN; }
			else if (msg == PROGRESS_SYNC) setStatus("Synchronizing disk...");
			sprintf(displayBuf,"%s elapsed",globalBuf);
			sprintf(&displayBuf[20],"%s remaining",&globalBuf[10]);
			if (num && hasGlobal) {
				sprintf(displayBuf,"%s elapsed",globalBuf);
				sprintf(&displayBuf[20],"%s remaining",&globalBuf[10]);
				drawProgressBar(percentage,-8,4,-3,fg,bg,displayBuf,&displayBuf[20]);
				} // display second bar
			else if (!num) {
				showProgressInfo(event->total,completed);
				drawProgressBar((msg == PROGRESS_SYNC)?-percentage:percentage,-8,4,(hasGlobal)?-6:-3,fg,bg,displayBuf,&displayBuf[20]);
				if (hasGlobal && (msg == PROGRESS_FAIL || msg == PROGRESS_CANCEL || msg == PROGRESS_ABORT || complete)) { // on failure, change progress bar color
					event = &progress[1];
					calculateValues(event->total,globalLastCompleted,&percentage,event->start,currentTime,&elapsedTime);
					sprintf(displayBuf,"%s elapsed",globalBuf);
					if (msg != PROGRESS_FAIL && msg != PROGRESS_CANCEL && msg != PROGRESS_ABORT) percentage = 100;
					drawProgressBar(percentage,-8,4,-3,fg,bg,displayBuf,&displayBuf[20]);
					}
				}
			if ((msg == PROGRESS_FAIL || complete) && !nowait)  feedbackComplete((msg == PROGRESS_FAIL)?"Failed":"");
			if (msg == PROGRESS_CANCEL) { setStatus("Cancelling..."); return true; }
			}
		event->last = currentTime;
		event->percentage = percentage;
		}
	return false;
	}
