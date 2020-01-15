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
#include <errno.h>

#include "drive.h"			// types
#include "partition.h"
#include "window.h"     // globalPath, options
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

extern char localmount;
char globalBuf[BUFSIZE];

char trackMount = 0; // set if a unique mount is being added; remove when exiting

#define MAXTYPES 7

#define MAXPDEF 80
#define PSIZE 16

int globalFile = -1;
char globalOverflow = 0;
char *currentLine;
char *line;

void closeFile(void) { if (globalFile != -1) { close(globalFile); globalFile = -1; } }

char openFile(char *path) {
        line = globalBuf; *line = 0; globalOverflow = 0;
	closeFile();
        if ((globalFile = open(path,O_RDONLY | O_LARGEFILE)) < 0) return 1;
        return 0;
        }

char readFileLine() {
        char *next;
        int overflow = 0;
        int size,index;
        currentLine = line;
        if ((next = strchr(line,'\n')) != NULL) { *next = 0; line = ++next; return 1; } // more lines to flush
        if ((index = strlen(line)) != 0) memmove(globalBuf,line,index); // move remainder to beginning of buf
	currentLine = globalBuf; // reset line
        while((size = read(globalFile,&globalBuf[index],BUFSIZE-index-1)) > 0) {
                index += size;
                globalBuf[index] = 0;
                if ((next = strchr(globalBuf,'\n')) == NULL) {
                        if (index >= BUFSIZE-1) { globalOverflow = overflow = 1; index = 0; }
                        continue;
                        }
                *next = 0;
                line = ++next;
                if (overflow) { // discard this line
                        if ((index = strlen(line)) != 0) memmove(globalBuf,line,index);
                        overflow = 0;
                        continue;
                        }
                return 1;
                }
        close(globalFile);
        globalFile = -1;
        return 0;
        }

char verifyMount(char *dev, char *mountPoint, char *permission) {
	if (mountPoint == NULL && dev == NULL) return 0; // must have at least one
	char pmatch[PSIZE];
        if (permission != NULL) {
                if (strlen(permission) > PSIZE - 3) permission = NULL;
                else {
                        pmatch[0] = ',';
                        strcpy(&pmatch[1],permission);
                        pmatch[strlen(permission)+1] = ',';
                        pmatch[strlen(permission)+2] = 0;
                        }
                }
	char *device, *mpoint, *perms;
	if (openFile("/proc/mounts")) return 0; // implies /proc isn't mounted
	while(readFileLine()) {
		device = mpoint = perms = NULL;
		if ( ((device = strtok(currentLine," ")) != NULL) &&
		     ((mpoint = strtok(NULL," ")) != NULL) &&
		     (strtok(NULL," ") != NULL) && // filesystem type ignored
		     ((perms = strtok(NULL," ")) != NULL) ) {
				if (((dev == NULL) || matchDevice(device,dev)) && ((mountPoint == NULL) || !strcmp(mountPoint,mpoint))) { // now check for specific permission, if required
					closeFile();
					if (permission == NULL) return 1;
					if (strlen(perms) >= strlen(permission)) {
						if (!strcmp(permission,perms) || // only
						    !strncmp(perms,&pmatch[1],strlen(permission)+1) || // leading
						    !strncmp(&perms[strlen(perms)-strlen(permission)-1],pmatch,strlen(permission)+1) || // trailing
						    strstr(perms,pmatch) != NULL) return 1;
						}
					return 0;
					} // specific permission
			}
		} // end while
	return 0;
	}

// this just sees if a device is mounted somewhere
char verifyQuickMount(char *dev) {
	if (dev == NULL) return 0;
	if (openFile("/proc/mounts")) return 0;
	while(readFileLine()) {
		if (strtok(currentLine," ") == NULL) continue;
		if (matchDevice(currentLine,dev)) { closeFile(); return 1; }
		}
	return 0;
	}

char verifyItemMount(item *itm) {
	if (itm->state == ST_NOSEL) return 0;
	return verifyQuickMount((itm->diskNum == -1)?((disk *)itm->location)->deviceName:((partition *)itm->location)->deviceName);
	}

int validateMountDirectory(char *path) {
        struct stat64 z;
        if (stat64(path,&z)) {  // doesn't exist
                mkdir(path,0); // S_IWUSR | S_IRUSR | S_IXUSR);
                if (stat64(path,&z)) return 0; // doesn't exist
                }
        if (S_ISDIR(z.st_mode)) return 1;
        return 0;
        }

// selector set, then *s is a selector; otherwise it's a string; selector implies selector is populated with mount info
int mountLocation(void *s, unsigned char type, unsigned char special) {
	unsigned char *loc;
	char *path = globalPath;
        int mnt;
	selection *sel = NULL;
	item *itm = NULL;
	void *p = NULL;
	if (!type) {
		sel = s;
        	if (sel == NULL || *sel->currentPath) return 0; // must be unmounted
		itm = &sel->sources[sel->position];
		if (itm->diskNum == -1) { p = itm->location; type = ((disk *)p)->diskType; loc = ((disk *)p)->deviceName; }
		else if (itm->diskNum != -2) { p = itm->location; type = ((partition *)p)->type; loc = ((partition *)p)->deviceName; }
		else loc = itm->location;  // must be a local mount w/o a type requirement
		}
	else loc = s;
	strcpy(path,"/mnt/");
        if (!strcmp(loc,"/ram")) {
                if (verifyMount("tmpfs","/mnt/ram","rw")) { navigateDirectory("/mnt/ram",sel); return 1; }
		return 0;
                }
	else if (strncmp(loc,"/dev/",5)) { // local mount; just traverse if it exists
		navigateDirectory(loc,sel);
                if (sel == &sels[LIST_IMAGE]) localmount |= 1;
                else if (sel == &sels[LIST_SOURCE]) localmount |= 2;
                else if (sel == &sels[LIST_TARGET]) localmount |= 4;
                return 1;
		}
        else if (validateMountDirectory(path)) { // make sure /mnt/ exists, then go further
                if (!strncmp(loc,"/net/",5)) return 0; // net; don't handle this yet
		if (special == 1) strcat(path,"pre"); // preScript
		else if (special == 2) strcat(path,"post"); // postScript
		else strcat(path,&loc[5]);
                if (validateMountDirectory(path)) { // mount and navigate here; assume single-depth path for now; otherwise modify validateMountDirectory()?
                        if (verifyMount(loc,path,"rw")) { navigateDirectory(path,sel); return 2; } // already mounted
                        if (verifyMount(loc,path,NULL)) return 0; // mounted already, incorrectly
                        if (mount(loc,path,types[type],0,NULL)) return 0; // mount error
			if (sel == &sels[LIST_IMAGE] && p != NULL) { currentImage.mount = p; currentImage.isDisk = (itm->diskNum == -1)?1:0; } // keep track of system restore mount
			if (sel == s) trackMount |= (sel == &sels[LIST_IMAGE])?SEL_IMAGE:(sel == &sels[LIST_SOURCE])?SEL_SOURCE:SEL_TARGET;
			else trackMount |= 8;  // local mount
                        if (verifyMount(loc,path,"rw")) { navigateDirectory(path,sel); return 1; }
                        return 0;
                        }
                }
        }

int unmountLocation(char *path) {
        int count = 0;
        while(verifyMount(NULL,path,NULL) && count < 10) {
                if (count) sleep(1);
                umount(path);
                count++;
                }
	return (count == 10)?1:0;
        }

void mountDefaults(void) {
        int mnt;
        if (!verifyMount("proc","/proc","rw") && !verifyMount("/proc","/proc","rw")) {
                mnt = mount("proc","/proc","proc",0,NULL);
                if (mnt) { debug(INFO, 1,"mountDefaults proc %s\n",strerror(mnt)); }
                }
        if (!verifyMount("sys","/sys","rw") && !verifyMount("/sys","/sys","rw") && !verifyMount("sysfs","/sys","rw")) {
                mnt = mount("sys","/sys","sysfs",0,NULL);
                if (mnt) { debug(INFO, 1,"mountDefaults sys %s\n",strerror(mnt)); }
                }
	if (!verifyMount("tmpfs","/dev/shm","rw") && !verifyMount("/tmpfs","/dev/shm","rw")) { // will only work if /dev/shm exists
		mnt = mount("tmpfs","/dev/shm","tmpfs",0,NULL);
		if (mnt) { debug(INFO, 1,"mountDefaults shm %s\n",strerror(mnt)); }
		}

        /* mount RAM disk, as appropriate */
        if (verifyMount("tmpfs","/mnt/ram","rw")) { ; } // already there; include it if so set
        else if (verifyMount("tmpfs","/mnt/ram",NULL)) { options &= ~OPT_RAMDISK; } // don't include; it's not 'rw'
        else if (options & OPT_RAMDISK) { // mount the RAM disk, at 75% RAM space
        if (validateMountDirectory("/mnt") && validateMountDirectory("/mnt/ram")) {
                if (mount("tmpfs","/mnt/ram","tmpfs",0,"size=75%")) options &= ~OPT_RAMDISK;
                } else options &= ~OPT_RAMDISK;
        }
}
