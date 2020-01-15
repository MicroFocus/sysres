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
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h> // used for 'bool' type
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "cli.h"					// validOperation
#include "drive.h"				// types, mapTypes
#include "fileEngine.h" 	// readSpecificFile(), fileBuf
#include "image.h" 				// iset
#include "mount.h"				// globalBuf
#include "restore.h"    	// locateImage()
#include "partition.h"
#include "window.h"     	// globalPath, mode
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

#define MAXLEN 256
#define DEVLEN 100 	// /sys/block/#1/#2/...

#define SKIPCD

// #define DIVIDER "==============================================================================\n"


extern int filterCount;
extern char drive_filter[MAXDISK][MAXDRIVES];
extern bool validMap;

extern char signature[];

char *getStringPtr(poolset *sel, char *str);

char readable[10];

extern char show_list;

extern unsigned char sha1buf[];

#include <signal.h>
volatile sig_atomic_t has_interrupted;

unsigned long getImageTitle(char *filename, char *buf, archive *arch);

/* MBR spec:
	446 bytes
	64 bytes = partition table
	2 bytes = mbr signature 55aa

  GUID partition spec:
	LBA0 = legacy MBR
	LBA1-LBA33 (33x512 at this time) = GPT
	end-LBA34 (33x512 at this time) = secondary header

        status "Clearing $dn partition table"
        dd if=/dev/zero of=$dn bs=1024 count=1
        gptbase=`echo ${dn:5} | sed "s>/>\\!>"`
        gptend=`cat /sys/block/$gptbase/size | xargs -iX expr X - 1`
        dd if=/dev/zero of=$dn bs=512 count=1 seek=$gptend
        parted $dn print -s >/dev/null 2>&1
        if [ "$?" = "0" ]; then showErr "Unable to clear $dn partition table"; fi


When writing an MBR, the partitions cannot interfere with ANY MOUNTED partitions. i.e., if any partitions are mounted the resulting partition number, type, flags and start/size/sectorSize must be identical.
All other partitions can be changed.

Only the last partition in a partition set can be dynamic(?) unless we start using makePart code.

*/

/*

 Nr.  TYPE  START SIZE  LABEL
 mbr  msdos 0	   2.5TB
 1*    vfat  39293 230MB DATA PARTITION
 2     ext3
 3     ext3
 --------------------------------------
 5+    ntfs
 6+    swap

*/

// (E?)# ext3 start size 'label'
//     free


#define EXT2_SUPER_MAGIC 0xEF53
#define VOLNAMSZ 17

typedef struct ext2_super_block {
        u_char  nop1[56];
        u_char  magic[2];
        u_char  nop2[34];
	u_char  extended[12]; // 0x45C
        u_char  uuid[16]; // 0x468
        u_char  volname[16]; //
} ext2;

#define SWAP_SUPER_MAGIC "SWAPSPACE2"
typedef struct swap_super_block {
        char          nop1[1024];    /* Space for disklabel etc. */
        unsigned int  version;
        unsigned int  nop2[2];
        unsigned char uuid[16];
        char          volname[16];
} swap;

#define XFSLSIZE 12
#define XFS_SUPER_MAGIC "XFSB"
typedef struct xfs_super_block {
    u_char    magic[4];
    u_char    nop1[28];
    u_char    s_uuid[16];
    u_char    nop2[60];
    u_char    volname[12];
} xfs;

#define VFATLSIZE 11
#define VFAT_SUPER_MAGIC "MSDOS5.0"
typedef struct vfat_super_block {
    u_char     nop[3];
    u_char     magic[8];
    u_char     nop2[60];
    u_char     volname[11];
    u_char     type[8];  // FAT16, FAT32
    u_char     nop3[420];
    u_char     term[2];  // 0x55AA
} vfat;


extern char *labelBuf;

char *readableTime(unsigned int elapsed, char bufLoc) {
	unsigned char *buf = &globalBuf[bufLoc*10];
        unsigned int s = elapsed % 60;
        unsigned int m = (elapsed / 60) % 60;
        unsigned int h = elapsed / 3600;
	if (h >= 1000) strcpy(buf,"unknown"); // 41 days is a long time
        else if (h) sprintf(buf,"%i:%02i:%02i",h,m,s);
        else sprintf(buf,"%i:%02i",m,s);
	return buf;
        }

void readableSize(unsigned long val) {
        float divisor = 1.0;
	char delim = ' ';
        if (val >= 1099511627776) { delim = 'T'; divisor = 1099511627776.0; }
        else if (val >= 1073741824) { delim = 'G'; divisor = 1073741824.0; }
        else if (val >= 1048576) { delim = 'M'; divisor = 1048576.0; }
        else if (val >= 1024) { delim = 'K'; divisor = 1024.0; }
	divisor = val/divisor;
	if (divisor >= 1000000.0) strcpy(readable,"large");
	else if (divisor >= 1000.0) sprintf(readable,"%.f%cB",divisor,delim);
	else if (delim == ' ') sprintf(readable,"%.fB ",divisor);
	else sprintf(readable,"%.1f%cB",divisor,delim);
	// if (strlen(readable) > 10) res[10] = 0;  // limit it to 10 characters
	readable[9] = 0;
        }

int driveNameCompare(const void *a, const void *b) {
        const disk *ia = (const disk *)a;
        const disk *ib = (const disk *)b;
        return (strcmp(ia->deviceName,ib->deviceName));
        }

int partitionStartCompare(const void *a, const void *b) {
	const partition *ia = (const partition *)a;
	const partition *ib = (const partition *)b;
	if (ia->start < ib->start) return -1;
	else return (ia->start > ib->start);
	}

unsigned char findLabel(char *devName, char **label) {
	char tmp[VOLNAMSZ];
	int n = getpagesize(); // typically 4096
	*label = "";
	unsigned char type = PART_RAW;
	if (n < sizeof(swap)) return type;
	if (labelBuf == NULL) return type;  // in case malloc didn't work
	swap *s = (swap *)labelBuf;
	ext2 *e = (ext2 *)&labelBuf[1024];
	xfs *x = (xfs *)labelBuf;
	vfat *vf = (vfat *)labelBuf;
	int fd = open(devName,O_RDONLY | O_LARGEFILE);
	if (fd < 0) return type;
	if (read(fd,(char *)labelBuf,n) != n) { close(fd); debug(INFO,1,"No label read for %s (ext'd?).\n",devName); return type; }
	close(fd);
	if (!strncmp(vf->type,"FAT32   ",8) && vf->term[0] == 0x55 && vf->term[1] == 0xAA) {  // vf->magic == MSDOS5.0, mkdosfs
		strncpy(tmp,vf->volname,VFATLSIZE);
		tmp[VFATLSIZE] = 0;
		// if (!strcmp("NO NAME    ",tmp)) *tmp = 0;
		type = PART_VFAT;
		}
	else if (!strncmp(vf->magic,"NTFS    ",8)) {
		tmp[0] = 0; // skip label for now; actual $Volume is further out
		type = PART_NTFS;
		}
	else if (e->magic[0] + 256*e->magic[1] == EXT2_SUPER_MAGIC) {
		strncpy(tmp,e->volname,VOLNAMSZ-1);
		if ((e->extended[0] + 256*e->extended[1]) &  4) {  // has journal
                        if (((e->extended[4] + 256*e->extended[5]) < 0x40) && ((e->extended[8] + 256*e->extended[9]) < 8)) type = PART_EXT3; // small INCOMPAT & RO_COMPAT
			else type = PART_EXT4;
                        }
		else type = PART_EXT2;
		}
	else if (!strncmp(labelBuf+n-10,SWAP_SUPER_MAGIC,10) && s->version == 1) { strncpy(tmp,s->volname,VOLNAMSZ-1); type = PART_SWAP; } // swap
	else if (!strncmp(x->magic,XFS_SUPER_MAGIC,4)) { strncpy(tmp,x->volname,XFSLSIZE); type = PART_XFS; tmp[XFSLSIZE] = 0; } // xfs
	else return type;
// /dev/disk/by-label => does not appear to be present in RHEL 6.2, so we can't use that in addition to the block approach
	tmp[VOLNAMSZ-1] = 0;
	while(strlen(tmp) && tmp[strlen(tmp)-1] == ' ') tmp[strlen(tmp)-1] = 0;
	*label = getStringPtr(&dset.pool,tmp);
	return type;
	}

bool quickRead(char *path, char *buf, int max) {
	int n, size = 0;
	int file = open(path,O_RDONLY | O_LARGEFILE);
	max--;
	if (file < 0) return false;
	while((n = read(file,&buf[size],max-size)) > 0 && size < max) size+=n;
	close(file);
	while(size && (buf[size-1] == '\n' || buf[size-1] == ' ')) size--;
	buf[size] = 0;
	return (*buf)?true:false; // empty
	}

/* we don't need to compensate for cciss!... names, as later kernels map this to scsi letters */
int makeNode(char *path, char *node) {
	char buf[16];
	struct stat64 statBuf;
	// dev_t dev;
	int file;
	int maj, min;
	sprintf(globalBuf,"%s/dev",path);
	if (!quickRead(globalBuf,buf,16)) return 0;
	if (strchr(buf,'\n') != NULL) *(strchr(buf,'\n')) = 0;
	if (strchr(buf,':') != NULL) min = atoi(strchr(buf,':')+1);
	else return 0;
	maj = atoi(buf);
	sprintf(globalBuf,"/dev/%s",node);
	if (stat64(globalBuf,&statBuf)) { // create a device node here
		if (mknod(globalBuf,S_IFBLK | S_IRUSR | S_IWUSR ,MKDEV(maj,min))) return 0;
		}
	if ((file = open(globalBuf,O_RDONLY | O_LARGEFILE)) < 0) return 0; // can't read it
	close(file);
	return 1;
	}

int checkFilter(char *name) { // filter out drives we don't want
	int i;
	if (!filterCount) return 0;
	for (i=0;i<filterCount;i++) {
		if (!strcmp(drive_filter[i],name)) return 0;
		}
	return 1;
	}

char numberSuffix(char *dev) {
	while(*dev) {
		if (*dev < '0' || *dev++ > '9') return 0;
		}
	return 1;
	}

char scanSingleDrive(char *path, char *node, disk *d) {
	struct dirent *d2ent;
	DIR *pdir;
	// d is the original drive. Set some values; see 'readDrive' for the ones. We won't have to free it afterwards since it's part of  the same dset
	if ((pdir = opendir(path)) == NULL) return 1; // path == /sys/block/sd...
	while((d2ent = readdir(pdir)) != NULL) {
		if (!strcmp(d2ent->d_name,".") || !strcmp(d2ent->d_name,"..")) continue;
		if (strlen(d2ent->d_name) > DEVLEN) continue; // ignore larger device names
		sprintf(path,"/sys/block/%s/%s",node,d2ent->d_name);
		if (!makeNode(path,d2ent->d_name)) continue; // no readable dev file
		appendPartition(d,path,d2ent->d_name);
		}
	closedir(pdir);
	return 0;
	}

int syncTable(disk *d) {
	int i, eremoved = 0;
	for (i=0;i<d->partCount;i++) {
               if (d->parts[i].flags == FREE) { debug(INFO,1,"Kernel table mismatch on %s\n",d->deviceName); return 1; }  // could cause issues; display warning (shouldn't happen)
               if (d->parts[i].flags == EXTENDED) { // remove extended partition
                      if (eremoved) { debug(INFO,1,"Multiple extended partitions in table on %s\n",d->deviceName); return 1; }
                      else debug(INFO,1,"Ignoring extended partition from %s\n",d->deviceName);
                      if (i+1 != d->partCount) {
                             memmove(&d->parts[i],&d->parts[d->partCount-1],sizeof(partition));
                             eremoved = 1;
                             if (d->parts[i].flags == EXTENDED) { debug(INFO,1,"Multiple extended partitions in table on %s\n",d->deviceName); return 1; }  // could cause issues; display warning (shouldn't happen)
                             }
                      d->partCount--;
                      }
               }
	qsort(d->parts,d->partCount,sizeof(partition),partitionStartCompare);  // sort based on start sector
	return 0;
	}

void scanDrives(void) {
	char path[MAXLEN];
	struct stat64 statBuf;
	DIR *pdir, *dir = opendir("/sys/block");
	struct dirent *dent, *d2ent;
	if (dir == NULL) return;
	dset.dcount = 0;
	disk *d;
	resetPool(&dset.pool,0);
	while((dent = readdir(dir)) != NULL) {
		if (!strcmp(dent->d_name,".") || !strcmp(dent->d_name,"..")) continue;
		if (strlen(dent->d_name) >= DEVLEN) continue;  // ignore larger device names
#ifdef SKIPCD
		if (!strncmp(dent->d_name,"sr",2) && numberSuffix(&dent->d_name[2])) continue;
		if (!strncmp(dent->d_name,"loop",4) && numberSuffix(&dent->d_name[4])) continue;
		if (!strncmp(dent->d_name,"ram",3) && numberSuffix(&dent->d_name[3])) continue;
#endif
		sprintf(path,"/sys/block/%s/queue",dent->d_name);
		if (stat64(path,&statBuf)) continue; // no queue file
		sprintf(path,"/sys/block/%s",dent->d_name);
		if (checkFilter(dent->d_name)) continue;
		debug(INFO,5,"Scanning disk: %s\n",dent->d_name);
		if (!makeNode(path,dent->d_name)) continue;
		if (dset.dcount >= dset.size && expandDisk(&dset)) break; // can't allocate more disks
		d = &dset.drive[dset.dcount];
		if (!readDrive(path,dent->d_name,d)) continue;
		dset.dcount++;
		/* see if block device is readable; if not, don't bother */
		if (scanSingleDrive(path,dent->d_name,d)) continue;
		if (!readFromDisk(d,0,0,NULL,0)) checkIfLoop(d);
		if (syncTable(d)) {
			debug(INFO,1,"Table sync issue with %s\n",d->deviceName);
			d->diskType = DISK_RAW;
			d->partCount = 0;
			} // partition table error/mismatch
		}
	closedir(dir);
	qsort(dset.drive,dset.dcount,sizeof(disk),driveNameCompare); // sort /proc names alphabetically
	return;
	}

// check even GPT disks to make sure they don't contain any mounts
char diskMounted(disk *d) {
	struct stat64 statBuf;
	char path[MAXLEN];
	DIR *pdir;
	struct dirent *dent;
	char *dev = &d->deviceName[5];
	int len = strlen(dev) + 40;
	if (verifyQuickMount(d->deviceName)) return 1;
	sprintf(path,"/sys/block/%s",dev);
	if ((pdir = opendir(path)) == NULL) return 0; // shouldn't happen
	while((dent = readdir(pdir)) != NULL) {
	        if (!strcmp(dent->d_name,".") || !strcmp(dent->d_name,"..")) continue;
                if (strlen(dent->d_name)+len >= MAXLEN) continue; // ignore larger device names
		sprintf(path,"/sys/block/%s/%s/dev",dev,dent->d_name);
		if (stat64(path,&statBuf)) continue; // no dev file
		sprintf(path,"/dev/%s",dent->d_name);
		if (verifyQuickMount(path)) { closedir(pdir); return 1; }
		}
	closedir(pdir);
	return 0;
	}

void remapDrive(disk *t, disk *d) {
	if ((d->state & ST_BLOCK) == SI_PART) {
		memcpy(t,d,sizeof(disk));
		t->diskType = ((partition *)d->map)->type;
		if (t->diskType == PART_RAW) t->diskType = DISK_RAW;
		t->diskLabel = ((partition *)d->map)->diskLabel;
		t->state = ((partition *)d->map)->state;
		t->partCount = 0;
		}
	else memcpy(t,d->map,sizeof(disk));
	t->deviceName = d->deviceName;
	t->sectorSize = d->sectorSize;
	t->deviceLength = d->deviceLength;
	t->parts = d->auxParts;
	t->diskName = d->diskName;
	}

void remapPartition(partition *t, partition *p) {
	if ((p->state & ST_BLOCK) == SI_LOOP) {
		memcpy(t,p,sizeof(partition));
		t->type = ((disk *)p->map)->diskType;
		if (t->type & DISK_MASK) t->type = PART_RAW;
		t->diskLabel = ((disk *)p->map)->diskLabel;
		t->state = ((disk *)p->map)->state;
		t->deviceName = ((disk *)p->map)->deviceName;
		}
	else memcpy(t,p->map,sizeof(partition));
	t->start = p->start;
	t->length = p->length;
	t->num = p->num;
	}

bool readPercentages(archive *arch, char *format, int major, int minor, int imageSet) {
	float percentage = 0.0;
	char r2[10];
	int i, n;
	if (readSpecificFile(arch, major, minor, 1) == 1) {
		if (show_list & 4) { // verify the file sha1sum
			// progressBar(arch->fileSizePosition,PROGRESS_LIGHT,PROGRESS_INIT);
			progressBar(arch->expectedOriginalBytes,PROGRESS_LIGHT,PROGRESS_INIT);
			if (show_list & 1) { // only show expected sums (verify list)
				n = readSignature(arch,0);
				for (i=0;i<20;i++) sprintf(&globalBuf[i << 1],"%02X",fileBuf[i+4]);
				}
			else {
				bzero(sha1buf,24);
				globalBuf[40] = 0;
                		while((n = readFile(fileBuf,FBUFSIZE,arch,1)) > 0) {
					// progressBar(arch->fileBytes,arch->originalBytes,PROGRESS_UPDATE);
					if (progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_UPDATE)) return true;
					}
				printf("\r\033[?25h\033[K");
				for (i=0;i<20;i++) sprintf(&globalBuf[i << 1],"%02X",sha1buf[i+4]);
				}
#ifdef NETWORK_ENABLED
	if (has_interrupted) {
		progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_UPDATE);
		return true;
		}
#endif
			if (n) { strcat(globalBuf," \033[31mFAILED\033[0m"); validOperation = 0; } // no signature found, for list or detail
			else if (show_list & 1) strcat(globalBuf,"  \033[33mFOUND\033[0m");
			else strcat(globalBuf,"     \033[32mOK\033[0m");
			}
		else {
                	readableSize(arch->expectedOriginalBytes);
                	strcpy(r2,readable);
                	readableSize(arch->fileSizePosition);
                	if (arch->expectedOriginalBytes) percentage = ((100.0 * arch->fileSizePosition)/arch->expectedOriginalBytes);
                	percentage = (percentage > 100.0)?0.0:100.0-percentage;
                	if (percentage > 99.9) percentage = 99.9;
                	sprintf(globalBuf,format,r2,readable,((arch->state & COMPRESSED) == GZIP)?"zlib ":((arch->state & COMPRESSED) == LZMA)?"lzma ":"none ", percentage);
			}
		}
	else if (show_list & 4) {
		if (minor) sprintf(globalBuf,"%40s      -","unavailable");
		else strcpy(globalBuf,"unavailable");
		}
	else sprintf(globalBuf,format,"-","-","",0.0);
	if (imageSet) sprintf(&globalBuf[strlen(globalBuf)]," img %i",imageSet);
	return false;
	}

void showDisk(disk *d, char imageSet, archive *arch) {
        int i;
	char *tname;
	char *map, flag[16]; // max size of 'incomplete'
	char isMounted = 0;
	char reloadCondition = 0;
	unsigned char fieldEntry;
	unsigned int percentage;
	partition *parts = d->parts;
	if (imageSet) sprintf(globalBuf,"img %i",imageSet);
	i = determineMapType(d,&map);
	sprintf(flag,"%s%s",(restrictedMap(i))?"\033[31m":"",mapTypes[i]);

	disk t;
	partition tp;
	int tmp = imageSet;
	if (d->map != NULL) { // restore
		reloadCondition = hasMountMismatch(NULL,d);
		remapDrive(&t,d);
		d = &t;
		}
	switch(d->diskType & DISK_MASK) {
                case DISK_MSDOS: tname="msdos"; break;
                case DISK_GPT: tname=  "gpt"; break;
                case DISK_RAW: tname = "empty"; break;
                default: tname="loop "; break;
                }
	if ((d->diskType & TYPE_MASK) != PART_CONTAINER) {
		if (mode == -1) isMounted = verifyQuickMount(d->deviceName);
		else if (!imageSet) isMounted = ((d->state & CLR_MASK) == CLR_RB);
		if (arch != NULL) { if (readPercentages(arch,"[%s/%s/%s%.1f%%]",imageSet,0,imageSet)) exit(1); }
		printf("\033[32;1m%c\033[0m ",mapField(d->state & ST_BLOCK));
		readableSize(d->sectorSize * d->deviceLength);
		printf("%s%s %s%s%s %s ",(isMounted)?"\033[31m":"",&d->deviceName[5],tname,(d->diskType & TYPE_DAMAGED)?"/damaged":"",(!(d->diskType & DISK_MASK))?types[d->diskType]:"",readable);

		if (imageSet && (show_list & 4)) {
			printf("%s\n",globalBuf);
			}
		else {
			if (d->diskLabel != NULL && *d->diskLabel) printf("%s ",d->diskLabel);
			printf("%lu/%lu %s \033[32m%s%s %s\033[0m\n",d->deviceLength,d->sectorSize,(imageSet)?globalBuf:d->diskName,flag,(reloadCondition & 8)?" reload":"",(map != NULL)?&map[5]:""); // d->physSectorSize
			}
		}
	else { tmp = 0; printf("  prt independent partition%s img 0\n",(d->partCount > 1)?"s":""); } // fix this to accomodate image lists, as well
	if (d->partCount) {
		if (arch != NULL)  {
			if (show_list & 4) printf("\033[4m   #  type %8s %40s %6s\033[0m\n","size","sha1sum","status");
			else printf("\033[4m   #  type %8s %12s %12s %12s  dev label    \033[0m\n","size","filesize","stored","compression");
			}
		else printf("\033[4m   #  type %8s %12s %12s %12s  dev label    \033[0m\n","size","start","end","length");
		}
        for (i=0;i<d->partCount;i++) {
                partition *pt = &d->parts[i];
		if ((pt->flags & FREE) || (pt->num == -1) || !(pt->flags & (PRIMARY | LOGICAL))) continue;
		if (mode == -1) isMounted = verifyQuickMount(pt->deviceName);
//		else if (!imageSet) isMounted = (pt->map == NULL)?((pt->state & CLR_MASK) == CLR_RB):0; // if it's mapped, then it isn't mounted
		else if (!imageSet) isMounted = (pt->map == NULL)?verifyQuickMount(pt->deviceName):0;

		if (pt->map != NULL) { remapPartition(&tp, pt); pt = &tp; }
		if (((d->state & ST_GROUPED) == ST_GROUPED) && pt->map == NULL && !pt->state && pt->type) {
			fieldEntry = ((d->state & ST_BLOCK) == ST_MAX)?ST_FULL:nextAvailable(0,pt->type,0);
			}
		else fieldEntry = pt->state;
		if (!pt->type && (fieldEntry & ST_BLOCK)) fieldEntry = 0; // fix an issue on reporting maps
		if (arch != NULL) { if (readPercentages(arch,"%12s %12s   %5s%4.1f%%",tmp,pt->num,0)) exit(1); }
		printf("\033[32;1m%c\033[0m%s",mapField(fieldEntry & ST_BLOCK),(isMounted)?"\033[31m":""); // see if it's empty map; if so, then don't highlight anything
		if (pt->flags & BOOTABLE) { printf("%s",(pt->num < 10)?"  ":(pt->num < 100)?" ":""); printf("\033[4m%i\033[0m%s ",pt->num,(isMounted)?"\033[31m":""); }
		else printf("%3i ",pt->num);
		readableSize(d->sectorSize * pt->length);
		if (arch != NULL) printf("%5s %8s %s",types[pt->type],readable,globalBuf);
		else printf("%5s %8s %12lu %12lu %12lu",types[pt->type],readable,pt->start,pt->start+pt->length-1,pt->length);
                // if (!(((pt->flags & 0x0F) == PRIMARY) || ((pt->flags & 0x0F) == LOGICAL))) { printf("%s\n",(isMounted)?"\033[0m":""); continue; }
		printf(" %4s %s%s\n",(imageSet && !pt->type)?"":(imageSet && (show_list & 4))?"":&pt->deviceName[5],(imageSet && (show_list & 4))?"":pt->diskLabel,(isMounted)?"\033[0m":"");
                }
	printf("\n");
	}

char listDisks(diskset *ds) {
        int i, j;
        char isSelected = 0;
        char imageSet = (ds == &iset)?1:0;
	if (imageSet) locateImage(); // populates globalPath with archive name
	selection *sel = &sels[LIST_DRIVE];
	unsigned long archSize;
	disk *d;
	archive arch;
	if (ds == NULL) {
       		for (i=0;i<sel->count;i++) {
               		if (i == sel->position || sel->sources[i].state & ST_SEL) {
                       		d = (disk *)sel->sources[i].location;
				if (d->state & ST_SEL) { showDisk(d,0,NULL); isSelected = 1;  }
				else {
					for (j=0;j<d->partCount;j++) {
						if (d->parts[j].state & ST_SEL) { showDisk(d,0,NULL); isSelected = 1; break; }
						}
					}
				}
			}
	} else { // list all disks or image contents
		if (imageSet) {
			if ((archSize = getImageTitle(globalPath,globalBuf,&arch)) == 0) debug(EXIT, 0,"Unable to read archive title %s\n",globalPath);
			readableSize(archSize);
			printf("Archive: %s [%s/%s]\n\n",globalBuf,readable,signature);
			if (!(show_list & 6)) closeArchive(&arch);
			}
		if (!ds->dcount) debug(EXIT, 1,"Nothing to list.\n");
        	for (i=0;i<ds->dcount;i++) {
			showDisk(&(ds->drive[i]),(imageSet)?i+1:0,(imageSet && (show_list & 6))?&arch:NULL);
			}
		if (imageSet && (show_list & 6)) closeArchive(&arch);
		}
	return isSelected;
	}

bool checkMapping(bool verify, bool ui) {
        selection *imageSel = &sels[LIST_AUX1];
        selection *driveSel = &sels[LIST_AUX2];
	int i = driveSel->position;
	int j = driveSel->top;
	int n;
	disk *d;
        item *imageItem = &imageSel->sources[imageSel->position];
        item *driveItem = &driveSel->sources[driveSel->position];
        // char imageType = (imageItem->diskNum == -1)?MBR:PART;
	bool imageTypeMBR = (imageItem->diskNum == -1);
        // char driveType = (driveItem->diskNum == -1)?MBR:(driveSel->position && (driveSel->sources[driveSel->position-1].state != ST_NOSEL))?PART:LOOP;
	// char driveType = (driveItem->diskNum == -1)?MBR:PART;
	bool driveTypeMBR = (driveItem->diskNum == -1);
        int imageSectorSize = (imageTypeMBR)?((disk *)imageItem->location)->sectorSize:iset.drive[imageItem->diskNum].sectorSize;
        int driveSectorSize = (driveTypeMBR)?((disk *)driveItem->location)->sectorSize:dset.drive[driveItem->diskNum].sectorSize;
        sector imageLength = (imageTypeMBR)?((disk *)imageItem->location)->deviceLength:((partition *)imageItem->location)->length;
        sector driveLength = (driveTypeMBR)?((disk *)driveItem->location)->deviceLength:((partition *)driveItem->location)->length;
        validMap = false;

	if (verify && (n = verifyAllSelected(driveSel))) {
		if (n >= 2) {
			fillAttached(&sels[LIST_DRIVE]);
			if (n == 2) { driveSel->position = i; driveSel->top = j; }
			}
		if (ui) displaySelector(driveSel,0);
		return checkMapping(false,ui);
		}
        if (!imageTypeMBR && imageItem->state == CLR_WDIM) return false; // empty partition
        if (!driveTypeMBR) {
                if ((driveItem->state & CLR_MASK) == CLR_RB) return false; // can't access a mounted partition
		if (imageTypeMBR) { // allow only loop or 'X' states to copy onto partitions
			d = (disk *)imageItem->location;
			if ((d->diskType & DISK_MASK) && ((d->diskType & DISK_TABLE))) return false; // ((d->state & ST_BLOCK) != ST_FULL)) return false;

			// allow loops and block-saved disks ('dd') to be place onto partitions
			}
                // if (imageType == MBR) return false;
		if ((imageLength * (sector)imageSectorSize) > (driveLength * (sector)driveSectorSize)) return false; // won't fit
                // check to see if loop/part fits slot, or is growable
                }
        else {
		if (driveItem->state == CLR_RB) return false;
                else if ((driveItem->state & CLR_MASK) ==  CLR_BUSY) { // see if MBR doesn't interfere with mounted partitions
                        if (!imageTypeMBR) return false; // can't put part onto a busy MBR
			if (hasMountMismatch((disk *)imageItem->location,(disk *)driveItem->location) & 4) return false; // would destroy currently-mounted restore part, or eliminate currently mounted parts
                        }
		else if ((imageLength * (sector)imageSectorSize) > (driveLength * (sector)driveSectorSize)) {
			return false; // source image bigger than target drive; don't allow
			}
                // sectorSize*length == sectorSize*length, physSectorSize should match
                // sectorSize needs to match, and physSize should match as well.
                }
        validMap = true;
        return true;
        }

/*

struct __partition {
        int num;        // 1
        char deviceName[MAXDISK]; // /dev/sda1
        char diskLabel[MAXDISK]; // name of partition
        PedSector start; //
        PedSector length; //
        char size[MAXSIZE]; // 300MB
        char type[MAXSIZE]; // ext3
        short flags;
        };

*/
