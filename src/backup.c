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
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef PARTCLONE
  #include <libpartclone.h>
#endif
#include "cli.h"					// validOperation
#include "drive.h"				// types, testMode, mapTypes
#include "fileEngine.h"
#include "mount.h"				// globalBuf
#include "partition.h"
#include "partutil.h"			// readable
#include "sysres_debug.h"	// debug()
#include "window.h"     	// globalPath

extern navImage image1;
extern int termWidth;


extern unsigned char md_value[];
extern int md_len;

char *imagePath(char *append);
extern int segment_size;
extern char compression;

extern char imageDevice[];
extern unsigned char mapMask;
unsigned int opCount; // current backup operation
unsigned int globalOps; // total number of backup operations

int backupPID = 0;
extern volatile sig_atomic_t has_interrupted;



void exitConsole(void) { // go back to UI
	noecho();
	curs_set(0);
	refresh();
	}

bool ddEngine(char *device, int major, int minor, unsigned char type, archive *arch, unsigned long length) {
	int n;
	progressBar(length, PROGRESS_GREEN, PROGRESS_INIT);
	int fd = open(device,O_RDONLY | O_LARGEFILE);
        if (fd < 0) { debug(ABORT,1,"Unable to open %s",device); return true; }
	if (addFileToArchive(major,minor,ST_FULL | compression,arch) != 1) {
		close(fd);
		debug(ABORT,1,"Error adding file to archive; check disk space");
		return true;
		}
	if (n = writeBlock(fd, length, arch,true)) {
		close(fd);
		if (n == -2) return true; // user cancelled
		debug(ABORT,1,"Error writing block; check disk space");
		return true;
		}
	signFile(arch);
	progressBar(0, arch->fileBytes, PROGRESS_OK);
	close(fd);
	return false;
	}

#define ARGCOUNT 6

bool pcloneEngine(char *device, int major, int minor, unsigned char type, archive *arch) {
	pc_hdr hdr;
	unsigned char buf[4096];
	int n;
	char *argv[] = { "pclone", "-c", "-o", "-", "-s", NULL }; // -d3
	argv[ARGCOUNT-1] = device;
	if (addFileToArchive(major,minor,ST_CLONE | compression,arch) != 1) {
		debug(ABORT,0,"Error adding file to archive; check disk space");
		return true;
		}
	int mypipe[2];
	pid_t pid;
	int status;
	if (pipe(mypipe)) debug(EXIT,0,"Unable to create pipe\n");
	pid = fork();
	if (pid == (pid_t) 0) { // child process
		close(mypipe[0]);
		// signal(SIGQUIT,SIG_DFL) // for production
#ifdef PARTCLONE
		LIBPARTCLONE_MainEntry(type,mypipe[1],ARGCOUNT,argv);
#endif
		close(mypipe[1]);
		_exit(0);
		}
	else if (pid < (pid_t) 0) debug(EXIT,0,"Unable to fork\n");
	else { // parent
		close(mypipe[1]);
		if (ui_mode) {
			progressBar(0,PROGRESS_GREEN,PROGRESS_INIT); // reset progress bar for feedback
			if (progressBar(0,0,PROGRESS_UPDATE)) { // it was cancelled
				close(mypipe[0]);
				wait(pid);
				feedbackComplete("*** CANCELLED ***");
				return true;
				}
			}
		setStatus("Scanning used blocks...");
		backupPID = pid;
		startTimer(1);
		// printf("%s Scanning used blocks...",&globalBuf[30]); fflush(stdout);
		if ((n = read(mypipe[0],&hdr,sizeof(pc_hdr))) != sizeof(pc_hdr)) {
			if (has_interrupted) progressBar(0,0,PROGRESS_CANCEL);
			backupPID = 0;
			close(mypipe[0]);
			wait(pid,&status,0);
			stopTimer();
			if (has_interrupted) { feedbackComplete("*** CANCELLED ***"); return true; }
			else setStatus(NULL);
			if (type == PART_EXT2 || type == PART_EXT3 || type == PART_EXT4) debug(ABORT,1,"Image header issue; try e2fsck first");
			else debug(ABORT,1,"Image header issue"); // except when doing DD; then it's something else
			return true;
			}
		backupPID = 0;
		stopTimer();
		debug(INFO,1,"block: %i, fixed: %i, used: %lu, total: %lu\n",hdr.blocksize,PC_FIXED,hdr.usedblocks,hdr.totalblocks);
		if (ui_mode) setStatus("Processing");
		progressBar(hdr.usedblocks * (hdr.blocksize + 4) + PC_FIXED + hdr.totalblocks, PROGRESS_GREEN, PROGRESS_INIT);
		if (writeFile(&hdr,n,arch) == -1) {
			close(mypipe[0]);
			wait(pid);
			feedbackComplete("*** WRITE ERROR ***");
			return true;
			}; // write out header
		while((n = read(mypipe[0],buf,4096)) > 0) {
			if (writeFile(buf,n,arch) == -1) {
				close(mypipe[0]);
                        	wait(pid);
                        	feedbackComplete("*** WRITE ERROR ***");
                        	return true;
				}
			if (progressBar(arch->originalBytes, arch->fileBytes, PROGRESS_UPDATE)) { // it was cancelled
				close(mypipe[0]);
				wait(pid);
				feedbackComplete("*** CANCELLED ***");
				return true;
				}
			}
		// if (hdr.usedblock * (hdr.blocksize + 4) + PC_FIXED + hdr.totalblocks != arch->originalBytes)...
		close(mypipe[0]);
		signFile(arch);
//		wait(pid);
		waitpid(pid,&status,0);
		if (status) { debug(ABORT,1,"Non-zero exit code from pclone engine"); return true; }
		progressBar(0, arch->fileBytes, PROGRESS_OK);
		}
	return false;
	}

char *getEngine(unsigned char type, unsigned char state) {
	switch(state) {
		case ST_IMG: return (type == DISK_MSDOS)?"mbr":(type == DISK_GPT)?"gpt":"prtimg"; break;
#ifdef PARTCLONE
		case ST_CLONE: return (type == DISK_MSDOS)?"# mbr":(type == DISK_GPT)?"# gpt":"pclone"; break;
#else
		case ST_CLONE: return (type == DISK_MSDOS)?"# mbr":(type == DISK_GPT)?"# gpt":"uclone"; break;
#endif
		case ST_FILE: return (type == DISK_MSDOS)?"+ mbr":(type == DISK_GPT)?"+ gpt":"fsarch"; break;
		case ST_FULL: return (type == DISK_MSDOS)?"x mbr":(type == DISK_GPT)?"x gpt":"direct"; break;
		case ST_SWAP: return "swpimg"; break;
		default: return "???"; break;
		}
	}

bool populateEntry(archive *arch, char pass, unsigned char state, unsigned char type, unsigned int major, unsigned int minor, unsigned long length, unsigned long start, char *label, char *device, disk *d) {
	imageEntry e;
	if (!major && !(pass & 1)) return false; // floating partition (0:0 won't go through this function here)
	else if (major && (pass & 1)) return false; // part attached to disk

	if (state == CLR_RB) state = 0; // report only
	else opCount++;
	// pick engine for MBR
	if (state == ST_AUTO) state = ST_IMG;
	else if (state == ST_MAX) state = ST_FULL;

	// opCount++;
	if (!(pass & 2)) { // populate index
		bzero(&e,sizeof(imageEntry)); // in case the struct is no longer aligned; this way the SHA1SUM will be consistent
		debug(INFO,5,"Adding %s as index %i:%i\n",device,major,minor);
		e.fsType = (state)?type:(type & TYPE_BOOTABLE)?TYPE_BOOTABLE:PART_EMPTY; // EXT, DISK_MBR, etc.
		e.archType = state; // [COMPRESSION &] ARCH_INTERPRETER (dd, MBR, partclone, SWAP), determine by ST_SEL state condition; 0 = do nothing, don't include
		e.major = major;
		e.minor = minor;
		e.length = length;
		e.start = start;
		bzero(e.label,IMAGELABEL);
		if (*label && state) strcpy(e.label,label);
		if (!testMode) {
			if (writeFile(&e,sizeof(imageEntry),arch) == -1) { debug(ABORT,0,"Archive write error"); return true; }
			}
		// printf("Populating %i:%i from %s\n",major,minor,device);
		}
	else if (state && (type & TYPE_MASK)) { // write an actual partition to the image, using a particular engine

// 0 sda pclone ext3 /boot MB % elapsed        remaining
// *1  [pclone/mbr+,#,x/prtimg/fsarch/swap/direct] 1 ext3
	debug(INFO,5,"Storing %s as %i:%i, type %i, state %i\n",device,major,minor,type,state);
	setProgress(PROGRESS_BACKUP,(major)?imageDevice:"prt",device,major,minor,type,label,0,state,arch);
	// sprintf(&globalBuf[30],"%-4s => %i %6s %-5s %-16s",&device[5],minor,getEngine(type & TYPE_MASK,state),(type & DISK_MASK)?"disk":types[type & TYPE_MASK],(label != NULL && *label)?label:"");
	if (testMode) { if (!ui_mode) printf("%s\n",&globalBuf[40]); return false; }
	if (type & DISK_TABLE) {
		if (!ui_mode) { printf(&globalBuf[40]); fflush(stdout); }
		if (addFileToArchive(major,0,state | compression,arch) != 1) {
			debug(ABORT,0,"Unable to add file to archive");
			return true;
			}
		if (readFromDisk(device,0,0,arch,0) == false) {
			debug(ABORT,0,"Partition table error");
			return true;
			} ; // uninterruptible
		// TODO: interrupt handler
		return readExtraBlocks(d,state,arch);
		}
        switch(state) {
                // case ST_IMG: break;
                case ST_CLONE: return pcloneEngine(device,major,minor,type & TYPE_MASK,arch); break;
                case ST_FULL: return ddEngine(device,major,minor,type & TYPE_MASK, arch, d->sectorSize * length); break;
                // case ST_FILE: break;
		case ST_SWAP:
			if (addFileToArchive(major,minor,state | compression,arch) != 1) {
				debug(ABORT,0,"Unable to add file to archive");
				return true;
				}
			return storeSwap(device, arch);
			break;
		default: if (!ui_mode) printf("No known engine.\n"); break;
		}
		}
	return false;
	}

bool restrictedMap(int mapVal) {
	if (!mapVal) return true;
	if (!mapMask) return false;
	return ((mapMask >> (mapVal - 1)) & 1)?false:true;
	}

bool createBackupIndex(archive *arch, char pass) {
	int i, j;
	disk *d;
	partition *p;
	selection *sel = &sels[LIST_DRIVE];
	unsigned char mbrState;
	unsigned int count = 1;
	unsigned int partCount = 1;
	char *comp = (compression == GZIP)?"zlib":(compression)?"lzma":"no";
	*imageDevice = 0;
	if (pass == 2) {
		globalOps = opCount;
		opCount = 0;
		}
	for (i=0;i<sel->count;i++) {
		if (sel->position == i || (sel->sources[i].state & ST_SEL)) d = &dset.drive[sel->sources[i].diskNum];
		else continue;
		j = determineMapType(d,NULL);
		if (restrictedMap(j)) {
			if (!pass && !ui_mode) printf("Skipping %s; map restriction does not match '%s'.\n",d->deviceName,(j)?mapTypes[j]:"none");
			continue;
			}
		validOperation = 1;
		if (d->state & ST_SEL) {
			if (pass == 2) {
				nextDeviceName();
                               	if (!ui_mode) printf("Backing up %s (%s) as %s img %i using %s compression.\n",d->deviceName,mapTypes[j],imageDevice,count,comp);
				}
			if (populateEntry(arch,pass,d->state & ST_BLOCK,d->diskType,count,0,d->deviceLength,d->sectorSize,d->diskLabel,d->deviceName,d)) return true; // change second zero
			mbrState = d->state & ST_BLOCK; // might be 'X', which is a full disk copy, or 'O', which isn't
			}
		else {
			mbrState = 0;
			if (pass == 3) { // disks with individual partitions
				if (!ui_mode) printf("Backing up %s (%s) as prt img 0 using %s compression.\n",d->deviceName,mapTypes[j],comp);
				}
			}
		// if (d->diskType != DISK_MSDOS) { count++; continue; } // no partitions to list (add GPT later once we've added it); should not have partCount
		for (j=0;j<d->partCount;j++) {
			p = &d->parts[j];
			if (!(p->flags & PRIMARY || p->flags & LOGICAL)) continue; // skip possible ext'd partition
			if (mbrState || (p->state & ST_SEL)) {
			// if ((mbrState & ST_GROUP) || (p->state & ST_SEL)) {
				if (!mbrState) { // this reports at the end
					if (populateEntry(arch,pass,p->state,p->type,0,partCount++,p->length,d->sectorSize,p->diskLabel,p->deviceName,d)) return true; // similar to LOOP, but not quite
					}
				else {
					if (mbrState == ST_AUTO && p->state != CLR_RB) p->state = nextAvailable(0,p->type,0);
					if (populateEntry(arch,pass,(mbrState == ST_MAX)?ST_FULL:p->state,(p->flags & BOOTABLE)?p->type|TYPE_BOOTABLE:p->type,count,p->num,p->length,p->start,p->diskLabel,p->deviceName,d)) return true;
					}
				}
			}
		if (mbrState) count++;
		}
		return false;
	}

bool beginArchive(archive *arch) {
	int len;
	char *title = currentImage.imageTitle;
	// struct stat64 s;
	char *name = (ui_mode)?currentImage.imageName:image1.path;
        sprintf(globalPath,"%s%s",currentImage.imagePath,name);
        if (!*title) title = name;
        if ((len = strlen(title)) < 255) bzero(&title[len],256-len);
        // if (!stat64(globalPath,&s)) debug(EXIT,1,"File %s already exists.\n",name);
        createImageArchive(globalPath,segment_size,arch); // 1024 is 1GB split size
        if (addFileToArchive(0,0,0,arch) != 1) { // no compression for the top-level index
		debug(ABORT,0,"Unable to add file to archive");
		return true;
		};
        if (writeFile(title,256,arch) == -1) return true; // error
	return false;
	}

void createBackup(char pass) {
	int i;
	archive arch;
	progressBar(0,0,PROGRESS_INIT | 1); // no global totals for backup
//	if (ui_mode) endwin(); // temporary

	// TODO: check the backup index before creating the archive to make sure we're doing the expected operation, if the option-check is enabled
//	createBackupIndex(NULL,0); // check all the attempted backups to see if they're complete
	// end of TODO
	if (!testMode && beginArchive(&arch)) return; // error
	opCount = 0;
	for (i=0;i<4;i++) { // 4 passes
		if (createBackupIndex(&arch,i)) { // cancelled or error -- don't close archive since that will sign it
			if (!testMode) {
			        if (arch.currentFD != -1) {
					if (arch.fileHeaderFD != -1 && arch.fileHeaderFD != arch.currentFD) { close(arch.fileHeaderFD); fsync(arch.fileHeaderFD); }
					close(arch.currentFD);
					fsync(arch.currentFD);
					arch.currentFD = -1;
					}
				}
			// TODO: delete the entire archive
			return;
			};
		}
	if (!testMode) closeArchive(&arch);
	if (ui_mode) progressBar(0, arch.fileBytes, PROGRESS_COMPLETE);

        if (ui_mode) {  // temporary
//               printf("Done.\n");
//               getch();
//               exitConsole();
		sels[LIST_IMAGE].sources[sels[LIST_IMAGE].position].state = CLR_GB;
                        *currentImage.imagePath = 0;
                        manageWindow(&sels[LIST_IMAGE],INIT);
		}
	}

/*

Map type is: 3
Indexing /dev/sdb as 1:0
Indexing /dev/sdb1 as 1:1
Storing /dev/sdb as 1:0, Engine is MBR.
Storing /dev/sdb1 as 1:1, ENGINE IS PARTCLONE, type ext4.
165.2MB [0%] elapsed: 0:10 [50:34], est. remaining: 50:24


*/


/*  CHECK OUT createTable() FOR remap[] CODE TO ORDER PARTITIONS ACCORDINGLY */
/* after a restore has been performed on a disk, it will see if any labels were SYSLABEL. If so, it will mount them, and if the top-level directory is empty it will copy itself across. (no .sha1)
if you do not want that, then do not select the SYSLABEL label; instead, format it yourself.
*/
