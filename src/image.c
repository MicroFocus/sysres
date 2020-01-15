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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <ncurses.h>
#include <dirent.h>

#include "cli.h"
#include "mount.h"			// globalBuf, currentLine
#include "partition.h"
#include "partutil.h"		// readable
#include "position.h"		// busyMap
#include "window.h"     // globalPath, options, mode, setup
#include "fileEngine.h"
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

diskset iset;

#define MAXPDEF 80
#define PSIZE 16

char selectedType = 0; // current type of file

char imageSizeString[10];
unsigned long imageSize;
char signature[9];
char imageDevice[64];
int loopDrive = -1; // disk which contains separate partitions

extern char pbuf[];

char *getStringPtr(poolset *sel, char *str);

unsigned long getImageTitle(char *filename, char *buf, archive *arch);

int partitionCompare(const void *a, const void *b) {
        const partition *ia = (const partition *)a;
        const partition *ib = (const partition *)b;
	if (ia->num == ib->num) return 0;
	return (ia->num < ib->num)?0:1;
        }


void addEntry(selection *sel, char *val, char *loc, char state) {
	if (sel->count >= sel->size && expandSelection(sel,0)) return; // can't add more
	item *i = &(sel->sources[sel->count++]);
//	i->identifier = getStringPtr(&sel->pool,val);
	i->identifier = val;
	i->location = loc;
	i->state = state;
	}

char *imagePath(char *append) {
        static char *pathStart;
        if (append == NULL) pathStart = &globalPath[strlen(globalPath)];
        else sprintf(pathStart,"/%s",append);
        return globalPath;
        }

void convertReadable(float val, char *out, char hdspec) {
        float divisor = 1.0;
        char delim = ' ';
	if (hdspec) {
		if (val >= 1000000000000) { delim = 'T'; divisor = 1000000000000.0; }
		else if (val >= 1000000000) { delim = 'G'; divisor = 1000000000.0; }
		else if (val >= 1000000) { delim = 'M'; divisor = 1000000.0; }
		else if (val >= 1000)  { delim = 'K'; divisor = 1000.0; }
		}
	else {
        	if (val >= 1099511627776) { delim = 'T'; divisor = 1099511627776.0; }
        	else if (val >= 1073741824) { delim = 'G'; divisor = 1073741824.0; }
        	else if (val >= 1048576) { delim = 'M'; divisor = 1048576.0; }
        	else if (val >= 1024) { delim = 'K'; divisor = 1024.0; }
		}
        if (delim == ' ') sprintf(out,"%.0f bytes",val);
	else if (hdspec) sprintf(out,"%.1f%cB",(val/divisor),delim);
        else sprintf(out,"%.2f%cB",(val/divisor),delim);
        }

// performs field validation on a number
int convertToNum(char *string, void *num, char type) {
	if (string == NULL || *string == '-') return 1;
	switch(type) {
		case 2: *((int *)num) = strtoll(string,NULL,16); break; // hex number
		case 1: *((sector *)num) = atol(string); break; // atoll(string); break;
		default: *((int *)num) = atoi(string);
		}
	return 0;
	}

void parseImageLine(char *line) {
	char sizing[64];
	int disknum, partnum = 0;
	char *num, pcount = 0;
	unsigned char type;
	char *tmp;
	sector x;
	disk *d;
	partition *p;
	if (convertToNum(strtok(line,":"),&disknum,0)) return;
	num = strtok(NULL," ");
	if (convertToNum(num,&partnum,0)) return;
	if (partnum) { // read partition
		if (!disknum || (disknum != iset.dcount)) return; // should have been set by mbr record
		d = &(iset.drive[disknum-1]);
		if (d->partCount >= d->psize && expandPartition(d)) return; // unable to match additional partitions
		p = &(d->parts[d->partCount]);
		p->num = partnum; p->map = NULL;
		// get partition
		if (convertToNum(strtok(NULL," "),&p->start,1)) return;
		if (convertToNum(strtok(NULL," "),&p->length,1)) return;
		if (strtok(NULL," ") == NULL) return; // end location, not needed
		// if ((type = strtok(NULL,":")) == NULL) return; // format type; not used
		strtok(NULL,":"); // type
		// if (convertToNum(strtok(NULL,":"),&p->tbyte,2)) return; // numerical type
		strtok(NULL,":"); // skip p->tbyte
		p->flags = 0; // reset this
		if ((tmp = strtok(NULL," ")) == NULL) return; // human size of partition NOTE: THIS VALUE IS NOT CHECKED
		// p->size = getStringPtr(&iset.pool,tmp);
		while((tmp = strtok(NULL," ")) != NULL) { // most of these should be exclusive
			if (!strcmp(tmp,"PRI")) { p->flags |= PRIMARY; pcount++; }
			if (!strcmp(tmp,"LOG")) { p->flags |= LOGICAL; pcount++; }
			if (!strcmp(tmp,"EXT")) { p->flags |= EXTENDED; pcount++; }
			if (!strcmp(tmp,"BOOT")) p->flags |= BOOTABLE;
			}
		if (pcount != 1) return; // must have one and only one primary type
		sprintf(pbuf,"/img/title%i:%i",disknum,partnum);
		p->deviceName = getStringPtr(&iset.pool,pbuf);
		p->diskLabel = "";
		p->type = 0;
		p->state = 0;
		d->partCount++;
		}
	else { // read drive spec
		if (iset.dcount+1 != disknum) return; // should be one less than the current disk count (i.e., must be an unbroken incremental count)
		if (iset.dcount >= iset.size && expandDisk(&iset)) return; // too many entries
		d = &iset.drive[iset.dcount];
		if (!strcmp(num,"msdos")) d->diskType = DISK_MSDOS;
                else if (!strcmp(num,"gpt")) d->diskType = DISK_GPT;
                else return; // unsupported table at this time
		if (convertToNum(strtok(NULL," "),&d->deviceLength,1)) return;
		if (convertToNum(strtok(NULL," "),&d->sectorSize,0)) return;
		strtok(NULL," "); // d->physSectorSize (ignore for now)
		sprintf(pbuf,"/img/title%i",disknum);
		d->deviceName = getStringPtr(&iset.pool,pbuf);
		d->diskName = "";
		if ((tmp = strtok(NULL," ")) == NULL) return; // human size of partition NOTE: THIS VALUE IS NOT CHECKED
		// d->size = getStringPtr(&iset.pool,tmp);
		d->state = 0; d->map = NULL;
		// the rest is moot (human readable size of disk)
		d->partCount = 0; // start with zero partitions
		iset.dcount++;
		}
	}
// MBR: 446 + 64 bytes + 2 bytes
// 1=status, 3 = chs address, 1 part type, 3 = chs address, 4 lba, 4 # of sectors

void displayImage(WINDOW *win, selection *sel) {
	char *type;
	int i;
	switch(selectedType) {
		case 1: type = "Working Archive"; break;
		case 2: type = "Archive"; break;
		case 3: type = "Directory"; break;
		case 4: type = "File"; break;
		case 5: type = "Disk Drive"; break;
		default: type = "Unknown"; break;
		}
	if (selectedType == 1 || selectedType == 2) {
		if (selectedType == 1) mvwprintw(win,2,4,"     Type: %s",type);
		else mvwprintw(win,2,4,"     Type: %s [%s]",type,imageSizeString);
		if (!iset.dcount) mvwprintw(win,4,4,"<empty>");
		else {
			mvwprintw(win,3,4,"Signature: ");
			// for (i=0;i<md_len;i++) sprintf(&imageData[i*2],"%02x",md_value[i]);
			// if (!md_len) strcpy(imageData,"<none>");
			limitDisplay(win,3,15,sel->width-sel->horiz-10,signature);
			}
		}
	}

void populateImageSelector(selection *sel) {
	int i;
	sel->count = 0;
	for (i=0;i<iset.dcount;i++) {
		if (sel->count) addEntry(sel," ","",ST_NOSEL);
		populateAttached(&iset.drive[i],sel,0,i);
		}
	}

void nextDeviceName(void) {
        int len = strlen(imageDevice);
        if (!len) { strcpy(imageDevice,"ida"); return; }
        while(len > 1 && imageDevice[--len] == 'z') { ; }
        if (len == 1) strcat(imageDevice,"a");
        else imageDevice[len] += 1;
        memset(&imageDevice[len+1],'a',strlen(imageDevice)-(len+1));
	}

void addImageEntry(imageEntry *e) {
	disk *d;
	partition *p;
	if (!e->major && !e->minor) { debug(INFO, 0,"Invalid index.\n"); return; } // invalid
	if (e->minor) { // a partition
		if (e->major) { // attached to a disk
			if (e->major != iset.dcount) { debug(INFO, 0,"Major index order error.\n"); return; } // error; should be after a disk definition
			d = &(iset.drive[e->major-1]);
			if (d->partCount >= d->psize && expandPartition(d)) { debug(INFO, 0,"expandPartition limit reached.\n"); return; } // unable to add additional partitions
			p = &(d->parts[d->partCount++]);
			p->num = e->minor; p->map = NULL;
			p->length = e->length;
			p->start = e->start;
			p->flags = PRIMARY;
			if (e->fsType & TYPE_BOOTABLE) p->flags |= BOOTABLE;
			sprintf(globalBuf,"/dev/%s%i",imageDevice,e->minor);
			p->deviceName = getStringPtr(&iset.pool,globalBuf);
			// p->deviceName = "PART";
			if (*e->label) p->diskLabel = getStringPtr(&iset.pool,e->label);
			else p->diskLabel = "";
			p->type = e->fsType & TYPE_MASK;
			p->state = e->archType;
			p->major = iset.dcount;
			}
		else {
			if (loopDrive == -1) { // create loop drive
				if (iset.dcount >= iset.size && expandDisk(&iset)) { debug(INFO, 0,"expandDisk limit reached.\n"); return; } // too many entries
				loopDrive = iset.dcount;
				d = &iset.drive[iset.dcount++];
				d->deviceLength = d->sectorSize = 1;
				d->diskType = PART_CONTAINER;
				strcat(imageDevice,"prt");
				d->deviceName = "prt"; // should never show up anywhere
				d->diskName = "";
				d->state = 0; d->map = NULL; d->partCount = 0;
				d->major = iset.dcount;
				}
			if (iset.dcount - 1 != loopDrive) return; // loop drive must be last and current entry
			e->length *= e->start; // sector size, since loopDrive has sectorSize == 1 from diverse disks
			e->start = 0;
			e->major = loopDrive+1;
			addImageEntry(e);
			}
		}
	else { // disk definition (including loop)
		if (iset.dcount + 1 != e->major) { debug(INFO, 0,"Out of sequence error.\n"); return; } // out of sequence ERROR
		if (iset.dcount >= iset.size && expandDisk(&iset)) { debug(INFO, 0,"expandDisk out of bounds.\n"); return; } // too many entries
		d = &iset.drive[iset.dcount++];
		d->diskType = e->fsType;
		d->deviceLength = e->length;
		d->sectorSize = e->start;
		nextDeviceName();
		sprintf(globalBuf,"/dev/%s",imageDevice);
		d->deviceName = getStringPtr(&iset.pool,globalBuf);
		// d->deviceName = "IMAGE";
		d->diskLabel = getStringPtr(&iset.pool,e->label);
		d->diskName = "";
		d->state = e->archType;
		d->map = NULL; d->partCount = 0;
		d->major = iset.dcount;
		}


/*
                e.fsType = type; // EXT, DISK_MBR, etc.
                e.archType = state; // COMPRESSION & ARCH_INTERPRETER (dd, MBR, partclone, SWAP), determine by ST_SEL state condition; 0 = do nothing, don't include
                e.major = major;
                e.minor = minor;
                e.length = length;
                e.start = start;
                bzero(e.label,34);
                if (label != NULL) strcpy(e.label,label);
                writeFile(&e,sizeof(imageEntry),arch);
*/




	}

void processImageArchive(char *filename) {
	imageEntry e;
	archive arch;
	int n;
	int len = sizeof(e);
	imageSize = getImageTitle(filename,globalBuf,&arch); // ignore the title
	if (!imageSize) return;
	readableSize(imageSize);
	strcpy(imageSizeString,readable);
// endwin();
	*imageDevice = 0;
	while(((n = readFile(&e,len,&arch,1)) == len) && (!e.label[IMAGELABEL-1])) {
			addImageEntry(&e); // last char in label must be \0
			}
// exit(1);
	if (n != 0) {
		debug(INFO, 0,"Archive index error.\n");
		}

// for (i=0;i<md_len;i++) sprintf(&imageData[i*2],"%02x",md_value[i]);
                        // if (!md_len) strcpy(imageData,"<none>");
	for(n=0;n<4;n++) { sprintf(&signature[n*2],"%02X",((unsigned char *) &arch.timestamp)[n]); }
	signature[8] = 0;  // good 'til 2100 or so
	closeArchive(&arch);
	}

void populateImage(selection *sourceSel, char clearPanel) { // inserts information into image selector
	struct stat64 stats;
	selection *sel = NULL;
	item *curpos = NULL;
	selectedType = 0; // unknown

	if (clearPanel) return;
	if (sourceSel != NULL) {
		sel = sourceSel->attached;
	 	sel->count = sel->top = sel->position = 0;
		resetPool(&sel->pool,0);
		curpos = &(sourceSel->sources[sourceSel->position]);
		if (!*sourceSel->currentPath) selectedType = 5; // disk drive
		else if (!strcmp(currentImage.imagePath,sourceSel->currentPath) && !strcmp(currentImage.imageName,curpos->location)) selectedType = 1; // current image
		}
	if (!setup) {
		iset.dcount = 0;
		loopDrive = -1;
		resetPool(&iset.pool,0);
		}
	if (!selectedType) { // determine full image path
		if (curpos != NULL) sprintf(globalPath,"%s%s",sourceSel->currentPath,curpos->location);
		else if (image1.image[strlen(image1.image)-1] == '/') sprintf(globalPath,"%s%s",image1.image,(image1.path != NULL)?image1.path:"");
		else sprintf(globalPath,"%s/%s",image1.image,(image1.path!= NULL)?image1.path:"");
		imagePath(NULL);
		if (!strncmp(globalPath,"http://",7)) {
			if (curpos != NULL && curpos->state != CLR_GB) return;
#ifdef NETWORK_ENABLED
			*imageDevice = 0;
			if (globalPath[strlen(globalPath)-1] == '/') globalPath[strlen(globalPath)-1] = 0; // clear leading '/'
			iset.dcount = 0; resetPool(&iset.pool,0); loopDrive = -1;
			selectedType = getHTTPInfo(1);
			if ((selectedType == 2) && (setup && sel != NULL)) { populateImageSelector(sel); return; }
#endif
			}
		else if (!stat64(globalPath,&stats)) {
			if (stats.st_mode & S_IFDIR) selectedType = 3;
			else if (stats.st_mode & S_IFREG && (getType(globalPath) == 1)) {
				selectedType = 2; // it's an image
				}
			else selectedType = 4; // regular file
			}
		}
	int i;
	// configure attached selector; leave it blank unless it's an image
	if (selectedType != 1 && selectedType != 2) return; // not an image; nothing to display
	if (selectedType == 2) { // get some info about the image, if possible
		if (setup && sel != NULL) { populateImageSelector(sel); return; }  // don't over-write
	 	// populateSHA1(imagePath(IMG_SHA1));
/* parse .imageindex and populate image selector with 'image dset' structure */
		if (strncmp(globalPath,"http://",7)) processImageArchive(globalPath);
		if (mode == RESTORE) clearBusy(); // clear any previous diskset maps (just in case)
		// scanImageContents(imagePath(""));
/*
		if (loopDrive != -1) { // properly sort the partitions // no longer needed, since they should be sorted in the image itself (not a readdir() even)
		qsort(iset.drive[loopDrive].parts,iset.drive[loopDrive].partCount,sizeof(partition),partitionCompare);
			}
*/
		if (sel != NULL) populateImageSelector(sel);
		}
	return; // empty for now
	}
