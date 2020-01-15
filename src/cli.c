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
  /* ANSI/POSIX */
  #define __USE_LARGEFILE64
  #define _LARGEFILE64_SOURCE
  #define _GNU_SOURCE
  #include <errno.h>
  #include <fcntl.h>
  #include <linux/reboot.h>
  #include <ncurses.h>
  #include <signal.h>
  #include <stdlib.h>
  #include <string.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  /* System Restore */
  #include "backup.h"				// backupPID
  #include "cli.h"          // Validate self-compatibility.
  #include "drive.h"				// mapTypes
  #include "fileEngine.h"		// arch_md_len
  #include "frame.h"				// expert
  #include "image.h" 				// iset, loopDrive, imageSize
  #include "mount.h"				// trackMount, currentLine
  #include "partition.h"
  #include "position.h"			// busyMap
  #include "restore.h"  		// performRestore(), renameImage(), locateImage(), globalPath2
  #include "sysres_debug.h"	// SYSRES_DEBUG_level
  #include "sysres_linux.h" // SYSRES_LINUX_SetKernelLogging()
  #include "sysres_signal.h"  // SYSRES_SIGNAL_SIGINT_Handler()
  #include "sysres_xterm.h" // SYSRES_XTERM_CURSOR_ON
  #include "window.h"     	// globalPath, options, mode, currentFocus, setup, modeset, shellset, performVerify, performValidate

/*----------------------------------------------------------------------------
** Macro values
*/
#define STARTDELAY 2      // 2 seconds to discover connected USB devices
#define SYSRESINIT "/.sysresinit"
#define MAX_ARGS   30

/*----------------------------------------------------------------------------
** Storage
*/
  /* Global (Exported by cli.h) */
	char 			driveSelected		= 0; // set this if a drive was selected for command-line operation
	char 			ui_mode       	= 0; // 0 = text mode, 1 = ncurses
	char 			add_img        	= 0; // automatically copy restore image to any drive with a label of SYSLABEL
	char 			testMode       	= 0;
	char      validOperation 	= 0;
	navImage 	image1;
	navImage 	image2;

	/* Local */
	int                    filterCount        = 0;
	int                    partCount          = 0;
	char                   localmount         = 0;
	char                   show_list          = 0; // show list; don't do anything; 2 == show detail, not list (only relevant for images), 4 = show detail w/verify
	unsigned char          mapMask            = 0; // 8 bits, 8 types of maps
	char                   makedir            = 0; // 1 = make any directories that aren't there
	int                    doSelect           = -1;
	char                   force              = 0;
	int                    segment_size       = 0;
	char                   compression        = GZIP; // default; change with compression= option
	char                  *cifsUser           = NULL;
	char                  *cifsPass           = NULL;
	unsigned int           usbDelay           = 0;
	bool                   kernelMode         = false;
	volatile pthread_t     threadTID          = 0;
  char                  *kernelCmdBuffer;
  char                  *kernelArgv[MAX_ARGS];
	char                   drive_filter[MAXDISK][MAXDRIVES]; // drives= option to restrict drives included in scan
	char                   part_filter[MAXPDEF][MAXPART]; // source or target partition definitions
	navImage               preScript;
  navImage               postScript;

/*----------------------------------------------------------------------------
**
*/
void usage(const char *I__programPath)
	{
  int   rCode = EXIT_SUCCESS;
	const char *programName;

  /* Validate caller args. */
  if(!I__programPath)
    {
    rCode=EINVAL;
    goto CLEANUP;
    }

  /* Parse the programName from the I__programPath */
  programName = strrchr(I__programPath, '/');
	if(programName)
		programName++;
	else
		programName = I__programPath;

	fprintf(stderr,"\nUsage: %s [ui] [backup|list|rename|restore|verify] <options>\n\n", programName); // transfer
	fprintf(stderr,"       backup source=... target=<image> desc=<title> segment=<MB> compression=[none|zlib|lzma]\n");
	fprintf(stderr,"       detail | <list [restore...|backup...]>\n");
  fprintf(stderr,"       rename source=<image> desc=<title>\n");
  fprintf(stderr,"       restore source=<image> target=... [--addimg]\n");
	fprintf(stderr,"       verify [list|detail] source=<image>\n\n");
	fprintf(stderr,"       <image>=//label/<path>,/dev/<device>/<path>,<path>\n");
	fprintf(stderr,"       drives=<device,...>  (limits the disks to scan)\n");
  fprintf(stderr,"       restrict=<complete,incomplete,direct,loop,restore,backup,partial,custom>\n");
  fprintf(stderr,"       source=<drive>:<map><mbr|part|//label> (map: blank=default, -=remove, or:)\n");
	fprintf(stderr,"              mbr: *=mbr only, #=mbr+32KB, +=mbr+32MB, x=mbr+all free space\n");
  fprintf(stderr,"              part: *=IMG, #=CLONE, +=FILE, x=FULL, X=MAX\n");
  fprintf(stderr,"       target=<img#>[:<part#>]@<drive>[:<partition>]\n\n");
  fprintf(stderr,"       --addimg       after restoring, copy image to %s partition\n",SYSLABEL);
  fprintf(stderr,"       --anypart      allow mounting any partition for images\n");
	fprintf(stderr,"       --append       include special files when copying images\n");
	fprintf(stderr,"       --auto         autoselect (if not specified) and restore/backup drive(s)\n");
	fprintf(stderr,"       --buffer       copy image to buffer first (restore mode)\n");
  fprintf(stderr,"       --debug        show additional information to debug issues\n");
	fprintf(stderr,"       --delay        wait %i seconds for USB drives to settle (can use multiple times)\n",STARTDELAY);
  fprintf(stderr,"       --force        over-write existing archive\n");
	fprintf(stderr,"       --halt         same as --poweroff\n");
  fprintf(stderr,"       --help         this usage screen\n");
  fprintf(stderr,"       --license      display the license component of this program\n");
  fprintf(stderr,"       --makedir      creates target directories\n");
	fprintf(stderr,"       --poweroff     power off after successful completion\n");
  fprintf(stderr,"       --ramdisk      mount the ram disk\n");
	fprintf(stderr,"       --reboot       reboot after successful completion\n");
  fprintf(stderr,"       --test         don't perform any backup/restore operations\n");
  fprintf(stderr,"       --version      display the version of this program\n\n");

CLEANUP:

	exit(rCode);
	}

/*----------------------------------------------------------------------------
**
*/
void prefixPath(char *path, char *item)
	{
	int offset = strlen(item);

	if((strlen(path)+offset) >= MAX_PATH-1)
		debug(EXIT, 1,"Path value exceeds length of %i.\n",MAX_PATH-1-offset);

	memmove(&path[offset],path,strlen(path)+1); // include terminator
	strncpy(path,item,offset);

	return;
	}

/*----------------------------------------------------------------------------
**
*/
void localizeDir(navImage *loc, char localize)
	{
	if(loc->path == NULL)
		{
		prefixPath(loc->image,".//");
		loc->path = &loc->image[2];
		}
	else if(localize)
		{ // add an extra '/' before the filename, if it's a local mount ONLY
		memmove(&loc->path[1],loc->path,strlen(loc->path)+1);
		loc->path++;
		}

	*loc->path++ = 0;  // don't do this the second time around

	return;
	}

/*----------------------------------------------------------------------------
**
*/
void dirChain(char *path)
	{
	char *tmp = path;

	if(path == NULL)
		return;

	while((tmp = strchr(&tmp[1],'/')) != NULL)
		{
		*tmp = 0;
		mkdir(path,0);
		*tmp++ = '/';
		}

	return;
	}

char *trimPath(selection *sel) {
	sel->currentPath[sel->currentPathStart-1] = 0;
	return sel->currentPath;
	}

void unmountExit(int code) {
	if (trackMount & 1) { unmountLocation(trimPath(&sels[LIST_IMAGE])); debug(INFO, 1,"Unmounted %s\n",sels[LIST_IMAGE].currentPath); }
	if (trackMount & 2) { unmountLocation(trimPath(&sels[LIST_SOURCE])); debug(INFO, 1,"Unmounted %s\n",sels[LIST_SOURCE].currentPath); }
	if (trackMount & 4) { unmountLocation(trimPath(&sels[LIST_TARGET])); debug(INFO, 1,"Unmounted %s\n",sels[LIST_TARGET].currentPath); }
        if (trackMount & 16) { *image1.dev = 0; unmountLocation(image1.image); debug(INFO, 1,"Unmounted %s\n",image1.image); }
        if (trackMount & 32) { *image2.dev = 0; unmountLocation(image2.image); debug(INFO, 1,"Unmounted %s\n",image2.image); }
	trackMount = 0; // clear all
	if (code != -1) {
		if (!code) {
			if (options & (OPT_REBOOT | OPT_HALT)) { // halt or reboot instead of exiting
				printf((options & OPT_HALT)?"Powering down.\n":"Rebooting.\n");
				reboot((options & OPT_HALT)?LINUX_REBOOT_CMD_POWER_OFF:LINUX_REBOOT_CMD_RESTART);
				}
			}
		SYSRES_LINUX_SetKernelLogging(SYSRES_LINUX_KERN_LOGMODE_EXIT_D); exit(code);
		}
	}

/*----------------------------------------------------------------------------
**
*/
int mapLocation(navImage *loc, selection *sel, char localize, bool isScript)
	{ // mode is known
	char *image = loc->image;
  struct stat64 statBuf;
	int i;
  char state = 0; // 0 == not found, 1 == dir ok, file not found, 2 == dir ok, file found, 3 = isDir, 4 = isImage
	char *user, *pass; // for CIFS mounts

  if(image == NULL || *image == -1 || !*image)
		return 0; // ignore empty/blank set

  if(!strncmp(image,"/dev/",5) || ((options & OPT_RAMDISK) && (!strcmp(image,"/ram") || !strncmp(image,"/ram/",5))))
		return 0; // scan after mounting later

	if(!strncmp(image,"//",2))
		return 0; // label map; scan after mounting later

	if(!strncmp(image,"http://",7))
		return 0; // scan later

	if(!strncmp(image,"cifs://",7))
		return 0; // scan after mounting later

  // check if loc is a directory, a file, or doesn't exist, to determine local mount location
	if(loc->path != NULL)
		{
		*--loc->path = '/';
		} // re-introduce path


	loc->path = strrchr(image,'/');
  if(!stat64(image,&statBuf))
		{ // see if it's a file or directory
		if(S_ISDIR(statBuf.st_mode))
			{
			state = 3;
			loc->path = NULL;
			if(image[strlen(image)-1] != '/')
				{
				if(strlen(image)+1 >= MAX_PATH-1)
					debug(EXIT, 1,"Source value exceeds path length of %i.\n", MAX_PATH-2);

				strcpy(&image[strlen(image)],"/"); // adds the trailing '/'
				}
			}
    else
			{ // it's a file (check if it's an image FILE once we implement that feature)
			state = (statBuf.st_mode & S_IFREG && statBuf.st_size > 11 && getType(image) == 1)?4:2;
			if(state == 4 && mode == BACKUP && force)
				{  // need to keep this, otherwise the path gets mangled
				if(!unlink(image))
					{
					debug(INFO, 1,"Force removal of previous image %s\n",image);
					state = 1;
					}
				}

			localizeDir(loc,localize);
    	}
    }
   else
		{ // file doesn't exist; see if the enclosing directory exists
		localizeDir(loc,localize);
		if (stat64(image,&statBuf))
			{
			if(makedir)
				{
				dirChain(image);
				state = 1;
				}  // mkdir -p
			}
		else if(S_ISDIR(statBuf.st_mode))
			state = 1;
    }

	if(state == 1)
		{
		strcpy(currentImage.imageName,loc->path);
    strcpy(currentImage.imagePath,image);
    if(image[strlen(image)-1] != '/')
			strcat(currentImage.imagePath,"/"); // need this for mounts
		}

	if(isScript)
		return((state != 2)?1:0);
	else
		{
		if(!state)
			{
			debug(INFO, 1,"Unable to find path.\n");
			fprintf(stderr,"Unable to find path.\n");
			unmountExit(1);
			}

		if(state == 1 && (mode != BACKUP))
			{
			debug(INFO, 1,"Unable to find file.\n");
			fprintf(stderr,"Unable to find file.\n");
			unmountExit(1);
			}

		if(state == 2 && (mode == BACKUP) && (!force))
			{
			debug(INFO, 1,"A filename already exists with that name.\n");
			fprintf(stderr,"A filename already exists with that name.\n");
			unmountExit(1);
			}

		if(state == 4 && (mode == BACKUP) && (!force))
			{
			debug(INFO, 1,"An image archive already exists with that name.\n");
			fprintf(stderr,"An image archive already exists with that name.\n");
			unmountExit(1);
			}
		}

	if(sel != NULL)
		{ // navigate and place selector (mostly UI mode)
		// configure the list
		if(!strcmp(sel->currentPath,currentImage.imagePath))
			{ // need to re-load navigator
			*sel->currentPath = sel->currentPathStart = 0;
			navigateDirectory(image,sel);
			}
		else
			{ // only navigate if it's past the top-level mount
			if(state != 4 || strcmp(&image[sel->currentPathStart],loc->path))
				{
				navigateDirectory(&image[sel->currentPathStart],sel);
				}
			}

		if(state == 2 || state == 4)
			{ // navigate to existing file/archive
			for(i=0;i<sel->count;i++)
				{
        if(!strcmp(loc->path,sel->sources[i].location))
					{
					sel->position = i;
					break;
					}
				}
			}
		}

	return 0;
	}

/*----------------------------------------------------------------------------
**
*/
void addArgEntry(char *prefix, char *val, char *suffix, char argType)
	{
	int       i;
	char      map[11];
	int       argLen;
	navImage *img;

	if(prefix == NULL)
		prefix = "";

	if(suffix == NULL)
		suffix = "";

	argLen = strlen(val) + strlen(prefix) + strlen(suffix);
	if(!argLen)
		return;

	if(argType == 0)
		{ // drives=
		if(argLen >= MAXDISK)
			debug(EXIT, 1,"Device name length exceeds %i.\n",MAXDISK);

		if(filterCount >= MAXDRIVES)
			debug(EXIT, 1,"Number of drives exceeds %i.\n",MAXDRIVES);

		sprintf(drive_filter[filterCount],"%s%s%s",prefix,val,suffix);

		if(!strncmp(drive_filter[filterCount],"/dev/",5))
			memmove(drive_filter[filterCount],&drive_filter[filterCount][5],argLen-4); // remove /dev/, include terminating 0

		filterCount++;
		}
	else if(argType == 3)
		{ // restrict=
		if((strlen(prefix) + strlen(val) + strlen(suffix)) > 10)
			debug(EXIT, 1,"Unrecognized map type %s%s%s\n",prefix,val,suffix);

		sprintf(map,"%s%s%s",prefix,val,suffix);
		for(i=1;i<9;i++)
			{
			if(!strcmp(map,mapTypes[i]))
				break;
			}

		if(i == 9)
			debug(EXIT, 1,"Unrecognized map type %s\n",map);

		mapMask |= 1 << i-1;
		}
	else if((argType == 1 && mode == BACKUP) || (argType == 2 && mode == RESTORE) || (argType == 2 && mode == ERASE))
		{ // list of drives/partitions to backup from, or restore to sda:mbr, sda, sda:1, sda:0 (nothing selected but the drive, etc.
		if(argLen >= MAXPDEF)
			debug(EXIT, 1,"Partition %s length exceeds %i.\n",MAXPDEF);

		if(partCount >= MAXPART)
			debug(EXIT, 1,"Partition count exceeds %i.\n",MAXPART);

		sprintf(part_filter[partCount],"%s%s%s",prefix,val,suffix);

		if(!strncmp(part_filter[partCount],"/dev/",5))
			memmove(part_filter[partCount],&part_filter[partCount][5],argLen-4); // include terminating 0

		partCount++;
		}
	else if((argType == 1 && (mode == RESTORE || show_list || mode == ERASE)) || (argType == 2 && mode == BACKUP) || (argType && mode == TRANSFER))
		{ // single filename, location, path or erase type
		if(argLen >= MAX_PATH-1)
			debug(EXIT, 1,"Parameter length exceeds %i.\n",MAX_PATH-1);

		img = (argType == 2 && mode == TRANSFER)?&image2:&image1;
		sprintf(img->image,"%s%s%s",prefix,val,suffix);
		if(mode == ERASE)
			{ ; } // match against available erase types
		else
			mapLocation(img,NULL,1,false);  // isolate non-transfer mode here, if transfer is ever implemented
		// append to image array if source list (data migration not being worked on right now)
		// do /net/ later, once we figure out the formatting
		}
	else if(mode == LIST)
		debug(EXIT, 1,"List mode does not accept target argument.\n");

	return;
	}

/*----------------------------------------------------------------------------
**
*/
void readValues(char *val, char argType)
	{
	int count = 0;
	char *prefix = NULL;
	char *suffix = NULL;
	char *nextField = val;
	char *suffixField = NULL;
	char *tmp;
	int local = 0;

	if(argType && (mode == -1))
		if(!(show_list))
			debug(EXIT, 1,"Please specify a mode.\n");
		else mode = LIST;

	if(mode == TRANSFER || (mode == BACKUP && argType == 2) || (mode == RESTORE && argType == 1) || (mode == ERASE && argType == 1))
		{
		addArgEntry(NULL,val,NULL,argType);
		return;
		}

	while(nextField != NULL)
		{
		if((nextField = strchr(val,',')) != NULL)
			*nextField++ = 0;

		if(prefix == NULL && ((tmp = strchr(val,'[')) != NULL))
			{
			*tmp++ = 0;
			prefix = val;
			if((suffix = strchr(tmp,']')) != NULL)
				{
				*suffix++ = 0;
				local = 1;
				} // suffix already terminated
			else if(nextField != NULL)
				{ // find suffix outside of comma list
				if((suffix = strchr(nextField,']')) != NULL)
					{
					*suffix++ = 0;
					if((suffixField = strchr(suffix,',')) != NULL)
						*suffixField++ = 0; // find next comma delimiter, if any
					}
				}
			}
		else
			tmp = val;

		addArgEntry(prefix,tmp,suffix,argType);
		if(local)
			{
			prefix = suffix = NULL;
			local = 0;
			}

		val = nextField;
		if(val == NULL)
			{
			val = nextField = suffixField;
			suffixField = prefix = suffix = NULL;
			} // reset things
		}
	}

/*----------------------------------------------------------------------------
**
*/
void startCount(void)
	{
	int i;
	float remaining = usbDelay*1.0;

	SYSRES_SIGNAL_hasInterrupted = 0;
	printf("%s", SYSRES_XTERM_CURSOR_OFF);
	for(i=0;i<usbDelay*10;i++)
		{
		if(SYSRES_SIGNAL_hasInterrupted)
			break;

		printf("\rPausing for USB discovery: %s%.1f%s  ",
			SYSRES_XTERM_COLOR_GREEN_D,
			remaining,
			SYSRES_XTERM_COLOR_RESET_D
			);
		fflush(stdout);
		usleep(100000);
		remaining -= 0.1;
		}

	printf("\n" SYSRES_XTERM_CURSOR_ON);

	return;
	}

/*----------------------------------------------------------------------------
** #define setMode(X) { if (mode != -1) debug(EXIT, 1,"Please specify only one mode.\n"); mode = X; }
*/
void setMode(int I__mode)
	{
	if(mode != -1)
		debug(EXIT, 1,"Please specify only one mode.\n");

	mode = I__mode;

	return;
	}

/*----------------------------------------------------------------------------
**
*/
void readOption(char *param, char *val)
	{
	int count = 0;
	char *nextField;

	if(strcmp(param,"ui") == 0)
		modeset = 1; // implies ui_mode before navigator is called

	else if(val == NULL)
		{
		if      (!strcmp(param,"restore"))
			setMode(RESTORE);
		else if(!strcmp(param,"backup"))
			setMode(BACKUP);
		else if(!strcmp(param,"erase"))
			setMode(ERASE);
		else if(!strcmp(param,"transfer"))
			setMode(TRANSFER);
		else if(!strcmp(param,"list"))
			show_list |= 1;
		else if(!strcmp(param,"detail"))
			show_list |= 2; // show compression info rather than partition info
		else if(!strcmp(param,"verify"))
			show_list |= 4; // verify (list/detail imply only show the sha1sum)
		else if(!strcmp(param,"rename"))
			show_list |= 8;
		else if(!strcmp(param,"--auto"))
			options |= OPT_AUTO; // select F1, F2 if available
		else if(!strcmp(param,"--reboot"))
			options |= OPT_REBOOT; // reboots after POST or finishing an backup/restore operation
		else if(!strcmp(param,"--poweroff") || !strcmp(param,"--halt"))
			options |= OPT_HALT; // halts after POST or finishing a backup/restore operation
		else if(!strcmp(param,"--delay"))
			usbDelay += STARTDELAY;
		else if(!strcmp(param,"--ramdisk"))
			options |= OPT_RAMDISK;
		else if(!strcmp(param,"--buffer"))
			options |= (OPT_BUFFER | OPT_RAMDISK); // implies --ramdisk
		else if(!strcmp(param,"--append"))
			options |= OPT_APPEND;
		else if(!strcmp(param,"--makedir"))
			makedir = 1;
		else if(!strcmp(param,"--anypart"))
			options |= OPT_ANYPART;
		else if(!strcmp(param,"--test"))
			testMode = 1; // don't do any backup/restore/erase/copy operations
		else if(!strcmp(param,"--addimg"))
			add_img = 1; // required to copy image onto RESTORE partition
		else if(!strcmp(param,"--force"))
			force = 1; // delete existing archive; must be prior to the target= call
		else if(!strcmp(param,"--version"))
			show_list |= 16; // print version, but continue parsing
		else if(!strcmp(param,"--license"))
			showLicense(true);
		else if(!strcmp(param,"--debug"))
			SYSRES_DEBUG_level = SYSRES_DEBUG_level_10_D;
		else if(kernelMode)
			debug(INFO, 1,"Ignoring kernel parameter %s\n",param);
		else
			debug(EXIT, 1,"Unknown parameter %s. Use --help for a list of options.\n",param);
		}
	else if(!strcmp(param,"drives"))
		{
		if(filterCount)
			debug(EXIT, 1,"Only one drives argument is permitted.\n");

		readValues(val,0);
		}
	else if(!strcmp(param,"source"))
		{
		if(mode != TRANSFER && *image1.image && mode != BACKUP)
			debug(EXIT, 1,"Only one source argument is permitted.\n");

		// if mode == TRANSFER, special case
		readValues(val,1);
		}
	else if(!strcmp(param,"target"))
		{
		if((*image2.image == -1 && *image1.image && mode != RESTORE) || (*image2.image && *image2.image != -1))
			debug(EXIT, 1,"Only one target argument is permitted.\n");

		readValues(val,2);
		}
	else if(!strcmp(param,"segment"))
		{
		if(!*val)
			debug(EXIT, 1,"Segment size cannot be blank\n");

		if(*val < '0' || *val > '9' || ((segment_size = atoicheck(val)) < 1))
			debug(EXIT, 1,"Segment size must be a positive number\n");
		}
	else if(!strcmp(param,"compression"))
		{
		if(!strcmp(val,"none"))
			compression = 0;
		else if(!strcmp(val,"zlib"))
			compression = GZIP;
		else if(!strcmp(val,"lzma"))
			compression = LZMA;
		else
			debug(EXIT, 0,"Specify a compresion of none, zlib or lzma\n");
		}
	else if(!strcmp(param,"restrict"))
		{
		readValues(val,3);
		}
	else if(!strcmp(param,"desc"))
		{ // blank=just display filename
		if(*currentImage.imageTitle)
			debug(EXIT, 1,"Only one description is permitted.\n");

		if(!*val)
			debug(EXIT, 1,"Description cannot be blank.\n");

		if(strlen(val) > 255)
			debug(EXIT, 1,"Description is limited to 255 characters.\n");

		strcpy(currentImage.imageTitle,val);
		}
	else if(!strcmp(param,"pre"))
		{ // run this file prior to performing an image operation (file MUST exist to continue)
		if(!*val)
			return; // empty value; ignore
		if(options & OPT_PRE)
			debug(EXIT, 1,"Only one pre argument permitted\n");

		if(strlen(val) >= MAX_PATH-1)
			debug(EXIT, 1,"Pre parameter exceeds maximum length\n");

		options |= OPT_PRE;
		strcpy(preScript.image,val);
		printf("PRE: %s\n",val);
		}
	else if(!strcmp(param,"post"))
		{  // run this file after a successful image operation (file MUST exist for a successful entry)
		if(!*val)
			return; // empty value; ignore

		if(options & OPT_POST)
			debug(EXIT, 1,"Only one post argument permitted\n");

		if(strlen(val) >= MAX_PATH-1)
			debug(EXIT, 1,"Post parameter exceeds maximum length\n");

		options |= OPT_POST;
		strcpy(postScript.image,val);
		printf("POST: %s\n",val);
		}
	else if(kernelMode)
		debug(INFO, 1,"Ignoring kernel parameter %s.\n",param);
	else
		debug(EXIT, 1,"Unknown value pair parameter %s. Use --help for a list of options.\n",param);

	return;
	}

/*----------------------------------------------------------------------------
**
*/
int findMount(navImage *loc, void *p, selection *sel, char isDisk)
	{
	char tmp = 0, tmpc, res;
	char *src = loc->image;
	char *mpoint, *cifsshare, *cifsdir;
	char *dev;
	char *label;
	unsigned char type;
  unsigned char scriptType;

	if(*src == -1 || *src == 0)
		return 1; // empty descriptor; consider them as succesful as they're ignored by mapLocation later

	if(sel != NULL)
		{
		sel->count = 1;
		sel->position = 0;
		if(sel->count > sel->size && expandSelection(sel,0))
			return 0; // can't add more

		sel->sources[0].diskNum = -2; // implies location is a string
		}

	if((options & OPT_RAMDISK) && (!strcmp(src,"/ram") || !strncmp(src,"/ram/",5)))
		{ // mount and match RAM, if possible; of /ram mounts, change to /mnt/ram
		if(sel != NULL)
			{
			sel->sources[0].location = "/ram";
			res=mountLocation(sel,0,0);  // ram mount; no type needed
			}
		else
			res=mountLocation("/ram",1,0); // type is irrelevant here as long as it's not 0

		if(res)
			prefixPath(src,"/mnt");

		return res;
		}
	else if(strncmp(src,"/dev/",5) && strncmp(src,"//",2))
		{
		loc->local = 1;
		if(sel != NULL)
			{
			sel->sources[0].location = src;
			mountLocation(sel,0,0);  // local mount; no type needed
			}

		return 1; // always accept locals
		} // must be /dev/ or //, if not /ram
	else if(p == NULL)
		return 0;

	dev   = (isDisk) ? ((disk *)p)->deviceName : ((partition *)p)->deviceName;
	label = (isDisk) ? ((disk *)p)->diskLabel  : ((partition *)p)->diskLabel;
	type  = (isDisk) ? ((disk *)p)->diskType   : ((partition *)p)->type;

	// device label mounting
	if(strncmp(src,"/dev/",5))
		{ // check for device label
		if(label == NULL || !*label || !strcmp(label,"/"))
			return 0; // no label known, / not allowed for this match

		if(!strncmp(src,"//",2) && !strncmp(label,&src[2],strlen(label)))
			{ // match a label
			tmp = strlen(label)+2;
			tmpc = src[tmp];
			}
		else
			return 0;
		}
	else if(strncmp(src,dev,strlen(dev)))
		return 0; // no match
	else
		tmpc = src[strlen(dev)];

	if(tmpc != '/' && tmpc != 0)
		return 0; // not the full path

	scriptType = (loc == &preScript) ? 1 : (loc == &postScript) ? 2 : 0;

	if(sel != NULL)
		{
		// sel->sources[0].location = p->deviceName; // getStringPtr(&sel->pool,dev);
		// sel->sources[0].party = p; // includes type
		sel->sources[0].diskNum = (isDisk)?-1:0;
		sel->sources[0].location = p;
		res=mountLocation(sel,0,0);  // type is part of location->type
		}
	else
		{
		res=mountLocation(dev,type,scriptType);
		loc->dev = &src[strlen(dev)];
		if(trackMount & 8)
			{
			trackMount ^= 8; // clear it
			trackMount |= (loc == &image1) ? 16 : (loc == &image2) ? 32 : 0; // don't set if it's pre- or post-; handle that mount manually
			}

		if(res == 1 && (mode == RESTORE || mode == BACKUP) && !scriptType)
			{
			currentImage.mount = p; currentImage.isDisk = isDisk;
			}
		}

	if(res)
		{ // mounted ok
		if(tmp)
			{
			memmove(src,&src[tmp],strlen(src)-tmp+1);
			prefixPath(src,dev);
			} // replace label with device

		if(strlen(dev) < 128)
			{
			strcpy(loc->extended,dev);
			} // store original device entry
		else
			*loc->extended = 0;

		if(!scriptType)
			strncpy(src,"/mnt",4);
		else if(*loc->extended)
			{ // able to keep
			memmove(src,&src[strlen(dev)],strlen(src)-(strlen(dev))+1);
			prefixPath(src,"/mnt/pre");
			debug(INFO, 1,"Mapped %s from device %s\n",src,loc->extended);
			}
		else
			return 0; // can't copy /dev/... to loc->extended for preScript/postScript values (device string too large)
		}

	return res;
	}

/*----------------------------------------------------------------------------
**
*/
void modifyPart(char *drive, char *part, unsigned char option)
	{
	char matchPart[MAXDISK];
	char mbr = 0;
	int i, j, plen; // , prevState;
	selection *sel = &sels[LIST_DRIVE]; // use TARGET_DRIVE if we ever implement drive->drive duping
	item *itm;
	disk *d;
	partition *p;
	bool anyDrive = false;
	char *partLabel = NULL;

	if(part == NULL)
		part = "";
	else if(!strcmp(part,"0"))
		part = "mbr"; // map '0' to 'mbr'

	if(!strcmp(part,"mbr") || !*part)
		mbr = 1; // mbr same as drive

	if(strlen(drive)+strlen(part)+1 >= MAXDISK)
		debug(EXIT, 1,"Partition/drive name exceeds maximum size.\n");

	if(!strcmp(drive,"@") || !strcmp(drive,"all") || !strcmp(drive,"any"))
		anyDrive = true;

	if(!strncmp(part,"//",2))
		partLabel = &part[2];

//debug(EXIT, 1,"Labels not yet supported for drive specs.\n");
	sprintf(matchPart,"/dev/%s%s",drive,(mbr)?"":part);
	for (i=0;i<sel->count;i++)
		{
		itm = &(sel->sources[i]);
		if(anyDrive || !strcmp(&(((disk *)itm->location)->deviceName)[5],drive))
			{
			d = (disk *)itm->location;
			if(partLabel != NULL)
				{
				plen = strlen(d->deviceName);
				for(j=0;j<d->partCount;j++)
					{
					p = &d->parts[j];
					if(!strcmp(partLabel,p->diskLabel))
						{
						if(plen > strlen(p->deviceName))
							break; // shouldn't happen

						modifyPart(&d->deviceName[5],&p->deviceName[plen],option);
						}
					}

				continue;
				}

			if(anyDrive)
				{
				drive = &(d->deviceName)[5];
				sprintf(matchPart,"/dev/%s%s",drive,part);
				}

			if((doSelect != -1) && (doSelect != i))
				{
				if(doSelect != -2)
					sel->sources[doSelect].state = ST_SEL;

				sel->sources[i].state = ST_SEL;
				}

			doSelect = sel->position = i;
			// d = (disk *)itm->party;
			d = (disk *)itm->location;
			if((d->state & ST_BLOCK) == ST_AUTO || (d->state & ST_BLOCK) == ST_MAX)
				{
				j = d->state & ST_BLOCK;
				d->state = 0;
				if(option != ST_AUTO && option != ST_MAX)
					modifyPart(drive,NULL,(j == ST_AUTO)?ST_IMG:ST_FULL); // convert them, then modify
				}
/*
			switch(option)
				{
				case ST_AUTO: prevState = 0; break;
        case ST_IMG: prevState = (mbr)?ST_AUTO:0; break;
        case ST_CLONE: prevState = ST_IMG; break;
        case ST_FILE: prevState = ST_CLONE; break;
        case ST_SWAP: prevState = ST_FILE; break;
				case ST_MAX: prevState = ST_FULL; break;
        default: prevState = ST_SWAP; break;
        }
*/
			if(mbr)
				{ // modifies top-level disk flag
				d->state = option;
				if(option)
					{
					if(d->diskType & DISK_MASK)
						{
						// if (option != ST_FILE || option == ST_CLONE) d->state = ST_IMG;
						d->state = (option == ST_SWAP)?ST_FILE:option;
						// d->state = nextAvailable(prevState,0,1);
						if(!(d->diskType & DISK_TABLE))
							d->state = ST_FULL; // not a recognized partition (will add GPT later)
						}
					else
						{ // loop drive
						if(verifyQuickMount(d->deviceName))
							d->state = CLR_RB;
						else
							{
							if (option == ST_MAX) option = ST_FULL;
							d->state = (isSupported(d->diskType,option))?option:nextAvailable(0,d->diskType & TYPE_MASK,0);
							// d->state = nextAvailable(prevState,d->diskType & TYPE_MASK,0);
							}
						continue;
						}
					}
				}
/*
			if (prevState == ST_AUTO) prevState = 0;
			else if (prevState == ST_FULL) prevState = ST_SWAP;
*/
			if((!mbr || !*part))
				{ // scan through drive for match
				for(j=0;j<d->partCount;j++)
					{
					p = &(d->parts[j]);
					if(option == ST_AUTO)
						{
						p->state = (verifyQuickMount(p->deviceName))?CLR_RB:0;
						if (p->state) d->state |= CLR_BUSY;
						continue;
						}

					if((p->flags & FREE) || (p->num == -1) || !(p->flags & (PRIMARY | LOGICAL)))
						continue;

					if(*part && strcmp(p->deviceName,matchPart))
						continue;

					if(option && verifyQuickMount(p->deviceName))
						{ // check if mounted
						if(option == ST_MAX)
							d->state = 0;

						debug(INFO, 1,"Partition %s is mounted.\n",p->deviceName);	// error unless --skipmounted
						p->state = CLR_RB;
						}
					else if(option && option != ST_MAX)
						{ // validate partition type and fall through to the most efficient flagA
						p->state = (isSupported(p->type,option))?option:nextAvailable(0,p->type & TYPE_MASK,0);
						// p->state = nextAvailable(prevState,p->type,0);
/*
						p1 = isSupported(p,ST_IMG);
						p2 = isSupported(p,ST_CLONE);
						p3 = isSupported(p,ST_FILE);
						if ((!p1 && !p2 && !p3) || (p->state == ST_FULL)) p->state = ST_FULL;
						else if (option == ST_IMG) {
							if (p1) p->state = ST_IMG;
							else if (p2) p->state = ST_CLONE;
							else p->state = ST_FILE;
							}
						else if (option == ST_CLONE) {
							if (p2) p->state = ST_CLONE;
							else if (p1) p->state = ST_IMG;
							else p->state = ST_FILE;
							}
						else if (option == ST_FILE) {
							if (p3) p->state = ST_FILE;
							else if (p1) p->state = ST_IMG;
							else p->state = ST_CLONE;
							}
						else p->state = option;
*/
						}
					else
						p->state = 0;

					if(*part)
						break;
					}

				if(*part && j == d->partCount)
					{
					debug(INFO, 1,"Partition %s not found.\n",matchPart);
					}
				}

			if(!anyDrive)
				break;
			}
		}

	if(i == sel->count)
		{
		if(doSelect == -1)
			doSelect = -2;
		else if(doSelect >= 0)
			sel->sources[doSelect].state = ST_SEL;
		}

	return;
	}

/*----------------------------------------------------------------------------
** unAuto maps the MBR but removes the auto function so each partition can be
** manually added/removed; keep = transfer the partition over.
*/
void unAuto(disk *d, bool keep)
	{
	int i,j;
	int setting = (keep) ? SI_PART : 0;

	if(d == NULL)
		return;

	disk *dmap = d->map;

	if(dmap == NULL)
		return;

	if((d->state & ST_BLOCK) == SI_AUTO)
		{
		for(i=0;i<dmap->partCount;i++)
			{
			if(d->auxParts[i].map != NULL)
				d->auxParts[i].state = setting; // keep or discard

			if(!setting)
				{
				d->auxParts[i].map = NULL;
				d->auxParts[i].type = PART_RAW;
				d->auxParts[i].state = 0;
				} // don't copy
			}

		d->state = SI_MBR;
		}

	return;
	}

/*----------------------------------------------------------------------------
**
*/
int findPart(disk *d, int part, bool isMapped)
	{
	int i;
	int c = 0;
	partition *p = (isMapped) ? d->auxParts                 : d->parts;
	int pcount   = (isMapped) ? ((disk *)d->map)->partCount : d->partCount;

	for(i=0; i<pcount;i++)
		{
		if(p[i].num == part)
			return(c);

		if(p[i].flags & PRIMARY || p[i].flags & LOGICAL)
			c++;
		}

	debug(INFO, 1,"Partition %i not found in %s\n",part,d->deviceName);

	return -1;
	}

/*----------------------------------------------------------------------------
** atoi, but checks to make sure it's a positive number or matches a
** specific idX, prt string, otherwise returns -1
*/
int atoicheck(char *str)
	{
	int i;
  char *check = str;

  if(check == NULL || !*check)
		return -1;

	if(!strcmp(check,"prt"))
		return loopDrive;

	if(!strncmp(check,"id",2))
		{ // see if it matches a known image drive
		for(i=0;i<iset.dcount;i++)
			{
			if(!strcmp(check,&iset.drive[i].deviceName[5]))
				return(i+1);
			}

		return -1;
		}

	while(*check >= '0' && *check <= '9')
		check++;

  return((*check) ? -1 : atoi(str));
  }

/*----------------------------------------------------------------------------
** determine if a drive image/loop image matches something
*/
int matchSet(disk *drive, disk *image, char *srcpart, bool loop)
	{
	int k;

	if(loop)
		{ // see if there's a matching loop device
		int ipart = atoicheck(srcpart);
		if(srcpart != NULL && *srcpart && (ipart == -1 || !ipart || ipart > image->partCount))
			{
			return 0;
			} // partition doesn't make sense; change this code if we ever implement labels in addition to numbers

		if(ipart == -1)
			{
			for(k=0;k<image->partCount;k++)
				{
				if(image->parts[k].length == (drive->deviceLength * drive->sectorSize))
					return k+1;
				}
			}
		else if(image->parts[ipart-1].length = (drive->deviceLength * drive->sectorSize))
			return ipart;
		}
	else if((drive->deviceLength * drive->sectorSize) == (image->deviceLength * image->sectorSize))
		return 1;

	return 0;
	}

/*----------------------------------------------------------------------------
** F6 => Auto-image all selected drives
*/
void autoSelectMap(int direction) {
	disk *targetDisk;
	disk *imageDisk;
	int i,j,k,a,ipart, imatch;
	selection *sel = &sels[LIST_DRIVE];
	selection *imageSel = sels[LIST_IMAGE].attached;
	selection *targetSel = sels[LIST_DRIVE].attached;
	int iPos = imageSel->position;
	bool retry = false;
	int startImage = 0;

	if (direction) { // uses the arrow keys to select different image drives onto a specific target drive
		return; // feature not yet supported; could add this when images contain more than one image drive
		}
	if (currentFocus == WIN_SOURCE) { // select all states through which we will scan for a match
		for (i=0;i<sel->count;i++) sel->sources[i].state = ST_SEL;
		fillAttached(sel);
		}

	for (i=0;i<sel->count;i++) {
		if (i != sel->position && sel->sources[i].state != ST_SEL) continue;
		targetDisk = (disk *)sel->sources[i].location;
		imatch = -1; ipart = -1;
		startImage = (retry)?startImage:0;
		if (retry) { retry = false; } // try another image
		for (j=startImage;j<iset.dcount;j++) {
			imageDisk = &iset.drive[j];
			k = matchSet(targetDisk,imageDisk,NULL,(j == loopDrive));
			if (k) {
				if (j == loopDrive) ipart = k;
				imatch = j;
				break;
				}
			}
		if (imatch != -1) {
			// look for
			for (a=0;a<imageSel->count;a++) {
				if (imatch == loopDrive) {
					if (imageSel->sources[a].location == &iset.drive[imatch].parts[ipart]) break;
					}
				else if (imageSel->sources[a].location == &iset.drive[imatch]) break;
				}
			if (a != imageSel->count) { imageSel->position = a; }
			else { debug(INFO, 1,"autoSelectMap() source error\n"); return; } // should never happen; implies aux selector doesn't have a map to image drive
			for (a=0;a<targetSel->count;a++) {
				if (targetSel->sources[a].location == targetDisk) break;
				}
			if (a != targetSel->count) { targetSel->position = a; }
			else { imageSel->position = iPos; debug(INFO, 1,"autoSelectMap() target error\n"); return; } // should never happen; implies aux selector doesn't have a map to target drive
			toggleMap(true,0);
			if (targetDisk->map != NULL && (!(targetDisk->diskType & DISK_MASK) && verifyQuickMount(targetDisk->deviceName))) { targetDisk->map = NULL; targetDisk->state = CLR_RB; } // loop part is already mounted
			if (targetDisk->map == NULL) { // try a different image; incompatible map
				 retry = true; startImage = ++imatch; --i; continue;
				}
			imageSel->position = iPos;
			// targetSel->position = tPos;
			targetSel->position = 0; // difficult to calculate unless we determine disk* prior to running this
			}
		else if (currentFocus == WIN_SOURCE) sel->sources[i].state = 0;
		}
	if (currentFocus == WIN_SOURCE) {
		sel->position = -1;
		for (i=0;i<sel->count;i++) {
			if (sel->sources[i].state == ST_SEL) {
				if (sel->position == -1) { sel->position = i; sel->sources[i].state = 0; }
				else sel->sources[sel->position].state = ST_SEL;
				}
			}
		if (sel->position == -1) sel->position = 0;
		fillAttached(sel); // prevents cursor motion from auto-selecting things if expert mode is active
		}
	checkBusyMap(); // automatically sets busyMap
	allWindows(CLEAR);
	}

// automatically selects disk
void autoSelectDrive(void) {
	int i;
	disk *d;
	selection *sel = &sels[LIST_DRIVE];
	for (i=0;i<sel->count;i++) {
		if (i != sel->position && sel->sources[i].state != ST_SEL) continue;
		d = sel->sources[i].location;
		if (d->diskType & DISK_MASK) d->state = ST_AUTO;
		else d->state = nextAvailable(0,d->diskType,0);
		}
	}

void rescanMounts(disk *d) {
	int i;
	d->state = 0;
	for (i=0;i<d->partCount;i++) {
		if (verifyQuickMount(d->parts[i].deviceName)) { d->parts[i].state = CLR_RB; d->state = CLR_BUSY; }
		}
	}

void modifyMapping(char *map, char *part, bool isNegative) {
	char *target;
	char *srcpart = "";
	int idrive = -1, ipart;
	int mbr = 0;
	int i, j, k;
	int partA, partB;
	selection *sel = &sels[LIST_DRIVE];
	selection *imageSel = sels[LIST_IMAGE].attached;
	selection *targetSel = sels[LIST_DRIVE].attached;
	item *itm;
	disk *targetDisk;
	disk *imageDisk;
	static int lastPos = -1;
	bool hasFound;
	bool globalFound = false;
	int imatch;
	partition *parts;
	int partCount;
	bool properMap = false;
	bool isMapped;
	bool retry = false;
	int startImage = 0;
	int dpart;
	// if (part == NULL) part = "";
	/* parse [a]@[b] grammar; required if using non-identical image/drive combinations on the command-line */


	if ((target = strchr(map,'@')) != NULL) { // source/target provided, parse out source partition
		properMap = true;
		*target++ = 0;
		if (!strcmp(target,"any") || !strcmp(target,"all")) target = ""; // same as @:...
		if ((srcpart = strchr(map,'.')) != NULL) *srcpart++ = 0;
		else srcpart = "";
		}
	else if ((idrive = atoicheck(map)) == -1) { // it's a target drive
		if (!strcmp(map,"any") || !strcmp(map,"all")) target = "";
		else target = map;
		map = "";
		}
	else { target = ""; if (part != NULL) srcpart = part; part = NULL; }  // it's an image drive
	if (!strcmp(srcpart,"mbr")) srcpart="0";
	if (part != NULL && !strcmp(part,"mbr")) part="0";
	if (*map) { // check image drive
		idrive = atoicheck(map); // check again for '@' condition as well
		if (idrive == -1) debug(EXIT, 1,"Incorrect source drive format %s\n",map);
		if (!idrive && loopDrive == -1) debug(EXIT, 1,"Independent partition not available\n");
		idrive = (idrive)?idrive-1:loopDrive;
		}
	if (!*target && (part != NULL) && *part && !*srcpart) return; // can't do <img>@any:#; source part required for this form
	/* source image (drive) selection */
	if (idrive >= iset.dcount) debug(EXIT, 1,"Image disk number out of range.\n");
	for (i=0;i<sel->count;i++) {
		hasFound = false;
		imatch = -1;
		ipart = 0;
		dpart = 0;
                itm = &(sel->sources[i]);
		targetDisk = (disk *)itm->location;
		startImage = (retry)?startImage:0;
		if (retry) { debug(INFO, 1,"Trying to match another image for %s\n",targetDisk->deviceName); retry = false; }
		if (*target) { // we know the target drive
			if (strcmp(&(targetDisk->deviceName)[5],target)) continue;
			globalFound = hasFound = true;
			}
		if (idrive != -1) { // find first drive matching specific image
			imageDisk = &iset.drive[idrive];
			if (!*target || (idrive == loopDrive)) {
				debug(INFO, 1,"Checking %s against %s\n",targetDisk->deviceName,imageDisk->deviceName);
				k = matchSet(targetDisk,imageDisk,srcpart,(idrive == loopDrive));
				if (k) {
					// check srcpart label match; if no match, 'continue'; set ipart if label match
					if (idrive == loopDrive) ipart = k;
					globalFound = hasFound = true; imatch = idrive;
					debug(INFO, 1,"Image disk match: %i -> %s\n",idrive,targetDisk->deviceName);
					}
				}
			else imatch=idrive;
			}
		else { // try to match all drives
                        for (j=startImage;j<iset.dcount;j++) {
                                imageDisk = &iset.drive[j];
				k = matchSet(targetDisk,imageDisk,NULL,(j == loopDrive));
				if (k) {
					// check srcpart label match; if no match, 'continue'; set ipart if label match
					if (j == loopDrive && *srcpart) break; // can't match :3@ to a loop device
					if (j == loopDrive) ipart = k;
					globalFound = hasFound = true; imatch = j;
					debug(INFO, 1,"Image disk match: %s -> %s\n",imageDisk->deviceName,targetDisk->deviceName);
					break;
					}
                                }
			}

		if (hasFound && idrive == -1 && imatch == -1 && targetDisk->map != NULL) { // use existing map, if any, if we can't auto-match (will switch to existing afterwards, but we need to trigger imatch)
			for (k=0;k<iset.dcount;k++) {
				if (&iset.drive[j] == targetDisk->map) { imatch = k; break; }
				}
			}

		if (hasFound && imatch != -1) { // we know the drive match; process something
			// we create 'virtual' selectors consisting of the image/target tables in question
                        imageSel->count = imageSel->position = imageSel->top = 0;
                        populateAttached(&iset.drive[imatch],imageSel,0,imatch);
                        targetSel->count = targetSel->position = targetSel->top = 0;
                        populateAttached(targetDisk,targetSel,0,i);
			// keep track of global selector
                        if ((doSelect != -1) && (doSelect != i)) {
                                if (doSelect != -2) sel->sources[doSelect].state = ST_SEL;
                                sel->sources[i].state = ST_SEL;
                                }
                        doSelect = sel->position = i;
			// get the ipart from a number or label match
                        if (!ipart && *srcpart) {
                                ipart = atoicheck(srcpart);
                                if (ipart < 0) debug(EXIT, 1,"Unable to interpret partition definition %s:%s\n",map,srcpart);
				if (ipart && ((partB = findPart(imageDisk,ipart,false)) == -1)) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue; }
				}
			if (part != NULL && *part) { dpart = atoicheck(part); }
			disk *dmap = targetDisk->map;
			if (imatch == loopDrive) {
				if (part != NULL) { // <loop/part>@?:, <loop/part>@:?, 0@sda:3, @sda:3, where a loop dev. matches (not a regular drive image)
					if (!*part) {
						if (isNegative) { targetDisk->map = NULL; rescanMounts(targetDisk); continue; }  // do twice to remove all...?
						debug(EXIT, 1,"Can't auto-match a loop partition to a drive\n");
						}
					if (!*target) debug(EXIT, 1,"Can't auto-match a loop partition to a drive\n");
					if (!*srcpart) debug(EXIT, 1,"Can't auto-match a loop partition to a drive\n");
					if ((partA = findPart(targetDisk,dpart,(dmap != NULL))) == -1) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue;}
					imageSel->position = partB;
					targetSel->position = partA+1;
					debug(INFO, 1,"Map partition to drive:part\n");
					}
				else {
					imageSel->position = partB;
					debug(INFO, 1,"Map loop partition to drive\n");
					}
				if (!checkMapping(true,0)) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue;}
				toggleMap(true,0);
				}
			else if (*srcpart) { // :part, :part@sda, :part@:3,
				if (isNegative) debug(EXIT, 1,"Negative cannot be applied to a mapping.\n");
				if (!ipart) {
					if (part != NULL && strcmp(part,"0")) debug(EXIT, 1,"Can't map onto non-drive\n");
					debug(INFO, 1,"Map :mbr@sda onto drive");
					}
				else if (idrive != -1 && *target && part == NULL) { // @drive
					imageSel->position = partB+1;
					debug(INFO, 1,"Map partition to loop drive.\n");
					}
				else if (idrive == -1 && part == NULL) { // :3@, :3@sda
					if ((partA = findPart(targetDisk,ipart,(dmap != NULL))) == -1) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue; }
					// check to see if part physically fits here (TODO)
					targetSel->position = partA+1;
					imageSel->position = partB+1;
					debug(INFO, 1,"Restore partition %i onto %s part %i <- %i\n",ipart,(dmap != NULL)?"mapped":"unmapped",partA+1,partB+1);
					}
				else if (part != NULL && !strcmp(part,"0")) debug(EXIT, 1,"Can't map partition onto :mbr\n");
				else {
					if ((partA = findPart(targetDisk,dpart,(dmap != NULL))) == -1) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue; }
					targetSel->position = partA+1;
					imageSel->position = partB+1;
					debug(INFO, 1,"Restore partition onto current mapping\n");
					}
				if (!checkMapping(true,0)) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue;}
				toggleMap(true,0);
				if (!ipart) unAuto(targetDisk,0);
				}
			else if (idrive != -1 && *target && part != NULL && strcmp(part,"0")) {
				if (isNegative) debug(EXIT, 1,"Negative cannot be applied to a mapping.\n");
				if ((partA = findPart(targetDisk,dpart,(dmap != NULL))) == -1) continue;
				targetSel->position = partA+1; // mbr + partition count
				if (!checkMapping(true,0)) continue;
				toggleMap(true,0);
				}
			else { // @sda,@sda:3,2@sda,2@sda:3
				if (isNegative && (part == NULL || !*part || !strcmp(part,"0"))) {  // do the full disk, or mbr (mbr first, then full disk)
					if (targetDisk->map == NULL) {
						for (k=0;k<targetDisk->partCount;k++) targetDisk->parts[k].map = NULL;
						}
					else { targetDisk->map = NULL; rescanMounts(targetDisk); }
					continue;
					}
				bool wasClear = false;
				if ((idrive == -1 && targetDisk->map == NULL) || (idrive != -1 && targetDisk->map != imageDisk)) {
					wasClear = true;
					debug(INFO, 1,"Auto-populate, set to SI_AUTO\n");
					if (!checkMapping(true,0)) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue;}
					toggleMap(true,0);
					}
				dmap = targetDisk->map;
				if (dmap == NULL) { if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue; }
				// we now have a map; modify it
				if (part != NULL && *part && strcmp(part,"0")) { // <x>@sda:3, etc. :3,
					if ((partA = findPart(targetDisk,dpart,(dmap != NULL))) == -1) {
						if (wasClear) { targetDisk->map = NULL; targetDisk->state = 0; }
						if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue;
						}
					unAuto(targetDisk,isNegative);
					imageSel->position = targetSel->position = partA+1;
					if (!isNegative && !checkMapping(true,0)) {
						if (wasClear) { targetDisk->map = NULL; targetDisk->state = 0; }
						if (idrive == -1) { retry = true; startImage = ++imatch; --i; } continue;
						}
					toggleMap((isNegative)?false:true,0);
					debug(INFO, 1,"Forced re-mapping (may be empty, so just ignore).\n");
					}
				else if (part != NULL && !*part) { // sda:, @sda:,
					unAuto(targetDisk,1);
					debug(INFO, 1,"Map onto a drive, then switch to mbr and RETAIN other partition maps\n");
					}
				else if (part != NULL) { // sda:0, sda:mbr, @sda:0, @sda:mbr
					unAuto(targetDisk,0);
					debug(INFO, 1,"Map onto a drive, then switch to mbr mode and CLEAR other partitions\n");
					}
			 	else { 	debug(INFO, 1,"Map onto drive; keep as optimized [ignore if map already present]\n"); }
				}
			driveSelected = 1;
			}
		}
	imageSel->count = targetSel->count = 0; // reset temporary selectors
        if (!globalFound) {
                if (doSelect == -1) doSelect = -2;
                else if (doSelect >= 0) sel->sources[doSelect].state = ST_SEL;
                }
	}

void toggleDrives(void) { // select the drives which have been specified on the command-line <name>[:part]
	// part => mbr, >= 0, or blank. If blank, select all.
	// -, don't include, +, use fsarchiver (can grow), x i= use 'dd'
	int i, j, len;
	char *part; // *drive
//	char *drive;
	char drive[MAXPART];
	selection *sel = &sels[LIST_DRIVE];
	disk *d;
	if (!partCount) return; // nothing selected
	for (i=0;i<partCount;i++) {
	//	 drive = part_filter[i];
		strcpy(drive,part_filter[i]);
		if ((mode == RESTORE) && ((part = strchr(drive,'@')) != NULL)) { // convert the ':' (if any) to '.' before '@' to avoid parser confusion
			*part = 0;
			if ((part = strchr(drive,':')) != NULL) *part = '.';
			drive[strlen(drive)] = '@';
			}
		if ((part = strchr(drive,':')) != NULL) { // delimited drive
			if (part[1] == '-') {
				*part++ = 0; part++;
				if (mode == BACKUP) {
					debug(INFO, 1,"Checking drive: %s\n",drive);
					for (j=0;j<sel->count;j++) {
						d = sel->sources[j].location;
						if (strcmp(&d->deviceName[5],drive)) continue;
						if (j == doSelect || (d->state == ST_SEL)) break;
						}
					if (j == sel->count) modifyPart(drive,NULL,ST_AUTO);
					modifyPart(drive,part,0);
					}
				else modifyMapping(drive,part,true);
				}
			else if (part[1] != '-') {
				*part++ = 0;
				if (mode == RESTORE) modifyMapping(drive,part,false);
				else if (*part == '+') modifyPart(drive,++part,ST_FILE); // growable
				else if (*part == 'x') modifyPart(drive,++part,ST_FULL); // 'dd'
				else if (*part == 's') modifyPart(drive,++part,ST_SWAP);
				else if (*part == '*') modifyPart(drive,++part,ST_IMG);
				else if (*part == '#') modifyPart(drive,++part,ST_CLONE);
				else if (*part == 'X') {
					if (part[1]) debug(EXIT, 1,"Option 'X' can only be applied to entire drive.\n");
					modifyPart(drive,NULL,ST_MAX);
					}
				else modifyPart(drive,part,ST_IMG); // normal image; 'sda' is different than 'sda:'; sda: individually selects all available parts
				}
			}
		else { // select a full drive
			if (mode == BACKUP) modifyPart(drive,NULL,ST_AUTO); // automatically add all of them
			else modifyMapping(drive,NULL,false);
			}
		}
	}

void mountArgs(char hasUI, char type) {
	int i, j;
	disk *d;
	partition *p;
	selection *sel = NULL;
	navImage *img;
	switch(type) {
		case 0: img = &image1; break;
		case 1: img = &image2; break;
		case 2: img = &preScript; if (!(options & OPT_PRE)) return; break;
		default: img = &postScript; if (!(options & OPT_POST)) return; break;
		}
	int matched = 0;
	if (mode == ERASE) return;
	if (hasUI) {
		if (mode == TRANSFER) sel = (type)?&sels[LIST_TARGET]:&sels[LIST_SOURCE];
		else sel = &sels[LIST_IMAGE];
		}
	if (matched = findMount(img,NULL,sel,0));
	else for (i=0;i<dset.dcount;i++) {
		d = &(dset.drive[i]);
		if (matched) break;
		if (!(d->diskType & DISK_MASK)) {
			if (matched = findMount(img,d,sel,1)) break;
			}
		for (j=0; j<d->partCount;j++) {
			p = &(d->parts[j]);
			if (p->num == -1 || p->deviceName == NULL) continue; // skip this
			if (matched = findMount(img,p,sel,0)) break;
			}
		}
	if (matched) {
		if (mapLocation(img,sel,0,type > 1)) { // some error
			if (type == 2) { debug(INFO, 1,"Pre-script not found.\n"); options &= ~OPT_PRE; }
			if (type == 3) { debug(INFO, 1,"Post-script not found.\n"); options &= ~OPT_POST; }
			}
		else if (type == 2) debug(INFO, 1,"Pre-script %s detected.\n",img->path);
		else if (type == 3) debug(INFO, 1,"Post-script %s detected.\n",img->path);
		if (type == 2) unmountLocation("/mnt/pre"); // just in case it was a mount point
		if (type == 3) unmountLocation("/mnt/post"); // just in case it was a mount point
		}
	else debug(EXIT, 1,"Unable to locate %s\n",img->image);
	// else printf("Found something.\n");
	}

void autoSel(char *path, unsigned char mountType) {  // mountType == 0; don't mount; called only when mode == RESTORE
	int i;
	selection *sel = &sels[LIST_IMAGE];
	if (mountType) { // mount default first
		if (mountLocation(path,mountType,0)) debug(INFO, 1,"Auto-mount: %s\n",path);
		else { debug(INFO, 1,"Unable to auto-mount %s\n",path); return; }
		trackMount |= 1;
		strcpy(image1.image,path);
		memcpy(image1.image,"/mnt",4); // replace /dev with /mnt
		path = image1.image;
		}
	while(path[strlen(path)-1] == '/') path[strlen(path)-1] = 0; // remove trailing '/'
	if (!navigateDirectory(path,sel)) { debug(INFO, 1,"Unable to read directory %s\n",path); return; }
	for (i=0;i<sel->count;i++) {
		if (sel->sources[i].identifier == NULL) continue;
		if (sel->sources[i].state != CLR_GB) continue;
		strcpy(image1.image,path);
		image1.path = &(image1.image[strlen(path)+1]);
		strcat(image1.path,sel->sources[i].location);
		sel->position = i;
		break;
		}
	}

int parseKernelCmdline(void) {
        int args = 0;
	int i;

        if (openFile("/proc/cmdline")) return 0;
	if (!readFileLine()) return 0;
	closeFile();

        if ((kernelCmdBuffer = malloc(strlen(currentLine)+2)) == NULL) return 0;
        char *token = kernelCmdBuffer;
        kernelArgv[args++] = token;
        *token = 0;
        token = &token[strlen(token)]+1;
        kernelArgv[args] = token;
        bool hasQuote = false;
        bool hasEscape = false;
        int tokenLen = 0;
        char *alpha = currentLine;
        while(*alpha) {
                if (hasQuote) {
                        if (*alpha == '"') hasQuote = false;
                        else token[tokenLen++] = *alpha;
                        }
                else if (hasEscape) {
                        hasEscape = false;
                        token[tokenLen++] = *alpha;
                        }
                else if (*alpha == '"') hasQuote = true;
                else if (*alpha == '\\') hasEscape = true;
                else if (*alpha == ' ') {
                        token[tokenLen] = 0;
                        if (tokenLen) {
				if (args == (MAX_ARGS-1)) { token = NULL; break; }
                                token = &token[tokenLen]+1;
                                kernelArgv[++args] = token;
                                }
                        tokenLen = 0;
                        }
                else token[tokenLen++] = *alpha;
                alpha++;
                }
	if (token != NULL) { token[tokenLen] = 0; args++; }
	else debug(INFO, 1,"Kernel commandline limit reached.\n");
	for(i=1;i<args;i++)
		debug(INFO, 1, "Kernel argument: %s%s%s\n",
			SYSRES_XTERM_COLOR_YELLOW_D,
			kernelArgv[i],
			SYSRES_XTERM_COLOR_RESET_D
			);
	return args;
        }

/*----------------------------------------------------------------------------
** System Restore program start.
*/
int main(int argc, char *argv[])
	{
	char *arg;
	int i = 0;
	char valpass = 0;
	int operation = 0;
	struct stat64 tmpStat;
	int setui = 0;

  dset.dcount = *currentImage.imagePath = *currentImage.imageName = *currentImage.imageTitle = currentImage.newPartNum = 0;
	*image1.image = *image2.image = image1.local = image2.local = 0;
	image1.path = image2.path = NULL;
	*preScript.image = *postScript.image = preScript.local = postScript.local = 0;
	preScript.path = postScript.path = NULL;
	currentImage.mount = NULL;

	signal(SIGINT,SYSRES_SIGNAL_SIGINT_Handler); // ctrl-c
	signal(SIGQUIT,SIG_IGN); // ctrl-backslash
	signal(SIGPIPE,SIG_IGN); // issue with write()?
	signal(SIGTSTP,SIG_IGN); // ctrl-z

	int ppid = getppid(); // allow parsing /proc/cmdline if we're owned by init()
	if(ppid != 1)
		shellset = 1; // display shell menu if we weren't started by init

debug(INFO, 1,"%sPROGRAM START [disk %i/part %i/int %i/ptr %i/width %d] PPID %i [%i]%s\n",
		SYSRES_XTERM_COLOR_GREEN_D,
		sizeof(disk),
		sizeof(partition),
		sizeof(int),
		sizeof(char*),
		getTermWidth(),
		ppid,
		argc,
		SYSRES_XTERM_COLOR_RESET_D
		);

	initHashEngine();

	// TODO: also parse /proc/cmdline???

	mountDefaults();
	SYSRES_LINUX_SetKernelLogging(SYSRES_LINUX_KERN_LOGMODE_INIT_D);
	if(!verifyMount("proc","/proc","rw") && !verifyMount("/proc","/proc","rw"))
		debug(EXIT, 1,"/proc is not mounted.\n");

	if (!argc && ppid == 1 && (stat64(SYSRESINIT,&tmpStat)))
		{ // see if there's any /proc/cmdline items of note; convert them to argc, argv
		if((argc = open(SYSRESINIT,O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR| S_IWUSR | S_IRGRP | S_IROTH)) >= 0)
			close(argc); // touch this so we don't run the kernel command on exit

		kernelMode = true;
		argc = parseKernelCmdline();
		argv = kernelArgv;
		}

  if(argc > 1)
		{ // parse the argument line
		for(i = 1; i < argc;)
			{ // two-pass argument scan
			arg = argv[i];
			if(valpass)
				{
				if((arg = strchr(arg,'=')) != NULL)
					{
					*arg++ = 0;
					readOption(argv[i],arg);
					}
				}
			else if(!strcmp(arg,"--help"))
				{
				usage(argv[0]);
				exit(0);
				}
			else if (strchr(arg,'=') == NULL)
				readOption(argv[i],NULL);
			else if(!strncmp(arg,"pass=",5) || !strncmp(arg,"password=",9) || !strncmp(arg,"user=",5) || !strncmp(arg,"username=",9))
				{
				arg = strchr(arg,'=') + 1;
				if(*argv[i] == 'u')
					cifsUser = arg;
				else
					cifsPass = arg;
				}
			if((++i == argc) && !valpass)
				{  // check mode; reset for second pass
				if(options & OPT_RAMDISK)
					mountDefaults(); // see if RAMdisk is available again

				if(			(mode == RESTORE && !(options & OPT_RESTORE))
						||	(mode == BACKUP && !(options & OPT_BACKUP))
						||	(mode == ERASE && !(options & OPT_ERASE))
          	||	(mode == TRANSFER && !(options & OPT_TRANSFER))
						)
					debug(EXIT, 1,"Mode not supported by application.\n");

				if((mode != -1 && mode != TRANSFER))
					*image2.image = -1; // indicate that we don't need a special mount point for transfer

				i=valpass=1;
				if(usbDelay)
					startCount();
				}
			}
  	}

	debug(INFO, 1,"Start-up options: %i\n",options);
	// some logical sanity checking for mixing options
	if(mode != LIST && mode != -1 && (show_list & 4))
		debug(EXIT, 1,"Verify cannot be combined with an action command.\n");

	if(options & OPT_REBOOT && options & OPT_HALT)
		debug(EXIT, 1,"Can't have both halt and reboot as options.\n");

	if((show_list & 3) == 3)
		debug(EXIT, 1,"The list and detail options are mutually exclusive.\n");

	if(mode != LIST && mode != -1 && (show_list & 1))
		debug(EXIT, 1,"Use the detail option to show mappings.\n");

	if(modeset && ((show_list & (1|4)) == (1|4)))
		debug(EXIT, 1,"The verify list option is not available in UI mode.\n");

	if(show_list & 16)
		{
		show_list &= ~16; version();
		if(mode == -1 && !modeset)
			mode = LIST;
		}

	if(options & (OPT_REBOOT | OPT_HALT) && (mode == -1 && !modeset))
		{
		if(mode == -1 && !show_list && options & OPT_AUTO)
			debug(EXIT, 1,"Mode argument required for --auto option.\n");

		mode = LIST; // don't go to UI mode if --reboot or --poweroff was specified
		}

	if(mode == -1 && !show_list)
		{ // no 'ui' argument turns off mode list
		if(options & OPT_AUTO)
			debug(EXIT, 1,"Mode argument required for --auto option.\n");

		setui = 1;
		mode = (options & OPT_RESTORE)?RESTORE:(options & OPT_BACKUP)?BACKUP:(options & OPT_ERASE)?ERASE:(options & OPT_TRANSFER)?TRANSFER:-1;
		if(mode == -1)
			debug(EXIT, 1,"No modes have been defined. Program cannot run.\n");
		}

	if(modeset && show_list)
		{
		if(modeset && (show_list & 1))
			debug(EXIT, 1,"Use the detail option to enable expert mode.\n");

		if((mode == LIST || mode == -1) && (show_list == 4) && (options & OPT_RESTORE))
			{
			mode = RESTORE;
			performValidate = true;
			show_list = 0;
			} // ctrl-V
		else if((mode == LIST || mode == -1) && (show_list == (2|4)) && (options & OPT_RESTORE))
			{
			mode = RESTORE;
			performVerify = true;
			show_list = 0;
			} // F10
		else if(mode == -1 || mode == LIST)
			{
			mode = (options & OPT_RESTORE)?RESTORE:(options & OPT_BACKUP)?BACKUP:(options & OPT_ERASE)?ERASE:(options & OPT_TRANSFER)?TRANSFER:-1;
			if(mode == -1)
				debug(EXIT, 1,"Unable to set a mode.\n");

			show_list = 0;
			expert = true;
			}

		if(show_list)
			expert = true;
		}

	// if (modeset && (show_list & 1)) debug(EXIT, 1,"Use the detail option for enabling expert mode.\n");

	// mountDefaults();
	initAbout();
	allocateStorage();
// scanDrives(); exit(1); // TEMPORARY
	scanDrives(); // only if using new mountArgs()
	mountArgs(setui | modeset, 0);
	if(mode == TRANSFER)
		mountArgs(setui | modeset, 1); // interpret target= argument for transfer

	mountArgs(0, 2); // check for OPT_PRE file (don't check for OPT_POST file since that will be after an action)
	populateList(NULL);
	if(mode == BACKUP)
		{ // see if we have enough to start the run, unless --pause is specified; the executor won't run if there are no parts selected
		currentFocus = WIN_TARGET; // compatible with non-expert mode
		}
	else if(mode == RESTORE || show_list)
		{ //  && (options & OPT_AUTO)) {
		item *itm = NULL;
		if(options & OPT_AUTO)
			{ // look for one-and-only available drive mount
			for(i=0;i<sels[LIST_IMAGE].count;i++)
				{
				if((&sels[LIST_IMAGE].sources[i])->diskNum == -2)
					continue;

				if(itm != NULL)
					{
					itm = NULL;
					break;
					}

				itm = &sels[LIST_IMAGE].sources[i];
				}
			}

		if(*image1.image)
			{
			if(image1.path == NULL && (options & OPT_AUTO))
				autoSel(image1.image,0);

			populateImage(NULL,0);
			currentFocus = WIN_TARGET; // compatible with non-expert mode
			}
		else if(itm != NULL)
			{ // auto-mount and select first image
			if(itm->diskNum == -1)
				autoSel(((disk *)itm->location)->deviceName,((disk *)itm->location)->diskType);
			else
				autoSel(((partition *)itm->location)->deviceName,((partition *)itm->location)->type);

			populateImage(NULL,0);
			currentFocus = WIN_TARGET;
			}

		if(mode == RESTORE && (options & OPT_AUTO) && (!partCount) && (!expert || show_list))
			{ // set target=any with --auto
			strcat(part_filter[0],"any");
			partCount++;
			}
		}

	if(expert && (!show_list))
		options &= ~OPT_AUTO; // remove --auto mode if 'detail' used without restore/backup operation

	toggleDrives();
	if(!checkBusyState())
		setup = 0; // prevents over-writing of existing maps when starting ncurses display

	if(setui | modeset)
		{
		if((options & OPT_BUFFER) && (mode != RESTORE))
			{
			options &= ~OPT_BUFFER;
			debug(INFO, 1,"Canceling buffer option outside of restore.\n");
			}

		if((options & OPT_BUFFER) && !(*image1.image))
			{
			options &= ~OPT_BUFFER;
			debug(INFO, 1,"Canceling buffer option without an image.\n");
			}

		ui_mode = 1;
		navigator();
		unmountExit(0);
		}

	if((options & OPT_BUFFER) && (mode != RESTORE && !show_list))
		debug(EXIT, 1,"Buffer option can only be used with restore/list operations.\n");

	if((options & OPT_BUFFER) && !(*image1.image))
		debug(EXIT, 1,"No image found that can be buffered.\n");

	if(options & OPT_BUFFER)
		{ // buffer image, unmount things, then try again
		char *tmp;

		if(imageSize >= getAvailableBuffer())
			debug(EXIT, 1,"Insufficient RAM (%lu) allocated to buffer image (%lu).\n",getAvailableBuffer(),imageSize);

		if(checkBusyMap())
			 clearBusy(); // cancel toggle mode

		locateImage();
		strcpy(globalPath2,"/mnt/ram/");
		if((tmp = strrchr(globalPath,'/')) != NULL)
			strcpy(&globalPath2[9],&tmp[1]);
		else
			strcpy(&globalPath2[9],globalPath);

		debug(INFO, 1,"Copying %s to RAM buffer.\n",globalPath);
    copyImage("/mnt/ram",globalPath,globalPath2,false);
		if(options & OPT_APPEND)
			customFilesAppend(globalPath,globalPath2);

		unmountExit(-1); // unmount stuff as needed
		strcpy(image1.image,globalPath2);
		image1.path = image1.dev = 0;
		locateImage();
		// debug(EXIT, 1,"Buffer mode not yet implemented in text mode.\n");
		setup = 1;
		toggleDrives(); // re-enable mapping on image in /mnt/ram
		if(!checkBusyState())
			setup = 0;
		}

	debug(INFO, 1,"Primary image: %s/%s\n",image1.image,image1.path);
	if(*image2.image != -1 && *image2.image)
		debug(INFO, 1,"Secondary image: %s/%s\n",image2.image,image2.path);

	if((show_list & 8) && (show_list != 8 || (mode != LIST)))
		debug(EXIT, 1,"Rename option cannot be mixed with other options.\n");

	switch(mode)
		{
		case RESTORE:
		 	if(!*image1.image)
				debug(EXIT, 1,"Missing image filename.\n"); // || !*image1.path

			if(doSelect < 0)
				debug(EXIT, 1,"Nothing to restore.\n");

			if(show_list)
				{
				validOperation = 1;
				if(show_list == 4)
					{ // verify the entire archive
					locateImage();
					copyImage(NULL,globalPath,NULL,true);
					}
				else if(listDisks(NULL) == 0)
					debug(EXIT, 1,"Nothing to restore.\n");

				if(!validOperation)
					exit(1); // something didn't verify

				break;
				}

			performRestore();
			if(!validOperation)
				debug(EXIT, 1,"Nothing was restored.\n");

			// debugSelector(&sels[LIST_IMAGE]);
			// populateImage(NULL,0);
			break;

		case BACKUP:
			if(doSelect < 0)
				debug(EXIT, 1,"Nothing to archive.\n");

			if(show_list)
				{
				if(listDisks(NULL) == 0)
					debug(EXIT, 1,"Nothing to archive.\n");

				// debugSelector(&sels[LIST_IMAGE]);
				break;
				}

			if(!*image1.image)
				debug(EXIT, 1,"Missing image filename.\n");

			createBackup(0);
			if(!validOperation)
				debug(EXIT, 1,"Nothing was backed up.\n");

			break;

		case ERASE:
			break;

		case TRANSFER:
			break; // not supported at this time

		default:
			if(show_list)
				{ // display available hard drives
				if(show_list & 8)
					{
					if(show_list != 8)
						debug(EXIT, 1,"Unable to mix list options\n");

					renameImage();
					break;
					}

				if(*image1.image)
					{
					populateImage(NULL,0);
					validOperation = 1;
					if(show_list == 4)
						{
						locateImage();
						copyImage(NULL,globalPath,NULL,true);
						}
					else
						listDisks(&iset);

					if(!validOperation)
						exit(1);
					}
				else if(show_list == 1)
					listDisks(&dset);
				else
					debug(EXIT, 1,"Missing archive filename.\n");
				}

			break;
		}

	debug(INFO, 1,"Exiting command-line application [%i].\n",options);
	unmountExit(0);

	return(0);
 	}
