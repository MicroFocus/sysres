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

/*----------------------------------------------------------------------------
** Compiler setup.
*/
#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef PARTCLONE
  #include <libpartclone.h>
#endif
#include "cli.h"					// add_img, testMode, validOperation
#include "drive.h"				// types, mapTypes
#include "fileEngine.h" 	// readSpecificFile(), fileBuf
#include "image.h" 				// iset, loopDrive
#include "mount.h"				// globalBuf
#include "partition.h"
#include "partutil.h"			// readable
#include "window.h"     	// globalPath, options
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

#define MAX_MBRBUF 512000000

/*----------------------------------------------------------------------------
** Storage
*/
	/* Global storage */
	unsigned char *mbrBuf = NULL;
	char globalPath2[MAX_PATH];

	/* Local storage */
	bool           imageBuffered = false;

/*----------------------------------------------------------------------------
**
*/
static bool ddRestore(
		char          *device,
		int            major,
		int            minor,
		unsigned char  type,
		archive       *arch
		)
	{
	int fd = -1;
	int n;

	if(readSpecificFile(arch,major,minor,1) != 1)
		{
		debug(ABORT, 0,"Damaged archive");
		return true;
		}

  fd = open(device,O_WRONLY | O_LARGEFILE);
	if(fd < 0)
		{
		debug(ABORT, 0,"Direct write error to %s",device);
		return true;
		}

	progressBar(arch->expectedOriginalBytes,PROGRESS_RED,PROGRESS_INIT);
  n = readBlock(fd,arch->expectedOriginalBytes,arch,true);
	if(n)
		{
		close(fd);
		if(n != -2)
			debug(ABORT, 1,"Direct write issue"); // not cancelled

		return true;
		}

	progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_SYNC);
	startTimer(2);
	fsync(fd);
	stopTimer();
  close(fd);
	progressBar(0,arch->originalBytes,PROGRESS_OK);

	return false;
  }

/*----------------------------------------------------------------------------
**
*/
static bool pcloneRestore(
		char          *device,
		int            major,
		int            minor,
		unsigned char  type,
		archive       *arch
		)
	{
	bool					rCode = false;
  unsigned char buf[4096];
  int           n,i, offset;
  int           mypipe[2];
  pid_t         pid;
	int           status;

	int   argc = 6;
  char *argv[6] =
		{
		"pclone", 			// argv[0] Program {path +} name.
		"-r", 					// -r or --restore "Restore partition from the special image format."
		"-s", 					// -s or --source "Source FILE. The FILE could be a image file(made by partclone) or device depend on your action. Normanly, backup source is device, restore source is image file."
		"-",            // "Receving data from pipe line is supported ONLY for restoring, just ignore -s option or use '-' means receive data from stdin."
		"-o",           // -o or --output "Output FILE. The FILE could be a image file(partclone will generate) or device depend on your action. Normanly, backup output to image file and restore output to device."
		device          // See argv[5] assignment below. "Sending data to pipe line is also supported ONLY for back-up, just ignore -o option or use '-' means send data to stdout."
		};
  argv[5] = device;

	if(readSpecificFile(arch,major,minor,1) != 1)
		{
		debug(ABORT, 0,"Damaged archive");
		rCode=true;
		goto CLEANUP;
		}

	if(pipe(mypipe))
		debug(EXIT, 0,"Unable to create pipe\n");

	errno = EXIT_SUCCESS;
  pid = fork();
	if((-1) == pid)
		debug(EXIT, 0,"Unable to fork. [%d:%s]\n", errno, strerror(errno));

	// child process
	if(!pid)
		{
    close(mypipe[1]);
    LIBPARTCLONE_MainEntry(0,mypipe[0],argc,argv); // type = 0 doesn't matter for restore
    close(mypipe[0]);
    _exit(0);
    }

  // parent process.
	close(mypipe[0]);
	progressBar(arch->expectedOriginalBytes,PROGRESS_RED,PROGRESS_INIT);
	while((n = readFile(buf,4096,arch,1)) > 0)
		{
		offset = 0;
		if(progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_UPDATE))
			{  // display bytes out
			kill(pid,SIGKILL);
			startTimer(1);
			close(mypipe[1]);
			wait(pid);
			stopTimer();
			feedbackComplete("*** CANCELLED ***");
			rCode=true;
			goto CLEANUP;
			}

		progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_UPDATE | 1); // update global count
		while(offset < n && ((i = write(mypipe[1],&buf[offset],n-offset)) >= 0))
			{
			offset += i;
			}

		if(offset != n)
			{
			debug(ABORT, 1,"Stream error.\n");
			}
		}

	progressBar(arch->originalBytes, arch->originalBytes, PROGRESS_SYNC);
	startTimer(2);
	close(mypipe[1]);
// wait(pid);
	waitpid(pid,&status,0);
	stopTimer();
	if(status)
		{
		debug(ABORT, 1,"Non-zero exit code from pclone engine");
		rCode=true;
		goto CLEANUP;
		}

	progressBar(0,arch->originalBytes,PROGRESS_OK);
	progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_OK | 1); // update global count

CLEANUP:

	return(rCode);
  }

/*----------------------------------------------------------------------------
**
*/
static int validateNewTable(disk *d)
	{
	char path[256];
	sprintf(path,"/sys/block/%s",&d->deviceName[5]);
	// d->deviceLength, d->deviceName, d->diskname, d->sectorSize
	// d->map, d->auxParts => leave alone
	d->partCount = 0;
	d->state = 0;
	d->diskLabel = "";
	d->diskType = DISK_MSDOS;

	if(scanSingleDrive(path,&d->deviceName[5],d))
		return 1;

	if(!readFromDisk(d,0,0,NULL,0))
		checkIfLoop(d);

	if(syncTable(d))
		return 1;

	if(hasMountMismatch(NULL,d))
		return 1;

	return 0;
	}

/*----------------------------------------------------------------------------
**
*/
static bool restore(
		char *device,
		char *label,
		int major,
		int minor,
		char *target,
		int state,
		int type,
		archive *arch
		)
	{
	if(major == loopDrive + 1)
		major = 0;

	setProgress(PROGRESS_RESTORE,device,target,major,minor,type,label,0,state,arch);
// sprintf(&globalBuf[30],"%-4s => %-4s %6s %-5s %-16s",device,&target[5],getEngine(type & TYPE_MASK,state),(type & DISK_MASK)?"disk":types[type & TYPE_MASK],(label != NULL && *label)?label:"");
	if(testMode)
		{
		if(!ui_mode)
			printf("%s\n",&globalBuf[40]);

		return false;
		}

	if(type & DISK_TABLE)
		{ // gpt or msdos
		if((state & BUFFERED) == BUFFERED)
			{
			arch->state |= BUFFERED;
			arch->fileBytes = 0;
			}
    else if(readSpecificFile(arch, major, minor,1) != 1)
			{
			// debug(EXIT, 0,"Damaged archive");  // test for fatal error
			debug(ABORT, 0,"Damaged archive");
			return true;
			}

    if(!readFromDisk(target,0,0,arch,0))
			{
			debug(ABORT, 0,"Restore write error");
			return true;
			}

		if(((state & BUFFERED) != BUFFERED) && writeExtraBlocks(target,arch))
			return true;

		return false;
		}
  switch(state & ST_BLOCK)
		{
		// case ST_IMG: printf("Engine is Image.\n"); break;

		case ST_CLONE:
			return pcloneRestore(target, major, minor, type & TYPE_MASK, arch);
			break;

		case ST_FULL:
			return ddRestore(target, major, minor, type & TYPE_MASK, arch);
			break;

		// case ST_FILE: printf("Engine is fsarchiver.\n"); break;

		case ST_SWAP:
			if(readSpecificFile(arch,major,minor,1) != 1)
				{
				debug(ABORT, 0,"Damaged archive");
				return true;
				}

			return restoreSwap(target,arch);
			break;

		default:
			if(!ui_mode)
			printf("No known engine.\n");
			break;
    }

	// type determine the engine, as well
	return false;
	}

/*----------------------------------------------------------------------------
** unmount/remount image mount if we're restoring onto an image space and the
** partition is different than the original partition
*/
static bool imageRemount(disk *d)
	{
	int i;
	selection *sel = &sels[LIST_IMAGE];
	if(d == NULL)
		{
		if(ui_mode)
			{
			strcpy(image1.image,&sel->currentPath[sel->currentPathStart]);
      unmountLocation(trimPath(sel));
			if(!ui_mode)
				printf("Storing current path: %s [%i]\n",image1.image,sel->currentPathStart); // image name remains the same
      }
    else
			{
			strcpy(image2.image,&image1.dev[1]);
			image2.path = &image2.image[strlen(image2.image)] + 1;
			strcpy(image2.path,image1.path);
      *image1.dev = 0;
      unmountLocation(image1.image);
			if(!ui_mode)
				printf("Storing [%s / %s]\n",image2.image,image2.path);
      }
		}
	else
		{ // store new mount point on given disk
		for(i=0;i<d->partCount;i++)
			{
			if(d->parts[i].num == currentImage.newPartNum)
				break;
			}

			if(i == d->partCount)
				{
				debug(ABORT, 0,"Unable to find new image mount point %i",currentImage.newPartNum);
				return true;
				}

		if(!ui_mode)
			printf("Re-mounting image on %s\n",d->parts[i].deviceName);

		if(d->parts[i].type == PART_RAW)
			{
			debug(ABORT, 0,"New mount point is incorrect type");
			return true;
			}

		mountLocation(d->parts[i].deviceName,d->parts[i].type,0); // is always a partition, since we can't remount a loop device; type should be the same since the start loc should be the same
		currentImage.mount = &d->parts[i]; // isDisk remains the same; only a partition is remounted
		if(ui_mode)
			{
			sel->currentPathStart = strlen(d->parts[i].deviceName)+1;
			sprintf(sel->currentPath,"/mnt/%s/%s",&d->parts[i].deviceName[5],image1.image);
			if(!ui_mode)
				printf("Restoring current path: %s [%i]\n",sel->currentPath,sel->currentPathStart);
			}
		else
			{
			sprintf(image1.image,"/mnt/%s/%s",&d->parts[i].deviceName[5],image2.image);
			image1.dev = &image1.image[strlen(d->parts[i].deviceName)];
			image1.path = &image1.image[strlen(image1.image)]+1;
			strcpy(image1.path,image2.path);
			if(!ui_mode)
				printf("restoring: [ %s / %s ]\n",image1.image, image1.path);
			}
		}

	return false;
	}

/*----------------------------------------------------------------------------
**
*/
void locateImage(void)
	{
	selection *sel = &sels[LIST_IMAGE];

	if(ui_mode)
		sprintf(globalPath,"%s%s",sel->currentPath,sel->sources[sel->position].location);
	else
		sprintf(globalPath,"%s%s%s",image1.image,(image1.image[strlen(image1.image)-1] == '/')?"":(image1.path != NULL)?"/":"",(image1.path != NULL)?image1.path:"");

	debug(INFO, 1,"Image Name: %s\n",globalPath);
	}

/*----------------------------------------------------------------------------
**
*/
void renameImage(void)
	{
	int fd, n, offset = 0;

	locateImage();
	if(!getImageTitle(globalPath,globalBuf,NULL))
		debug(EXIT, 1,"Unable to parse %s\n",globalPath);

	if(!*currentImage.imageTitle)
		{
		debug(EXIT, 1,"Current title: %s\n",globalBuf);
		}

	if(!strcmp(currentImage.imageTitle,globalBuf))
		debug(EXIT, 1,"Title unchanged.\n");

	if((fd = open(globalPath,O_WRONLY | O_LARGEFILE)) < 0)
		debug(EXIT, 1,"Unable to write to %s\n",globalPath);

	bzero(globalBuf,256);
	strcat(globalBuf,currentImage.imageTitle);
	lseek64(fd,45,SEEK_SET); // skip the 20-byte header and 25-byte file descriptor
	while((offset < 256) && ((n = write(fd,&globalBuf[offset],256-offset)) > 0))
		{
		offset += n;
		}

	if(offset != 256)
		debug(EXIT, 1,"Error writing to %s\n",globalPath);

	close(fd);
	reSignIndex(globalPath);
	printf("Image title updated.\n");
	}

/*----------------------------------------------------------------------------
**
*/
static int reloadDrive(disk *d)
	{
	int fd;

	debug(INFO, 1,"Reloading partition table on %s\n",d->deviceName);
	fd = open(d->deviceName,O_RDONLY | O_LARGEFILE);
	if(fd < 0)
		{
		debug(INFO, 1,"Could not open %s\n",d->deviceName);
		return 1;
		}

	if(ioctl(fd,BLKRRPART) < 0)
		{
		close(fd);
		debug(INFO, 1,"Could not reload table for %s\n",d->deviceName);
		return 1;
		}

	close(fd);
	return 0;
	}

/*----------------------------------------------------------------------------
**
*/
static char *assignMap(
		void           *dev,
		bool            isDisk,
		int            *major,
		int            *minor,
		unsigned char  *state,
		unsigned char  *type,
		char          **label
		)
	{
	disk *d;
	partition *p;
	if(isDisk)
		{
		d = (disk *)dev;
		*state = d->state;
		*type = d->diskType;
		*major = d->major;
		*minor = 0;
		*label = d->diskLabel;
		return d->deviceName;
		}
	else {
		p = (partition *)dev;
		*state = p->state;
		*type = p->type;
		*major = p->major;
		*minor = p->num;
		*label = p->diskLabel;
		return p->deviceName;
		}
	}

/*----------------------------------------------------------------------------
**
*/
static bool reloadPartition(
		bool hasRestore,
		disk *d,
		char *devName,
		int major,
		int minor,
		unsigned char state,
		unsigned char type,
		archive *arch
		)
	{
	int bytes;
	bool nogo;

	debug(INFO, 5,"Reload required of device %s, compare against d->map\n",d->deviceName);
	if(mbrBuf != NULL)
		{
		free(mbrBuf);
		mbrBuf = NULL;
		} // just in case

	if(hasRestore)
		{
		if(!ui_mode)
			printf("Remount required of image mount.\n");

		if(!imageBuffered)
			{
			if(readSpecificFile(arch, major, minor,1) != 1)
				{
				debug(ABORT, 0,"Damaged archive");
				return true;
				}

			if(!readFromDisk(NULL,0,0,arch,0))
				{
				debug(ABORT, 0,"Buffer read error");
				return true;
				}

			if(arch->originalBytes > MAX_MBRBUF)
				{
				debug(ABORT, 0,"MBR buffer too large");
				return true;
				}

			bytes = arch->originalBytes;
			if((mbrBuf = malloc(bytes)) == NULL)
				debug(EXIT, 0,"Could not allocate MBR buffer space.\n");

			if(readSpecificFile(arch,major,minor,1) != 1)
				{
				debug(ABORT, 0,"Damaged archive");
				return true;
				}

			if(readFile(mbrBuf,bytes,arch,1) != arch->originalBytes)
				{
				debug(ABORT, 0,"Could not read MBR into buffer");
				return true;
				}
			state |= BUFFERED;
			}

		closeArchive(arch);
		if(!testMode && imageRemount(NULL))
			return true; // unmount restore partition
		}

	if(!testMode && diskMounted(d))
		{
		debug(ABORT, 0,"Disk still mounted");
		return true;
		}

	nogo = restore(&devName[5],"",major,minor,d->deviceName,state,type,arch); // no label for MBR
	if(mbrBuf != NULL)
		{
		free(mbrBuf);
		mbrBuf = NULL;
		}

	if(nogo)
		return true;

	if(testMode)
		{
		debug(EXIT, 0,"Unable to re-map disk at this time in test mode.\n");
		// we need to modify d to contain the entries of d->map
		}
	else if(reloadDrive(d))
		{
		debug(ABORT, 0,"Could not reload drive");
		return true;
		}

	if(!testMode && validateNewTable(d))
		{
		debug(ABORT, 0,"New table does not match");
		return true;
		}

	if(hasRestore)
		{ // remount restore partition, modify navigation location, etc.
		if(!testMode && imageRemount(d))
			return true;

		locateImage();
		if(readImageArchive(globalPath,arch) != 1)
			{
			debug(ABORT, 0,"Unable to read re-mounted archive %s",globalPath);
			return true;
			}

		state &= ST_BLOCK;
		if(state != ST_IMG)
			{ // write entire MBR bits here, since the buffer would only have done the primary table
			return restore(&devName[5],"",major,minor,d->deviceName,state,type,arch);
			}
		}

	return false;
	}

/*----------------------------------------------------------------------------
**
*/
static unsigned long findExpectedSize(int major, int minor, archive *arch)
	{
	if(major == loopDrive + 1)
		major = 0;

	if(readSpecificFile(arch,major,minor,1) != 1)
		return 0; // couldn't find this

	return arch->expectedOriginalBytes;
	}

/*----------------------------------------------------------------------------
**
*/
static bool restoreDisk(disk *d, archive *arch, unsigned long archSize)
	{
	char *map, *flag;
	int mapType = determineMapType(d,&map); // complete, direct, loop, etc.
	flag = mapTypes[mapType];
	int i, pass, count = 0;
	char reloadCondition = 0;
	unsigned char fieldEntry;
	int major = 0, minor = 0;
	unsigned char *devName;
	unsigned char state, type;
	char *label;
	int partCount = d->partCount;
	partition *parts = d->parts;
	unsigned long totalSize = 0;
	char *restoreLocation;
	unsigned char restoreType;

	if(restrictedMap(mapType))
		{
		if(!ui_mode)
			printf("Skipping %s; map restriction does not match '%s'.\n",d->deviceName,(mapType)?flag:"none");

		return false;
		}

	validOperation = 1;
	for(pass = 0; pass < 2; pass++)
		{ // first pass is for figuring out total size of disk image for feedback purposes
		if(pass)
			{
			if(restoreLocation != NULL)
				{
				count++;
				totalSize += archSize;
				}

			readableSize(totalSize);
			if(count > 1)
				progressBar(totalSize,CLR_YB,PROGRESS_INIT | 1);  // OTHER (global) progress bar
			else
				progressBar(0,0,PROGRESS_INIT | 1); // reset global total
			} // set the expected size

		restoreLocation = NULL;
		if(d->map != NULL)
			{ // restore drive
			count = 1;
			reloadCondition = hasMountMismatch(NULL,d); // will result in '4' if there are non-restore mounts so we shouldn't worry about partition table boundary issues
			if((d->state & ST_BLOCK) == SI_PART)
				{
				devName = assignMap(d->map,false,&major,&minor,&state,&type,&label);
				partCount = 0;
				if(type == PART_RAW)
					type = DISK_RAW;
				}
			else
				{
				devName = assignMap(d->map,true,&major,&minor,&state,&type,&label);
				partCount = ((disk *)d->map)->partCount;
				parts = d->auxParts;
				}

/* TODO ABJ - Alignment */


		if(pass)
			{
			if(!ui_mode)
				printf("Restoring %s (%s, %s) to drive %s.\n",&devName[5],flag,readable,&d->deviceName[5]);
			}

// if (!strcmp(d->deviceName,"/dev/sdi") && currentImage.newPartNum) reloadCondition |= 2 | 8; // TEST TEMPORARY; FORCES BUFFERING AND RELOAD OF IMAGE DRIVE EVEN IF PART TABLE IS IDENTICAL
		if(pass)
			{
			if(reloadCondition & 8)
				{ // have to reload the table after we're done
				if(reloadPartition((reloadCondition & 2),d,devName,major,minor,state,type,arch))
					return true; // interrupted
				}
			else
				{
				if(restore(&devName[5],label,major,minor,d->deviceName,state,type,arch))
					return true; // interrupted

				if(add_img && !(type & DISK_MASK) && !strcmp(label,SYSLABEL))
					{
					restoreLocation = d->deviceName;
					restoreType = type;
					}
				}
			}
		else
			{
			if(!(reloadCondition & 8) && add_img && !(type & DISK_MASK) && !strcmp(label,SYSLABEL))
				restoreLocation = d->deviceName; // add to totalSize

			totalSize += findExpectedSize(major,minor,arch);
			}
		}
	else if(pass)
		{
		if(!ui_mode)
			printf("Restoring individual partition%s (%s, %s) to %s.\n",(count > 1)?"s":"",flag,readable,&d->deviceName[5]);
		}

	for(i=0;i<partCount;i++)
		{
		partition *pt = &parts[i];
		if(pt->map == NULL)
			continue; // nothing to do

		count++; // another part
		if((pt->state & ST_BLOCK) == SI_LOOP)
			{
			devName = assignMap(pt->map,true,&major,&minor,&state,&type,&label);
			if(type & DISK_MASK)
				type = PART_RAW;
			}
		else
			devName = assignMap(pt->map,false,&major,&minor,&state,&type,&label);

// NEED TO FIGURE OUT PROPER DEVICENAME BASED ON non-mapped pt->num in the NEW world order
		if(pass)
			{
			if(restoreLocation == NULL && add_img && !(type & DISK_MASK) && !strcmp(label,SYSLABEL))
				{
				restoreLocation = d->parts[i].deviceName;
				restoreType = type;
				}

			if(restore(&devName[5],label,major,minor,d->parts[i].deviceName,state,type,arch))
				return true; // interrupted
			}
		else
			totalSize += findExpectedSize(major,minor,arch);
			}
		}

// one restore partition allowed per disk, onto which we restore the image
	if(restoreLocation != NULL && !testMode)
		{
		if(!mountLocation(restoreLocation,restoreType,0))
			{
			debug(ABORT, 0,"Unable to mount restore location");
			return true;
			}
		locateImage();
		flag = strrchr(globalPath,'/');
		if(flag == NULL)
			flag = globalPath;
		else
			flag++;

		sprintf(globalPath2,"%s/%s",restoreLocation,flag);
		memcpy(globalPath2,"/mnt/",5);

    // wclear(wins[WIN_PROGRESS]); // clear previous window
		prepareOperation("ADDING IMAGE TO RESTORE PARTITION");
		debug(INFO, 1,"Copying image from %s to %s\n",globalPath,globalPath2);
		i = copyImage(restoreLocation,globalPath,globalPath2,true);
		if(options & OPT_APPEND)
			customFilesAppend(globalPath,globalPath2);

		globalPath2[strlen(restoreLocation)] = 0;
		unmountLocation(globalPath2);
		if(i != 1)
			return true; // something went wrong/was cancelled
		}
	else if(restoreLocation != NULL)
		{
		if(!ui_mode)
			printf("img => %s\n",&restoreLocation[5]);
		}

	return false;
	}

/*----------------------------------------------------------------------------
**
*/
char performRestore(void)
	{
  int i, j;
  char isSelected = 0;
	unsigned long archSize;
  selection *sel = &sels[LIST_DRIVE];
  disk *d;

	// if (ui_mode) endwin();

	archive readArch;
	locateImage();
	if(!archiveSegments(globalPath,&archSize))
		{
		debug(ABORT, 0,"Invalid archive");
		return 1;
		}

	// PLACE IMAGE INTO RAM BUFFER
	imageBuffered = false; // not in RAM
	if(readImageArchive(globalPath,&readArch) != 1)
		{
		debug(ABORT, 0,"Invalid archive");
		return 1;
		}

	for(i=0;i<sel->count;i++)
		{
    if(i == sel->position || sel->sources[i].state & ST_SEL)
			{
      d = (disk *)sel->sources[i].location;
      if(d->state & ST_SEL)
				{ // implies d->map
				if(restoreDisk(d,&readArch,archSize))
					{
					closeArchive(&readArch);
					if(ui_mode)
						return 1;

					exit(1);
					}

				isSelected = 1;
				}
      else
				{ // just make sure there's something to restore; otherwise don't call restoreDisk
        for(j=0;j<d->partCount;j++)
					{
          if(d->parts[j].state & ST_SEL)
						{
						if(restoreDisk(d,&readArch,archSize))
							{
							closeArchive(&readArch);
							if(ui_mode)
								return 1;

							exit(1);
							}

						isSelected = 1; break;
						}
          }
        }
      }
    }
	if(ui_mode)
		progressBar(0,readArch.originalBytes,PROGRESS_COMPLETE);

	closeArchive(&readArch);
	if(ui_mode)
		{
	//	printf("Done.\n");
	//	getch();
	//	exitConsole();
	// need to navigate something, somewhere, especially if we remounted image
		}

	return isSelected;
	}
