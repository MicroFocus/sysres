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
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>

#include "sysres_debug.h"	// debug()
#include "window.h"
#include "partition.h"
#include "fileEngine.h"

#define CHUNK_SMALL 32768	// 32KB
#define CHUNK_LARGE 33554432	// 32MB

extern char labelBuf;

/*
                case ST_IMG: field = '*'; break; // partimage; mbr only
                case ST_CLONE: field = '#'; break; // partclone, mbr + 32KB
                case ST_FILE: field = '+'; break; // mbr + 32MB
                case ST_FULL: field = 'x'; break; // mbr + all free space
                case ST_SWAP: field = 's'; break;
*/

bool writeExtraBlocks(char *dev, archive *arch) {
	unsigned char magic[4];
	int n;
	unsigned long startBlock;
	unsigned long chunkSize;
#ifndef PRODUCTION
if (!strcmp(dev,"/dev/sda")) debug(EXIT,0,"TRYING TO WRITE TO /dev/sda!!!\n");
#endif
	int fd = open(dev,O_WRONLY | O_LARGEFILE);
	if (fd < 0) { debug(ABORT,0,"Write error to %s",dev); return true; }
	progressBar(arch->expectedOriginalBytes,PROGRESS_RED,PROGRESS_INIT);
	while ((n = readFile(magic,4,arch,1)) > 0) {
		if (n != 4) break;
		if (strcmp(magic,"BLCK")) { close(fd); debug(ABORT,1,"Magic block error"); return true; }
		if (readFile(&startBlock,sizeof(unsigned long),arch,1) != sizeof(unsigned long)) { close(fd); debug(ABORT,1,"Start block error"); return true; }
		if (readFile(&chunkSize,sizeof(unsigned long),arch,1) != sizeof(unsigned long)) { close(fd); debug(ABORT,1,"Chunk size error"); return true; }
		debug(INFO,5,"Seeking to %lu, writing %lu\n",startBlock,chunkSize);
		lseek64(fd,startBlock,SEEK_SET);
		if (n = readBlock(fd,chunkSize,arch,true)) {
			close(fd);
			if (n != -2) debug(ABORT,1,"Chunk issue");  // not cancelled
			return true;
			}
		}
	if (n != 0) { debug(ABORT,1,"Chunk integrity error"); return true; }
	progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_SYNC);
	startTimer(2);
	fsync(fd);  // make sure it's flushed
	stopTimer();
	progressBar(0,arch->originalBytes,PROGRESS_OK);
	progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_OK | 1);
	close(fd);
	return false;
	}


bool callWriteBlock(int fd, char *dev, unsigned long start, unsigned long length, archive *arch) {
	int n;
	debug(INFO,2,"Storing %s from seek %lu, length %lu\n",dev,start,length);
	lseek64(fd,start,SEEK_SET);
	if ((writeFile("BLCK",4,arch) == -1) || (writeFile(&start,sizeof(unsigned long),arch) == -1) || (writeFile(&length,sizeof(unsigned long),arch) == -1)) {
		debug(ABORT,1,"Error writing to archive; check disk space");
		return true;
		}
	if (n = writeBlock(fd,length,arch,true)) {
		close(fd);
		if (n != -2) debug(ABORT,1,"Error writing block; check disk space"); // not cancelled
		return true;
		}
	return false;
	}


bool readExtraBlocks(disk *d, unsigned char state, archive *arch) {
	int i, j;
	unsigned long totalLength = arch->originalBytes;
	unsigned long nextBlock = d->deviceLength * d->sectorSize;
	unsigned long startBlock = 512;
	if (d == NULL || (state == ST_IMG)) { // no extra blocks read with ST_IMG
		progressBar(arch->originalBytes,PROGRESS_GREEN,PROGRESS_INIT);
		signFile(arch);
		progressBar(0,arch->fileBytes,PROGRESS_OK);
		return false;
		}
	debug(INFO,2,"Reading extra blocks from %s\n",d->deviceName); // may overlap over ext'd sectors, but it shouldn't matter on restore
	int fd = open(d->deviceName,O_RDONLY | O_LARGEFILE);
	if (fd < 0) { debug(ABORT,0,"Unable to open %s",d->deviceName); return true; }
	if (d->partCount == 0 || state != ST_FULL) { // no partitions, or read only the first small/large chunk
		if (d->partCount) nextBlock = d->parts[0].start * d->sectorSize;
		if (state == ST_CLONE && nextBlock > CHUNK_SMALL) nextBlock = CHUNK_SMALL; // 512 -> 32768 = 63 sectors
		if (state == ST_FILE && nextBlock > CHUNK_LARGE) nextBlock = CHUNK_LARGE;
        	nextBlock -= startBlock; // now length
		totalLength += 20+nextBlock;
		progressBar(totalLength,PROGRESS_GREEN,PROGRESS_INIT);
		if (nextBlock) { if (callWriteBlock(fd,d->deviceName,startBlock,nextBlock,arch)) return true; }
		}
	else for (j=0;j<2;j++) { // run twice, once to determine the total size required
		startBlock = 512;
		for (i=0;i<=d->partCount;i++) {
			nextBlock = ((i == d->partCount)?d->deviceLength:d->parts[i].start) * d->sectorSize;
			if (startBlock >= d->deviceLength * d->sectorSize) break; // shouldn't happen
			if ((nextBlock -= startBlock) && j) { if (callWriteBlock(fd,d->deviceName,startBlock,nextBlock,arch)) return true; }
			totalLength += 20+nextBlock;
			if (i != d->partCount) startBlock = (d->parts[i].start + d->parts[i].length) * d->sectorSize; // next start block
			}
		if (!j) progressBar(totalLength,PROGRESS_GREEN,PROGRESS_INIT);
		}
	signFile(arch);
	progressBar(0,arch->fileBytes,PROGRESS_OK);
	close(fd);
	return false;
	}

bool storeSwap(char *dev, archive *arch) {
	int n = getpagesize();
	int fd = open(dev,O_RDONLY | O_LARGEFILE);
	if (fd < 0) { debug(ABORT,0,"Swap read error from %s",dev); return true; } // ERROR
	progressBar(n,PROGRESS_GREEN,PROGRESS_INIT);
	if (n = writeBlock(fd,n,arch,false)) {
		if (n != -2) debug(ABORT,1,"Swap read issue");
		return true;
		}
	signFile(arch);
	close(fd);
	progressBar(0,arch->fileBytes,PROGRESS_OK);
	return false;
	}

bool restoreSwap(char *dev, archive *arch) { // could also just use the dd engine
	int fd = open(dev,O_WRONLY | O_LARGEFILE);
	if (fd  < 0) { debug(ABORT,0,"Swap write error to %s",dev); return true; }
	progressBar(arch->expectedOriginalBytes,PROGRESS_RED,PROGRESS_INIT);
	if (readBlock(fd,arch->expectedOriginalBytes,arch,false)) { debug(ABORT,1,"Swap write issue"); return true; }
	debug(INFO,2,"Swap to %s, %lu\n",dev,arch->expectedOriginalBytes);
        progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_SYNC);
	startTimer(2);
        fsync(fd);  // make sure it's flushed
	stopTimer();
        progressBar(0,arch->originalBytes,PROGRESS_OK);
	progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_OK | 1); // update global counter
	close(fd);
	return false;
	}
