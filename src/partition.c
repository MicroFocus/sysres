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

#define _LARGEFILE64_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h> // used for 'bool' type
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/kdev_t.h>
#include "fileEngine.h"
#include "image.h" 			// iset
#include "mount.h"			// globalBuf, currentLine
#include "partition.h"
#include "window.h"
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

#define MAXLEN 256

#define SKIPCD

#define DIVIDER "==============================================================================\n"



extern int filterCount;
extern char drive_filter[MAXDISK][MAXDRIVES];
extern bool validMap;

#define PARTENTRY 512
char tmp[MAXDISK];
char mbr[PARTENTRY];

char *getStringPtr(poolset *sel, char *str);
bool quickRead(char *path, char *buf, int max);
bool readDeviceLine(char *path,char *val, bool append);

bool readFromDisk(disk *d, int fd, unsigned long offset, archive *arch, unsigned long firstExtended);


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


*/

void checkIfLoop(disk *d) {
	char *label;
	partition *p;
	d->partCount = 0;
	unsigned char type = findLabel(d->deviceName,&label);
	if (type != PART_RAW) { // add a partition entry
		d->diskType = type;
		d->diskLabel = label;
		}
	// else if (d->diskType != DISK_GPT) d->diskType = DISK_RAW;
	else d->diskType = DISK_RAW;
	}

int partNumber(char *node) {
        int nl = strlen(node)-1;
        if (node == NULL || !*node) return 0; // shouldn't happen
        int i = nl;
        while(node[i] >= '0' && node[i] <= '9') { if (!i--) return atoi(node); }
        if (i == nl) return 0;
        return atoi(&node[i+1]);
        }


// use kernel's device table to create the disk structure
void appendPartition(disk *d, char *path, char *node) {
	partition *p;
	if (d->partCount >= d->psize && expandPartition(d)) return; // no more space for add'l partitions
	p = &(d->parts[d->partCount]);
	p->map = NULL;
	p->state = 0;
	p->flags = FREE; // must be set by actual partition table as a sanity check later
	p->num = partNumber(node);
	sprintf(tmp,"/dev/%s",node);
	p->deviceName = getStringPtr(&dset.pool,tmp);
	if (!readDeviceLine(path,"size",false)) return;
	p->length = atol(tmp);
	// if (p->length == 2) return; // extended partition or some minimal block (verify against actual partition read)
	if (!readDeviceLine(path,"start",false)) return;
        p->start = atol(tmp);
	// p->size = getStringPtr(&dset.pool,readableSize(p->length * d->sectorSize));
	// p->size = "100MB";
	p->type = findLabel(p->deviceName,&p->diskLabel);
	d->partCount++;
	}

#define MBR_MAGIC 0x55AA

// validate structure read from disk against that given by the kernel (stored in disk *d); set p->flags when we match something
void validatePart(disk *d, int pnum, bool bootable, char type, unsigned long start, unsigned long length) {
	int i;
	partition *p;
	/* find partition with same start */
	for (i=0;i<d->partCount;i++) {
		p = &d->parts[i];
		if (p->start == start && p->num == pnum) {
			if (type == 0x0F || type == 0x05) { // remove extended partition from list
				// d->partCount--;
				// if (i < d->partCount) memmove(&d->parts[i],&d->parts[i+1],sizeof(partition));
				p->flags = EXTENDED;
				return;
				}
			if (p->length == length) break;
			else debug(INFO, 1,"Partition table length mismatch.\n");
			}
		}
	if (i == d->partCount) return; // ext'd part the second time around
        p->flags = (bootable)?(PRIMARY | BOOTABLE):PRIMARY; // boot (this is less relevant now, even for boot)
	// if (!type) p->type = 0; // empty type 0x00
	}

unsigned long getSector(u_char *val) { return (val[0] + val[1] * 0x100UL + val[2] * 0x10000UL + val[3] * 0x1000000UL); }

unsigned long getLong(u_char *val) { return getSector(val) + val[4] * 0x100000000UL + val[5] * 0x10000000000UL + val[6] * 0x1000000000000UL; }

bool writeBuffer(int fd, int len, char *buf) {
	int n,size = 0;
	while(size < len && (n = write(fd,&buf[size],len-size)) > 0) size+=n;
	if (size != len) return true;
	return false;
	}

gpt_table globalTable = { NULL, 0, NULL, 0 };

bool readGPT(disk *d, int fd, archive *arch, unsigned int bsize) {
        char backup = (bsize)?1:0;
        bool restore = (arch != NULL && (arch->state & ARCH_READ))?true:false;
	unsigned int restoreBsize = 0;

        if (!bsize) {

                // BLKGETSIZE64 => size of device in sectors
		if (d != NULL) {
                	if (ioctl(fd,BLKSSZGET,&bsize) < 0) { debug(INFO, 1,"Can't read blocksize.\n"); return false; }
			}
		if (restore) {
			if (readFile((unsigned char *)&restoreBsize,4,arch,1) != 4) return false; // read bsize
			if ((d != NULL) && (restoreBsize != bsize)) { debug(INFO, 2,"Target drive LBA size does not match archive LBA size.\n"); return false; }
			bsize = restoreBsize;
			}
                if (bsize > globalTable.lba_limit) { // allocate some space for the LBA header
                        if ((globalTable.lba = realloc(globalTable.lba,bsize)) == NULL) { debug(INFO, 1,"Allocation error.\n"); return false; }
                        globalTable.lba_limit = bsize;
                        }
                // if (bsize != PARTENTRY) { printf("Non-512 block sizes not yet handled.\n"); exit(0); } // set as raw in this case...?
		if (arch != NULL) debug(INFO, 1,"LBA size is %i.\n",bsize);
                }
        u_char *lba = globalTable.lba;

        int n, size =0;
        unsigned long start, stop;

        int i;
        // printf("Reading GPT header [%i, %i].\n",sizeof(unsigned int),sizeof(unsigned long));
	if (restore) size = readFile(lba,bsize,arch,1);
	else while(size < bsize && (n = read(fd,&lba[size],bsize-size)) > 0) size+=n; // make sure we read 512 bytes
       	if (size != bsize) { debug(INFO, 1,"GPT read size mismatch.\n"); return false; } // can't read this

        if (strncmp("EFI PART",lba,8)) { debug(INFO, 1,"Not a GPT header.\n"); return false; }
        unsigned int headerSize = getSector(&lba[12]); // size of this header
        unsigned long currentLBA = getLong(&lba[24]); // location of this LBA
        unsigned long backupLBA = getLong(&lba[32]); // location of backup LBA
        unsigned long usableStart = getLong(&lba[40]); // first usable block (not needed)
        unsigned long usableEnd = getLong(&lba[48]); // last usable block (+1 = backupStart, typically)
        unsigned long partlistStart = getLong(&lba[72]);
        unsigned int partNum = getSector(&lba[80]);
        unsigned int partSize = getSector(&lba[84]);

	if (restore) {
		if (d == NULL) debug(INFO, 1,"Buffering %i %sGPT header to memory.\n",bsize,(backup)?"backup ":"");
		else { // write LBA to disk
			debug(INFO, 1,"Writing %i-byte %sGPT header to %s.\n",bsize,(backup)?"backup ":"",d);
			if (writeBuffer(fd,bsize,lba)) { debug(INFO, 2,"Unable to write to %s.\n",d); return false; }
			}
		}

        u_char *parts = globalTable.partBlock;
        if (!backup) {
                if (globalTable.partLimit < 2*bsize + partSize*partNum) {
                        globalTable.partLimit = 2*bsize + partSize*partNum;
                        if ((globalTable.partBlock = realloc(globalTable.partBlock,globalTable.partLimit)) == NULL) { debug(INFO, 1,"GPT allocation error.\n"); return false; }
                        // printf("Allocated part block %i\n",globalTable.partLimit);
                        parts = globalTable.partBlock;
                        }
                memcpy(parts,lba,bsize);
                // store the primary lba into the partBlock, then the partBlock, then the last one. Then verify the partBlock from the last one.

		if (d != NULL) lseek64(fd,bsize*partlistStart,SEEK_SET);

                for (i=0;i<partNum;i++) {
                        size = 0;
			if (restore) size = readFile(lba,partSize,arch,1);
			else while(size < partSize && (n = read(fd,&lba[size],partSize-size)) > 0) size+=n;
                        if (size != partSize) { debug(INFO, 1,"GPT partition size mismatch.\n"); return false; } // can't read this
                        memcpy(&parts[bsize+i*partSize],lba,partSize);
                        start = getLong(&lba[32]);
                        stop = getLong(&lba[40]);
                        if (!start && !stop) continue;
                        // printf("Part %i: [%lu,%lu]\n",i,start,stop);
			if (arch == NULL) validatePart(d,i+1,0,0,start,stop-start+1);
                        }
                // printf("\033[32mBackup block should be: %lu\033[0m\n",usableEnd+1); // == usableEnd+1 or backupLBA - (part# * partSize)
                // printf("Compare to: %lu, store %lu x 2 + 1\n",backupLBA - ((partNum * partSize)/bsize),(partNum * partSize)/bsize + 1);

                if (d != NULL) {
			if (restore) { // write to disk
				debug(INFO, 1,"Writing %i GPT table (%i x %i) to %s.\n",partNum*partSize,partNum,partSize,d);
				if (writeBuffer(fd,partSize*partNum,&parts[bsize])) { debug(INFO, 2,"Unable to write to %s.\n",d); return false; }
				}
			lseek64(fd,bsize*backupLBA,SEEK_SET);
			}
		else debug(INFO, 1,"Buffered %i GPT table (%i x %i) to memory.\n",partNum*partSize,partNum,partSize);
		readGPT(d,fd,arch,bsize);
                }
        else { // store the backup block into the end of the partTable (loc = partNum * partSize + bsize)
                // validate the backup partition
                if (memcmp(lba,parts,16) || memcmp(&parts[40],&lba[40],32) || memcmp(&lba[80],&parts[80],bsize-80) || !memcmp(&lba[72],&parts[72],8)) { debug(INFO, 1,"Backup partition mismatch.\n"); return false; }
                memcpy(parts+partNum*partSize+bsize,lba,bsize);
		if (d != NULL) {
			lseek64(fd,bsize*partlistStart,SEEK_SET);
			if (restore) { // write to disk
				debug(INFO, 1,"Writing %i backup GPT table to %s.\n",partSize*partNum,d);
				if (writeBuffer(fd,partSize*partNum,&parts[bsize])) { debug(INFO, 2,"Unable to write to %s.\n",d); return false; }
				}
			else {
                		for (i=0;i<partNum;i++) {
                        		size = 0;
					while(size != partSize && (n = read(fd,&lba[size],partSize-size)) > 0) size+=n;
                        		if (size != partSize) { debug(INFO, 1,"GPT backup partition size mismatch.\n"); return false; } // can't read this
                        		if (memcmp(lba,&parts[bsize+i*partSize],partSize)) { debug(INFO, 1,"Backup partition mismatch on %i.\n",i); return false; }
					}
				if (arch != NULL) {
					debug(INFO, 1,"Storing %i byte GPT structure to archive.\n",2*bsize+partSize*partNum);
 					if (writeFile((unsigned char *)&bsize,4,arch) == -1) { debug(ABORT, 1,"Error writing to archive"); return false; }
					restoreBsize = 2*bsize + partNum*partSize;
					if (writeFile(parts,restoreBsize,arch) == -1) { debug(ABORT, 1,"Error writing to archive"); return false; }
					}
                        	}
			}
                }
	return true;
        }

bool extractEntries(disk *d, u_char *entry, int fd, unsigned long offset, archive *arch, unsigned long firstExtended) {
        int i;
        u_char *part;
        unsigned long start, length;
        unsigned long extd = 0;
        unsigned long next = 0;
        static pindex = 0;
	bool gpt = false;
        if (!offset) pindex = 0;
        for (i=0;i<((!offset)?4:2);i++) {
                if (!offset) pindex++;
                else if (!i) pindex++;
                part = &entry[i*16];
                start = getSector(&part[8]);
                length = getSector(&part[12]);
                if (part[4] == 0x0F || part[4] == 0x05) { next = start + firstExtended; if (!firstExtended) firstExtended = start; }
		else if (start) start += offset;
// printf("PART: %02X, offset %lu, next %lu\n",part[4],offset,next);
		if (!i && !offset && part[4] == 0xEE) { if (arch == NULL) d->diskType = DISK_GPT; debug(INFO, 1,"Disk is GPT\n"); gpt = true; }
                else if ((!offset || !i) && length && (arch == NULL)) { gpt = false; validatePart(d, pindex,(part[0] == 0x80),part[4],start,length); }
                }
	if (entry[65] + 0x100 * entry[64] != MBR_MAGIC) { debug(INFO, 2,"Magic doesn't match\n"); return false; }
	if (gpt) { gpt = readGPT(d,fd,arch,0); if (d != NULL) close(fd); return gpt; }
        else if (next) return readFromDisk(d,fd,next,arch, firstExtended);
	return true;
        }

bool readFromDisk(disk *d, int fd, unsigned long offset, archive *arch, unsigned long firstExtended) {
	int size = 0;
	int n;
	bool isOK;
	bool restore = (arch != NULL && (arch->state & ARCH_READ))?true:false;
#ifndef PRODUCTION
if (restore && d != NULL && !strcmp((char *)d,"/dev/sda")) debug(EXIT, 1,"DO NOT DO THIS TO /dev/sda!!!\n");	// don't goof up the primary disk!
#endif
	if (d != NULL) { // not buffered read from archive
		if (!fd) fd = open((arch == NULL)?d->deviceName:(char *)d,(restore)?O_WRONLY:O_RDONLY | O_LARGEFILE);
		else lseek64(fd,offset*512,SEEK_SET);
		if (fd < 0) {
			debug(INFO, 0,"Unable to open %s\n",(arch == NULL)?d->deviceName:(char *)d);
			return false;
			}
		}
	if (restore) size = readFile(mbr,PARTENTRY,arch,1);
        else { while(size < PARTENTRY && (n = read(fd,&mbr[size],PARTENTRY-size)) > 0) size+=n; } // make sure we read 512 bytes (d is never null here)
        if (size != PARTENTRY) { debug(INFO, 1,"Size %i not equal to PARTENTRY",size);  if (!offset && (d != NULL)) close(fd); return false; }
	if (arch != NULL) {
		if (restore) {
			if (d == NULL) debug(INFO, 5,"Counting 512 bytes for buffer allocator\n"); // used to count how big the mbr buffer needs to be from the archive
			else {
				debug(INFO, 2,"Writing 512 MBR bytes to %s\n",(char *)d);
				if (writeBuffer(fd,PARTENTRY,mbr)) { debug(INFO, 2,"Write error to %s",(char *)d); if (!offset) close(fd); return false; }
				}
			}
		else {
			debug(INFO, 1,"Write %i bytes from device %s to archive.\n",size,(char *)d);
			if (writeFile(mbr,PARTENTRY,arch) == -1) { debug(ABORT, 1,"Error writing to archive"); return false; }
			}
		}
	isOK = extractEntries(d,&mbr[446],fd,offset,arch, firstExtended);
	if (!offset && (d != NULL)) { if (restore) fsync(fd); close(fd); }
	return isOK;
	}

bool readDeviceLine(char *path,char *val, bool append) {
	char *b= tmp;
	if (append) b= &tmp[strlen(tmp)];
	else *tmp = 0;
	sprintf(globalBuf,"%s/%s",path,val);
	if (!quickRead(globalBuf,b,MAXDISK-strlen(tmp))) return false;
	return true;
	}

int readDrive(char *path, char *node, disk *d) {
	if (readDeviceLine(path,"queue/logical_block_size",false)) d->sectorSize = atoi(tmp);
	else d->sectorSize = 512;
	if (!readDeviceLine(path,"size",false)) return 0; // doesn't make sense
	d->deviceLength = atol(tmp);
	// DISK TYPE
	d->diskType = DISK_MSDOS; // EMPTY;
	// DISK SIZE (size * sectorSize / MB)
//	readableSize(d->deviceLength * d->sectorSize, tmp);
//        d->size = getStringPtr(&dset.pool,tmp);
	sprintf(globalBuf,"/dev/%s",node);
	d->deviceName = getStringPtr(&dset.pool,globalBuf);
	if (readDeviceLine(path,"device/vendor",false)) strcat(tmp," ");
	readDeviceLine(path,"device/model",true);
	d->diskName = getStringPtr(&dset.pool,tmp);
	d->map = NULL;
	d->partCount = 0;
	d->state = 0;
	d->diskLabel = ""; // usually blank
	return 1;
	// sprintf(globalBuf,"/sys/block/sda/sda#/start,size"); // elsewhere
	}

// echo 1 > /sys/block/sdX/device/rescan (Where X is your device letter); use other method...?
