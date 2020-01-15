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

#ifndef _FILEENGINE_H_
 #define _FILEENGINE_H_

/*----------------------------------------------------------------------------
** Compiler setup.
*/
#include <time.h>		// time_t
#include <lzma.h>   // lzma_stream
#include <zlib.h>		// z_stream

/*----------------------------------------------------------------------------
** Macro definitions
*/
#define FBUFSIZE 4096
#define MAX_FILE 256

#define ISIZE 4
#define LSIZE 8
#define L2SIZE 16
#define VSIZE 8
#define HDRSIZE 20
// #define VERSTRING "HPRI0000"

#define ARCH_WRITE 0
#define ARCH_READ 1

#define BUFFERED 7 // 00000111 // buffered read, primarily for MBR activity
#define COMPRESSED 6 // 00000110
#define GZIP 2  // 00000010
#define LZMA 4  // 00000100

#define ARCHTYPE	// 11111000 (same bits as ST_CLONE, ST_SWAP, ST_FULL)
#define ARCH_BLANK	// 00000000  <-- don't save the partition to the archive (do nothing)

#define windowBits 15
#define ENABLE_ZLIB_GZIP 32
#define GZIP_ENCODING 16

/*----------------------------------------------------------------------------
** Memory structures
*/
typedef struct imageArch
	{
	unsigned char state;
	char *archiveName;
	unsigned int major, minor;
	unsigned long splitSize; // max. size of each file in bytes, or size of file currently being read
	unsigned int currentSplit;	// 0, 1, 2, 3 (0 is not split)
	int fileHeaderFD;	// -1 no file being written (used to insert file length on writing)
	unsigned long fileSizePosition;	// totalFileSize / expected total filesize
	int currentFD;
	unsigned long fileBytes;	// total bytes in entire guest file (use to populate filesize value; filesize excludes filename, etc.)
	unsigned long originalBytes; 	// uncompressed bytes in entire guest file
	unsigned long expectedOriginalBytes;
	// unsigned long segmentBytes; // file bytes written in segment for guest file (excludes sha1 values, etc.)
	unsigned long segmentOffset;	// total bytes written in segment file (includes sha1 values, etc.)  -- use to set fileSizeOffset when creating a new file
	unsigned long totalOffset;	// total bytes written in entire archive
	unsigned long archiveSize;
	time_t timestamp; // useful if the segments get renamed
	z_stream strm;
#ifdef LIBLZMA
	lzma_stream lstr;
#endif
	} archive;

/*----------------------------------------------------------------------------
** Global Storage.
*/
extern unsigned char fileBuf[];
extern int arch_md_len;

/*----------------------------------------------------------------------------
** Function prototypes
*/
extern int readSpecificFile(archive *arch, int major, int minor, char decompress);
extern int createImageArchive(char *filename, unsigned int segmentSize, archive *arch);
extern int addFileToArchive(unsigned int major, unsigned int minor, unsigned char compression, archive *arch);
extern int signFile(archive *arch);
extern int readImageArchive(char *filename, archive *arch);
extern void closeArchive(archive *arch);
extern int readSignature(archive *arch, char checkSum);

#endif /* _FILEENGINE_H_ */
