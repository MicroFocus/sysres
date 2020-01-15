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

#include "cli.h"
#include "image.h" 			// iset, loopDrive
#include "mount.h"			// currentLine
#include "partition.h"
#include "partutil.h"		//	readable
#include "window.h"			// options
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

#include <ncurses.h>  	// required for 'bool' type

extern char pbuf[];

#define MAXTYPES 7
unsigned char pimage_types[] = PARTIMG;
unsigned char fsarch_types[] = FSARCH;
unsigned char pclone_types[] = PCLONE;
char *types[] = { "empty", "msdos", "vfat", "ntfs", "ext2", "ext3", "ext4", "xfs", "swap", "block" };
char *mapTypes[] = { "", "loop", "partial", "complete", "restore", "incomplete", "custom", "direct", "backup" }; // first entry should remain ""

#define MAXPDEF 80
#define PSIZE 16

char *getStringPtr(poolset *sel, char *str);

void addSelection(selection *sel,char *loc, char *id, char usePtr) { // adds an element to the selector with a blank identifier
	if (sel->count >= sel->size && expandSelection(sel,0)) return; // can't add more
	item *option = &(sel->sources[sel->count++]);
	option->state = 0;
	option->diskNum = -2; // it's a string pointer
	if (strlen(loc) >= MAXPSTR || strlen(id) >= MAXPSTR) debug(EXIT, 1,"Pool string too large.\n");
	option->location = (usePtr)?loc:getStringPtr(&sel->pool,loc);
	if (*id) option->identifier = (usePtr)?id:getStringPtr(&sel->pool,id);
	else option->identifier = ""; // use a constant blank instead
	}

bool isSupported(unsigned char type, char field) { // field 0 = any, 1 = partimage, 2 = fsarch, 3 = partclone
	int i = 0;
	if (!field || field == ST_IMG) {
		while(pimage_types[i] != 0) { if (type == pimage_types[i++]) return true; }
		i=0;
		}
	if (!field || field == ST_CLONE) {
		while(pclone_types[i] != 0) { if (type == pclone_types[i++]) return true; }
		i=0;
		}
	if (!field || field == ST_FILE) {
		while(fsarch_types[i] != 0) { if (type == fsarch_types[i++]) return true; }
		i=0;
		}
	if (!field || (field == ST_SWAP)) { // only one swap partition type, for now
		if (type == PART_SWAP) return true;
		}
	if (field == ST_FULL) return true; // everything works with ST_FULL (dd)
	return false;
	}

// scans through the disksset to populate the attached selector list (not the primary drive list)
// zone == 0, source drives, zone == 1, source images, zone == 2 source/target transfer list
// if selection is NULL, then return zone location, if any
void populateAttached(disk *d, selection *sel, char zone, int dnum) {
	int i, notLoop = 1;
	item *option;
	char *id = "";
	partition *pt;
	int dtype = d->diskType;
	partition *parts = d->parts;
	int pcount = d->partCount;
	char *label = d->diskLabel;
	unsigned char type;
	char t[5];
	bool isImage = false;
	char *readSize;
	if (d == &iset.drive[dnum]) isImage = true;

	if (d->map != NULL) { // display something else
/* THIS MAP COULD BE A PARTITION, AS WELL. */
		parts = d->auxParts;
		if ((d->state & ST_BLOCK) == SI_PART) {
			dtype = ((partition *)d->map)->type;
			if (dtype == PART_RAW) dtype = DISK_RAW;
			label = ((partition *)d->map)->diskLabel;
			pcount = 0;
			}
		// if ((d->state & ST_BLOCK) == SI_MBR || (d->state & ST_BLOCK) == SI_AUTO) {
		else {
			dtype = ((disk *)d->map)->diskType;
			label = ((disk *)d->map)->diskLabel;
			pcount = ((disk *)d->map)->partCount;
			}
		// else pcount = 0; // loop/raw device

		}

	switch(dtype & DISK_MASK) {
		case DISK_RAW: strcpy(pbuf," disk block"); break; // same as EMPTY
		case DISK_MSDOS: strcpy(pbuf,"  mbr msdos"); break;
		case DISK_GPT: strcpy(pbuf,  "  mbr   gpt"); break;
		default: if (isImage && dnum == loopDrive) notLoop = 0;
			else sprintf(pbuf," loop %5s",types[dtype & TYPE_MASK]);
			break;
		}
	if (!zone) { // a list of source drives (no mounting required)
		sel->position = sel->top = 0;
                if (sel->count >= sel->size && expandSelection(sel,0)) return; // can't add any more; sel->sources is full
                if (!sel->count) resetPool(&sel->pool,0);
		if (notLoop) { // add to the list
/* IF IT'S A LOOP SOURCE, THEN ALSO LIST THAT */
			option = &(sel->sources[sel->count++]);
			readableSize(d->sectorSize * d->deviceLength);
			sprintf(&pbuf[strlen(pbuf)]," %7s %s",readable,(dtype & TYPE_DAMAGED)?"damaged":label);
			option->identifier = getStringPtr(&sel->pool,pbuf);
			// option->location = d->deviceName;  // getStringPtr(&sel->pool,d->deviceName) /* source drives don't have partitions */
			option->state = d->state;
			option->diskNum = -1; // map to disk, not part structure
			// option->party = d;
			option->location = d;
			}
		}
	else if (!(d->diskType & DISK_MASK)) { // see if we can mount a loop drive
		if (zone == 1 && !(options & OPT_ANYPART) && (d->diskType != PART_MSDOS) && (d->diskType != PART_VFAT) && strcmp(d->diskLabel,SYSLABEL)) { ; }
		else if (d->diskType == PART_SWAP || d->diskType == PART_EMPTY || d->diskType == PART_RAW) { ; }
		else { // add this to the list (see if list can handle loop drives)
			option = &(sel->sources[sel->count++]);
			option->state = d->state;
			option->diskNum = -1;
			option->location = d;
			sprintf(pbuf,"%s %s",(*d->diskName)?d->diskName:ANONDISK,d->diskLabel);
			option->identifier = getStringPtr(&sel->pool,pbuf);
			}
		}
	// enumerate each partition
	for(i=0;i<pcount;i++) {
		pt = &parts[i];
		if ((pt->flags & FREE) || (pt->num == -1) || !(pt->flags & (PRIMARY | LOGICAL))) continue; // don't display non-partitions
		// if (!isSupported(pt->type,0)) continue; // only display items with the supported format for mounting
		if (sel->count >= sel->size && expandSelection(sel,0)) break; // can't add any more
		option = &(sel->sources[sel->count]);
		// option->location = pt->deviceName; // getStringPtr(&sel->pool,pt->deviceName)
		// option->party = pt;
		option->location = pt;
                // strcpy(option->type,pt->type);
		option->state = 0;
		option->diskNum = dnum; // i;
		if ((zone == 1) && !(options & OPT_ANYPART)) {
			if (pt->type != PART_MSDOS && pt->type != PART_VFAT && strcmp(pt->diskLabel,SYSLABEL)) continue;
			if (!*d->diskName) option->identifier = ANONDISK;
			else option->identifier = getStringPtr(&sel->pool,d->diskName); // should we add the disk label?
			sel->count++;
			return;
			}
		if (zone) { // used for source/target transfer directory
			if (pt->type == PART_SWAP || pt->type == PART_EMPTY || pt->type == PART_RAW) continue;
			sprintf(pbuf,"%s %s",(*d->diskName)?d->diskName:ANONDISK,pt->diskLabel);
			}
		else {
			option->state = pt->state;
			if (pt->map == NULL) { type = pt->type; label = pt->diskLabel; }
			else {
				if ((pt->state & ST_BLOCK) != SI_PART) {  // SI_LOOP, SI_MBR, etc.
					type = ((disk *)pt->map)->diskType;
					if (type & DISK_MASK) type = PART_RAW;
					label = ((disk *)pt->map)->diskLabel;
					}
				else {
					type = ((partition *)pt->map)->type;
					label = ((partition *)pt->map)->diskLabel;
					}

				}
			// if (type != 0 && !*type) { type = "oops"; } // display part type for unknowns
			readableSize(d->sectorSize * pt->length);
			if (notLoop) sprintf(pbuf,"  % 3i %5s %7s %s",pt->num,types[type],readable,label); // types[type]
			else sprintf(pbuf," part %5s %7s %s",types[type],readable,label); // p%03i, pt->num
			if (!type) option->state = CLR_WDIM;
			}
		option->identifier = getStringPtr(&sel->pool,pbuf);
		sel->count++;
		}
	}

/* setZone: 0 == LIST_DRIVE, 1 == LIST_IMAGE, 2 == LIST_SOURCE/LIST_TARGET */
void populateDisks(diskset *dset, selection *sel, char setZone) {
        int i;
        disk *d;
	partition *pt;
	sel->position = sel->top = sel->count = 0;
	resetPool(&sel->pool,0);
        for (i=0;i<dset->dcount; i++) {
                d = &(dset->drive[i]);
		if (sel->count >= sel->size && expandSelection(sel,0)) return; // can't add any more; sel->sources is full
                // sel->sources[sel->count].diskNum = i;
                // if (setZone <= 1) strcpy(identifier,d->diskName); // LIST_DRIVE/LIST_IMAGE
		if (setZone) populateAttached(d,sel,setZone,i); // LIST_IMAGE/LIST_SOURCE/LIST_TARGET
		else { // LIST_DRIVE
			sel->sources[sel->count].diskNum = i;
			sel->sources[sel->count].identifier = (*d->diskName)?d->diskName:ANONDISK; // getStringPtr(&sel->pool,d->diskName)
			// sel->sources[i].location = d->deviceName; // getStringPtr(&sel->pool,d->deviceName) (can't be disk*; would break local mounts and frame trimLine)
			// sel->sources[i].party = d;
			sel->sources[sel->count].location = d;
			sel->count++;
			}
                }
        }

bool existingMount(selection *sel,char *name) {
#ifdef NETWORK_ENABLED
	int i, len;
	for (i=0;i<sel->count;i++) {
		len = strlen(sel->sources[i].location); // does not end in a '/'
		if (!strncmp(name,sel->sources[i].location,len)) {
			if (name[len] == 0 || name[len] == '/') return true;
			}
		}
#endif
	if ((options & OPT_RAMDISK) && (!strncmp(name,"/mnt/ram",8) && (name[8] == 0 || name[8] == '/'))) return true;
	return false;
	}

void populateList(selection *sel) {
        int findMount = 0;
        if (sel == NULL) {
                populateList(&sels[LIST_IMAGE]);
                populateList(&sels[LIST_DRIVE]);
                populateList(&sels[LIST_SOURCE]);
                populateList(&sels[LIST_TARGET]);
                return;
                }
	if (sel->currentPathStart) return; // needs to be 0
        sel->count = 0;
	sel->attached->count = 0;
	resetPool(&sel->pool,0);
        if (sel == &sels[LIST_IMAGE]) findMount = 1;
        else if (sel != &sels[LIST_DRIVE]) findMount = 2; // direct i/o file transfer option
        populateDisks(&dset,sel,findMount);
	if (findMount) {
#ifdef NETWORK_ENABLED
                if ((sel == &sels[LIST_IMAGE]) || (sel == &sels[LIST_SOURCE]) || (sel == &sels[LIST_TARGET])) addNetworkMount(sel,NULL);
#endif
		if (*image2.image == -1) {
			if (sel == &sels[LIST_IMAGE] && *image1.image && image1.local && !existingMount(sel,image1.image)) addSelection(sel,image1.image,LOCALFS,1); // needs to be provided on command-line
			}
		else {
			if (sel == &sels[LIST_SOURCE] && *image1.image && image1.local && !existingMount(sel,image1.image)) addSelection(sel,image1.image,LOCALFS,1);
			else if (sel == &sels[LIST_TARGET] && *image2.image && image2.local && !existingMount(sel,image2.image)) addSelection(sel,image2.image,LOCALFS,1);
			}
		}
        if (findMount && (options & OPT_RAMDISK) && verifyMount("tmpfs","/mnt/ram","rw")) addSelection(sel,"/ram",RAMFS,1);  // make sure RAMdisk is still available
        }

long availRAM(void) {
	if (openFile("/proc/meminfo")) return 0;
	while(readFileLine()) {
		if (!strncmp(currentLine,"MemTotal: ",10)) { closeFile(); return atol(&currentLine[9]); }
		}
	return 0;
	}

int matchDevice(char *dev, char *match) { // sees if /dev/root, etc., can match /dev/sd...
	struct stat64 sbuf;
	dev_t node;
	if (!strcmp(dev,match)) return 1;
	if (!stat64(dev,&sbuf)) {
		node = sbuf.st_rdev;
		if (!stat64(match,&sbuf) && (sbuf.st_rdev == node)) return 1;
		}
	return 0;
	}

// check if the RAM disk is mounted; if not, create and mount
char hasRAM(void) {
	// /proc/mounts









	return 0;
	}


