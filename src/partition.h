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

#ifndef _PARTITION_H_
 #define _PARTITION_H_

#include <time.h>
#include <sys/types.h>
#include <lzma.h>
#include <zlib.h>

#define MAXDISK 64
#define MAXSIZE 10	// must be smaller than MAXDISK
#define MAXPART 16 // maximum number of partitions per drive
#define PARTLIMIT 0 // absolute maximum number of partitions in a drive, 0 = no upper bound (await malloc error)
#define ITEMLIMIT 0 // absolute maximum number of items in selector; 0 = no upper bound (await malloc error)
#define DRIVELIMIT 0 // absolute maximum number of items in a diskset, 0 = no upper bound (await malloc error)
#define MAXDRIVES 10 // can theoretically be itemlimit

#define DRIVEBLOCK 2 // size of each *disk block
#define PARTBLOCK 8 // size of each *partition block
#define ITEMBLOCK 32 // size of each *item block

#define FREE 1
#define PRIMARY 2
#define EXTENDED 4
#define LOGICAL 8
#define BOOTABLE 16

// #define MBR 2 // used by image mapping check
// #define EMPTY 0
// #define LOOP 5
// #define MSDOS 2
// #define GPT 3
// #define PART 6 // similar to LOOP

#define MAX_PATH 4096
#define MAXPSTR 256

#define VERSTRING "HPRI0000"
#define SYSLABEL "RESTORE"
#define LOCALFS "Local Filesystem"
#define CIFSMOUNT "CIFS Network Mount"
#define HTTPMOUNT "HTTP Network Mount"
#define ANONDISK "Anonymous Disk"
#define RAMFS "RAM Disk"

#define PART_EMPTY	0
#define PART_MSDOS	1
#define PART_VFAT	2
#define PART_NTFS	3
#define PART_EXT2	4
#define PART_EXT3	5
#define PART_EXT4	6
#define PART_XFS	7
#define PART_SWAP	8
#define PART_RAW	9

#define PART_CONTAINER      31 // container for loop device
// could make this 3 bits if we reduce the number of available format types
#define DISK_RAW       32
#define DISK_MSDOS     64
#define DISK_TABLE     64  //  01000000
#define DISK_GPT       96  // GPT table

#define DISK_MASK      96   // 01100000

#define TYPE_MASK      127  // 01111111
#define TYPE_DAMAGED   128  // 10000000 only applies to disks (tables)
#define TYPE_BOOTABLE  128  // only applies to partitions

#define PARTIMG { 0 } // PART_MSDOS, PART_EXT2, PART_EXT3, PART_EXT4, PART_VFAT, PART_NTFS, PART_XFS, 0 } /* btrfs? */
#define FSARCH { 0 } // PART_EXT2, PART_EXT3, PART_EXT4, PART_NTFS, PART_XFS, 0 }
#define PCLONE { PART_EXT2, PART_EXT3, PART_EXT4, PART_XFS, PART_MSDOS, PART_VFAT, PART_NTFS, 0 }

#define IMG_INDEX ".imageindex"
#define IMG_SHA1 ".image.sha1"
#define IMG_SPEC "spec-"
#define IMG_SPECLEN 5

// progress: 4 bits action, 4 bits bar (up to 16)

#define PROGRESS_INIT 0	   // 00000000
#define PROGRESS_UPDATE 16 // 00010000
#define PROGRESS_SYNC 32   // 00100000
#define PROGRESS_OK 48	   // 00110000
#define PROGRESS_FAIL 64   // 01000000
#define PROGRESS_ABORT 96  // 01100000
#define PROGRESS_COMPLETE 80  // 01010000
#define PROGRESS_COMPLETE_NOWAIT  112 // 01110000
#define PROGRESS_CANCEL 240   // 11110000
#define PROGRESS_MASK 240  // 11110000
#define PROGRESS_NUM 15    // 00001111

#define PROGRESS_NONE 0
#define PROGRESS_YELLOW 1
#define PROGRESS_BLUE 2
#define PROGRESS_RED 3
#define PROGRESS_GREEN 4
#define PROGRESS_LIGHT 5

#define PROGRESS_COPY 20
#define PROGRESS_VERIFY 21
#define PROGRESS_BACKUP 22
#define PROGRESS_RESTORE 23
#define PROGRESS_VERIFYCOPY 24
#define PROGRESS_VALIDATE 25

#define INFO 0
#define ABORT 1
#define EXIT 2

#define sector unsigned long
#define IMAGELABEL 38
#define PC_NOP 36
#define PC_FIXED 4168

//typedef struct __imagefile imagefile;

typedef struct __partition
	{
  int num;        // 1
	int major;
	char *deviceName; // /dev/sda1
	char *diskLabel;
  sector start; //
  sector length; //
	unsigned char type;
  unsigned char flags;// bootable, etc.
	unsigned char state; // not saved across modes
	void *map;	// major/minor would be more efficient, since then we also know if it's a disk when minor == 0; maybe when we do a global refactor of the code
  } partition;

typedef struct __disk
	{
  partition *parts;
	int psize; // used to dynamically reallocate *parts
  int partCount; // -1 on a mapped disk if disk is identical to source disk (do later)
	char *diskName; // Seagate ST30293
	char *deviceName; 			// /dev/sda
	char *diskLabel; // rarely used for loop devices
  unsigned char diskType;                   // msdos, gpt, loop, damaged gpt (?), none
	sector deviceLength;
  int sectorSize;
//	int physSectorSize;
	unsigned char state; // mbr state
	void *map;
	partition *auxParts; // a copy of the image partition table (copy, not link, so that we can share the same image across disks)
	int auxsize; // used to dynamically reallocate *auxParts (kept outside of global pool since re-mapping may cause issues with pool linearity)
	int major; // primarily used for images
  } disk;

typedef struct __strpool
	{
	struct __strpool *next;
	char stringpool[];
  }strpool;

typedef struct __poolset
	{
	strpool *pool;
	strpool *start;
	int size; // size of each block
	int index;
	} poolset;

typedef struct __diskset
	{
	disk *drive;
	int size;
	int dcount;
	poolset pool;
  } diskset;

typedef struct __item
	{
  char *identifier;       // proprietary; identifies bits of the archive incl. partition table
	void *location; 	// can be a (char *), (disk *), or (partition *), depending on diskNum and where it is used
	int diskNum;     // index location of disk in diskset, as appropriate; also needed to idenfity *part as disk/partition (could make a char to save space...?)
	unsigned char state; // on/off, etc., as appropriate; first 3 bits are color bits
  } item;

typedef struct __selection
	{
  item *sources;
	poolset pool;
	int size; // size of *sources (realloc as needed in ITEMBLOCK blocks)
  int position; // current posititon selected
  int top;      // current top of list window
  int count;    // number of total items
	int vert, horiz, limit, width; // window boundaries
  struct __selection *attached;
	char *currentPath; // also, option title for non-disk selection
	int currentPathStart;
  } selection;

typedef struct __network
	{
	char *url;
	char *user;
	char *pass;
	} network;


typedef struct __netset
	{
	network *net;
	int locCount;
	} netset;

typedef  struct __image
	{
	char imagePath[MAX_PATH]; // only used when creating images
	char imageName[256]; // only used when creating images
	char imageTitle[256]; // only used when creating images
	char isDisk;
	void *mount;
	int newPartNum; // part# of restore mount if we have to reload part. table
	} image;

typedef struct __navImage
	{
	char image[MAX_PATH];
	char extended[128]; // for preScript and postScript original device entry
	char *path; // pathname, if any
	char *dev; // set *dev to zero before unmounting
	char local;
	} navImage;

typedef struct __imageEntry
	{ // 64 bytes per entry (be careful of the alignment!)
	unsigned int major;
	unsigned int minor;
	unsigned long length;
	unsigned long start; // also, sector size (int)
  unsigned char fsType; // ext3, xfs, raw_disk, etc.
  unsigned char archType; // compression & dd, partclone
	char label[IMAGELABEL]; // fills it up to 64 bytes
	} imageEntry;

typedef struct __eventSpec
	{
  time_t start;
  time_t last;
  unsigned int percentage;
  unsigned long total;
	} eventSpec;

typedef struct pc_hdr
		{
    char nop[PC_NOP];
    int blocksize;
    unsigned long devicesize;
    unsigned long totalblocks;
    unsigned long usedblocks;
    } pc_hdr;

typedef struct __gpt_table
	{
	u_char *lba;
	int lba_limit;
	u_char *partBlock;
	unsigned int partLimit;
	} gpt_table;

#endif /* _PARTITION_H_ */
