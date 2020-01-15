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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fileEngine.h"
#include "mount.h"				// globalBuf
#include "partition.h"
#include "restore.h"			// mbrBuf
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

#define CLR_YB 4        	// mounted parts

extern volatile sig_atomic_t has_interrupted;

unsigned char fileBuf[FBUFSIZE];
unsigned char hdr[HDRSIZE];
unsigned char sha1buf[24];

int arch_md_len = 0;
unsigned char compressBuf[FBUFSIZE];

#ifdef SSL
#include <openssl/evp.h>
const EVP_MD *md;
EVP_MD_CTX mdctx;
unsigned char md_value[EVP_MAX_MD_SIZE];
void sha1Init(void) { EVP_MD_CTX_init(&mdctx); EVP_DigestInit_ex(&mdctx,md,NULL); }
void sha1Update(unsigned char *buf, int size) { EVP_DigestUpdate(&mdctx,buf,size); }
void sha1Finalize(void) { EVP_DigestFinal_ex(&mdctx,md_value,&arch_md_len); EVP_MD_CTX_cleanup(&mdctx); }
#else
#ifdef GCRYPT
#include <gcrypt.h>
gcry_md_hd_t digest = NULL;
unsigned char *md_value;
void sha1Init(void) { gcry_md_reset(digest); }
void sha1Update(unsigned char *buf, int size) {  gcry_md_write(digest,buf,size); }
void sha1Finalize(void) { md_value = gcry_md_read(digest,GCRY_MD_SHA1); }
#else
#include "sha1.h"
hash_state md;
unsigned char md_value[20];
void sha1Init(void) { sha1_init(&md); }
void sha1Update(unsigned char *buf, int size) { sha1_process(&md,buf,size); }
void sha1Finalize(void) { sha1_done(&md,md_value); arch_md_len = 20; }
#endif
#endif

int flushBufferToArchive(unsigned char *buf, unsigned int size,archive *arch);

/****************************
	COMPRESSION FUNCTIONS
****************************/

int gzipCompress(archive *arch, unsigned char *buf, int size) {
	int deflateFlag = (size)?Z_NO_FLUSH:Z_FINISH;
	int n;
	arch->strm.avail_in = size;
	arch->strm.next_in = buf; // buf, but we should be able to leave it at null if we're done
	do {
		arch->strm.avail_out = FBUFSIZE;
		arch->strm.next_out = compressBuf;
		if ((n = deflate(&arch->strm,deflateFlag)) < 0) { deflateEnd(&arch->strm); debug(INFO, 0,"Zlib deflate error.\n"); return -1; }
		if (arch->strm.avail_out != FBUFSIZE) {
			sha1Update(compressBuf,FBUFSIZE-arch->strm.avail_out);
			arch->fileBytes += FBUFSIZE - arch->strm.avail_out;
			n = flushBufferToArchive(compressBuf,FBUFSIZE-arch->strm.avail_out,arch);
			if (n != (FBUFSIZE-arch->strm.avail_out)) { deflateEnd(&arch->strm); debug(INFO, 0,"Stream length mismatch\n"); return -1; }
			}
		} while(arch->strm.avail_out == 0);
	if (!size) { deflateEnd(&arch->strm); arch->state &= ~COMPRESSED; }
	return 1;
	}

#ifdef LIBLZMA
int lzmaCompress(archive *arch, unsigned char *buf, int size) {
        int deflateFlag = (size)?LZMA_RUN:LZMA_FINISH;
        int n;
	int res;
        arch->lstr.avail_in = size;
        arch->lstr.next_in = buf; // buf, but we should be able to leave it at null if we're done
        do {
                arch->lstr.avail_out = FBUFSIZE;
                arch->lstr.next_out = compressBuf;
		res = lzma_code(&arch->lstr,deflateFlag);
		if (res != LZMA_OK && (!size && res != LZMA_STREAM_END)) { lzma_end(&arch->lstr); debug(INFO, 0,"LZMA deflate error.\n"); return -1; }
		if (arch->lstr.avail_out != FBUFSIZE) {
			sha1Update(compressBuf,FBUFSIZE-arch->lstr.avail_out);
                	arch->fileBytes += FBUFSIZE - arch->lstr.avail_out;
                	n = flushBufferToArchive(compressBuf,FBUFSIZE-arch->lstr.avail_out,arch);
                	if (n != (FBUFSIZE-arch->lstr.avail_out)) { lzma_end(&arch->lstr); debug(INFO, 0,"Stream length mismatch\n"); return -1; }
			}
                } while(res != LZMA_STREAM_END && arch->lstr.avail_out == 0);
        if (!size) { lzma_end(&arch->lstr); arch->state &= ~COMPRESSED; }
	return 1;
        }
#endif

int initCompressor(archive *arch, char inflate) {
	if (arch->state & GZIP) {
		bzero(&arch->strm,sizeof(z_stream));
                arch->strm.zalloc = Z_NULL;
                arch->strm.zfree = Z_NULL;
                arch->strm.opaque = Z_NULL;
                if (!inflate && (deflateInit2(&arch->strm,Z_DEFAULT_COMPRESSION,Z_DEFLATED,windowBits | GZIP_ENCODING,9,Z_DEFAULT_STRATEGY) < 0)) { debug(INFO, 0,"Zlib init error.\n"); return -1; }
		if (inflate && (inflateInit2(&arch->strm, windowBits | ENABLE_ZLIB_GZIP) < 0)) { debug(INFO, 0,"Zlib init error.\n"); return -1; }
		}
#ifdef LIBLZMA
	else {
		bzero(&arch->lstr,sizeof(lzma_stream));
		if (inflate && (lzma_auto_decoder(&arch->lstr,-1,0) != LZMA_OK)) { debug(INFO, 0,"LZMA init error.\n"); return -1; }
		if (!inflate && (lzma_easy_encoder(&arch->lstr,1,0) != LZMA_OK)) { debug(INFO, 0,"LZMA init error.\n"); return -1; }
		}
#else
	else return -1;
#endif
	return 1;
	}

int gzipDecompress(unsigned char *buf, int size, archive *arch) {
        int res;
        int n = 0;
        while(1) {
                while(arch->strm.avail_in) {
                        arch->strm.next_out = buf;
                        arch->strm.avail_out = size;
                        if ((res = inflate(&arch->strm,Z_NO_FLUSH)) < 0) { debug(INFO, 0,"Zlib inflate error.\n"); inflateEnd(&arch->strm); return -1; }
                        n = size-arch->strm.avail_out;
                        arch->originalBytes += n;
                        if (res == Z_STREAM_END) {
                                arch->state &= ~COMPRESSED; // no more compression to do
                                inflateEnd(&arch->strm);
                                if (arch->fileBytes != arch->fileSizePosition) return -1;
                                if (arch->originalBytes != arch->expectedOriginalBytes) return -1;
                                if (!n) return readSignature(arch,1);
                                return n;
                                }
                        if (n) return n;
                        }
                if ((n = readFile(compressBuf,FBUFSIZE,arch,0)) > 0) {
                        arch->strm.next_in = compressBuf;
                        arch->strm.avail_in = n;
                        }
                else {
                        arch->state &= ~COMPRESSED;
                        inflateEnd(&arch->strm);
                        if (!n) return readSignature(arch,1);
                        return n;
                        }
                }
        }

#ifdef LIBLZMA
int lzmaDecompress(unsigned char *buf, int size, archive *arch) {
        int res;
        int n = 0;
        while(1) {
                while(arch->lstr.avail_in) {
                        arch->lstr.next_out = buf;
                        arch->lstr.avail_out = size;
			res = lzma_code(&arch->lstr,LZMA_RUN);
			if ((res != LZMA_OK) && (res != LZMA_STREAM_END)) { debug(INFO, 0,"Zlib inflate error.\n"); lzma_end(&arch->lstr); return -1; }
                        n = size-arch->lstr.avail_out;
                        arch->originalBytes += n;
                        if (res == LZMA_STREAM_END) {
                                arch->state &= ~COMPRESSED; // no more compression to do
                                lzma_end(&arch->lstr);
                                if (arch->fileBytes != arch->fileSizePosition) return -1;
                                if (arch->originalBytes != arch->expectedOriginalBytes) return -1;
                                if (!n) return readSignature(arch,1);
                                return n;
                                }
                        if (n) return n;
                        }
                if ((n = readFile(compressBuf,FBUFSIZE,arch,0)) > 0) {
                        arch->lstr.next_in = compressBuf;
                        arch->lstr.avail_in = n;
                        }
                else {
                        arch->state &= ~COMPRESSED;
                        lzma_end(&arch->lstr);
                        if (!n) return readSignature(arch,1);
                        return n;
                        }
                }
        }
#endif

int compressBuffer(archive *arch, unsigned char *buf, int size) {
	if (arch->state & GZIP) return gzipCompress(arch,buf,size);
#ifdef LIBLZMA
	else return lzmaCompress(arch,buf,size);
#else
	else return -1;
#endif
	}

int readCompressed(unsigned char *buf, int size, archive *arch) {
	if (arch->state & GZIP) return gzipDecompress(buf,size,arch);
#ifdef LIBLZMA
	else return lzmaDecompress(buf,size,arch);
#else
	else return -1;
#endif
	}

/****************************
        WRITE ARCHIVE FUNCTIONS
****************************/

int signFile(archive *arch) {
	int n;
	if (arch->fileHeaderFD == -1) { arch->state &= ~COMPRESSED; return 1; } // file wasn't open
	if ((arch->state & COMPRESSED) && (compressBuffer(arch,NULL,0) == -1)) return -1;
	lseek64(arch->fileHeaderFD,arch->fileSizePosition,SEEK_SET);
	n = write(arch->fileHeaderFD,&arch->fileBytes,LSIZE); // write file size here
	if (n != LSIZE) { debug(INFO, 0,"ERROR WRITING %i BYTES!\n",LSIZE); return -1; }
	n = write(arch->fileHeaderFD,&arch->originalBytes,LSIZE);
	if (n != LSIZE) { debug(INFO, 0,"ERROR WRITING %i BYTES!\n",LSIZE); return -1; }
	if (arch->fileHeaderFD != arch->currentFD) close(arch->fileHeaderFD);
	else { lseek64(arch->fileHeaderFD,0,SEEK_END); } // could also use the known segmentOffset value instead and do SEEK_SET; will probably do that instead
	arch->fileHeaderFD = -1;
	sha1Finalize();
	bzero(sha1buf,24);
        memcpy(sha1buf,"SHA1",4);
        if (md_value != NULL && arch_md_len) memcpy(&sha1buf[4],md_value,20);
	// printf("BUF: "); for(n = 4;n<24;n++) printf("%02X",sha1buf[n]); printf("\n");
	if (flushBufferToArchive(sha1buf,24,arch) != 24) return -1;
	return 1;
	}

void closeArchive(archive *arch) {
	if (arch->currentFD == -1) return;
	if (!(arch->state & ARCH_READ)) {
		signFile(arch); // ignore errors for now
		if ((arch->fileHeaderFD != -1) && (arch->fileHeaderFD != arch->currentFD)) { close(arch->fileHeaderFD); fsync(arch->fileHeaderFD); }
		}
#ifdef NETWORK_ENABLED
	if (!strncmp(arch->archiveName,"http://",7)) {
		closeHTTPFile(arch->currentFD);
		}
	else {
		close(arch->currentFD);
		fsync(arch->currentFD);
		}
#else
	close(arch->currentFD);
	fsync(arch->currentFD);
#endif
	arch->currentFD = -1;
	// printf("Processed %lu bytes so far.\n",arch->totalOffset);
	}

int addFileToArchive(unsigned int major, unsigned int minor, unsigned char compression, archive *arch) {
	if (signFile(arch) == -1) return -1;
	int n, offset = 0;
	arch->fileBytes = 0;
	arch->originalBytes = 0;
	arch->state |= compression; // add compression setting
	sha1Init();
	// unsigned char fsize = strlen(filename);
	if (arch->splitSize && ((arch->splitSize - arch->segmentOffset) < L2SIZE)) {
		offset = arch->splitSize - arch->segmentOffset;
		n = write(arch->currentFD,&arch->fileBytes,offset); // originalBytes should be after fileBytes in struct; otherwise we'll have a boundary issue
		close(arch->currentFD);
		if (writeArchiveHeader(arch) == -1) return -1;
		}
	arch->fileHeaderFD = arch->currentFD;
	arch->fileSizePosition = arch->segmentOffset;
	if (flushBufferToArchive((char *)&arch->fileBytes,LSIZE,arch) != LSIZE) return -1; // zero filesize
	if (flushBufferToArchive((char *)&arch->originalBytes,LSIZE,arch) != LSIZE) return -1;
	if (flushBufferToArchive(&compression,1,arch) != 1) return -1;
	if (flushBufferToArchive((char *)&major,sizeof(unsigned int),arch) != sizeof(unsigned int)) return -1;
	if (flushBufferToArchive((char *)&minor,sizeof(unsigned int),arch) != sizeof(unsigned int)) return -1;
	arch->major = major;
	arch->minor = minor;
	if (arch->state & COMPRESSED) return initCompressor(arch,0);
	return 1;
	}

int writeFile(char *buf,int size,archive *arch) {
	int n;
	if (!size) return 0; // nothing to write
	arch->originalBytes += size;
	if (arch->state & COMPRESSED) {
		if (compressBuffer(arch,buf,size) == -1) return -1;
		}
	else {
		sha1Update(buf,size);
        	if (flushBufferToArchive(buf,size,arch) != size) return -1;
        	arch->fileBytes += size;
		}
	return size;
	}

// reads from fd and writes to the archive
int writeBlock(int fd, unsigned long size, archive *arch, bool progress) {
	int n;
	unsigned long bytes = 0;
	while (bytes < size && (n = read(fd,fileBuf,((size-bytes) < FBUFSIZE)?(size-bytes):FBUFSIZE)) > 0) {
		bytes += n;
		// if (writeFile(fileBuf,n,arch) != n) return -1;  // should do error checking on operations
		if (writeFile(fileBuf,n,arch) == -1) return -1;
		if (progress) { if (progressBar(arch->originalBytes,arch->fileBytes,PROGRESS_UPDATE)) { feedbackComplete("*** CANCELLED ***"); return -2; } }
		}
	if (bytes != size) return -1;
	return 0; // all ok
	}

// logic here to calculate maximum file size. File looks like so: ########|TITLE\0|<file>|SHA1SUM. So if there are less than 8 bytes left, pad them with zeroes. (do so when creating the archive filename, not here)

int flushBufferToArchive(unsigned char *buf, unsigned int size,archive *arch) {
        int n;
        unsigned int offset = 0;
        if (arch->splitSize && (size > (arch->splitSize - arch->segmentOffset))) {
		offset = arch->splitSize - arch->segmentOffset;
                if (flushBufferToArchive(buf,offset,arch) != offset) { debug(INFO, 0,"Stream segment flush error.\n"); return -1; }
                if (arch->currentFD != arch->fileHeaderFD) close(arch->currentFD); // can close segment; it doesn't contain the length header
                if (writeArchiveHeader(arch) == -1) return -1;
		n = flushBufferToArchive(&buf[offset],size-offset,arch);
		if (n == -1) return -1;
                return offset + n;
                }
        while(size && (n = write(arch->currentFD,&buf[offset],size)) > 0) {
                offset += n;
                size -= n;
                }
        arch->totalOffset += offset;
        arch->segmentOffset += offset;
	return offset;
        }

int writeArchiveHeader(archive *arch) {
        int bufSize = VSIZE+ISIZE+LSIZE;
	int nameOffset = 0;
        sprintf(hdr,VERSTRING); // image and version of this image
	memcpy(&hdr[VSIZE],&arch->currentSplit,ISIZE);
        memcpy(&hdr[VSIZE+ISIZE],&arch->timestamp,LSIZE);
        if (arch->currentSplit) {
		nameOffset = strlen(arch->archiveName);
                sprintf(&arch->archiveName[nameOffset],".%i",arch->currentSplit);
                }
	unlink(arch->archiveName); // just in case
        if ((arch->currentFD = open(arch->archiveName,O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
		if (nameOffset) arch->archiveName[nameOffset] = 0;
		debug(INFO, 0,"Unable to create archive %s\n",arch->archiveName); return -1;
		}
	if (nameOffset) arch->archiveName[nameOffset] = 0;
        arch->segmentOffset = 0;
        arch->currentSplit++;
        if (flushBufferToArchive(hdr,bufSize, arch) != bufSize) return -1;
	return 1;
        }

int createImageArchive(char *filename, unsigned int segmentSize, archive *arch) {
	arch->currentFD = -1;
	arch->archiveName = filename; // keep the pointer passed to it; make sure we're careful with it
        arch->currentSplit = arch->totalOffset = 0;
        arch->fileHeaderFD = -1;
        arch->timestamp = time(NULL);
        arch->splitSize = (unsigned long) segmentSize * 1024 * 1024; // segment size is megabytes
// printf("Split: %lu\n",arch->splitSize);
        arch->state = ARCH_WRITE;
        return writeArchiveHeader(arch);
        }

/*******************************************
	READ ARCHIVE SECTION
********************************************/

int readSignature(archive *arch, char checkSum) {
	int n;
	unsigned char sha1display[41];
	unsigned long remaining = arch->fileSizePosition - arch->fileBytes;
	bzero(sha1buf,24);
	if (arch->fileHeaderFD == -1) return 0; // end of file
	if (remaining) { // skip to end of file
		if (checkSum) return -1; // can't skip file and get valid checksum
		while(1) {
			remaining = arch->fileSizePosition - arch->fileBytes;
			if (arch->splitSize - arch->segmentOffset <= remaining) { // look for the next segment
				arch->totalOffset += (arch->splitSize - arch->segmentOffset);
				arch->fileBytes += (arch->splitSize - arch->segmentOffset);
				if (readArchiveHeader(arch) != 1) return -1;
				}
			else break;
			} // while
		// printf("Seeking: %lu\n",remaining);
#ifdef NETWORK_ENABLED
		if (!strncmp(arch->archiveName,"http://",7)) seekHTTPFile(arch->currentFD,remaining); else
#endif
                lseek64(arch->currentFD,remaining,SEEK_CUR);
                arch->totalOffset += remaining;
                arch->segmentOffset += remaining;
                arch->fileBytes += remaining;
                }
	arch->fileHeaderFD = -1;
// if (checkSum && (arch->state & COMPRESSED)) printf("Comp: %lu %lu\n",arch->originalBytes,arch->expectedOriginalBytes);
// printf("Read: %lu %lu\n",arch->fileSizePosition, arch->fileBytes);
	if (checkSum && (arch->state & COMPRESSED) && (arch->originalBytes != arch->expectedOriginalBytes)) return -1;
	if ((n = readBufferFromArchive(fileBuf,24,arch)) != 24) return -1;
	if (memcmp(fileBuf,"SHA1",4)) return -1;
	if (checkSum) {
			sha1Finalize();
                        memcpy(sha1buf,"SHA1",4);
// printf("RBUF: "); for(n = 0;n<20;n++) printf("%02X",md_value[n]); printf("\n");
			if (md_value != NULL && arch_md_len) memcpy(&sha1buf[4],md_value,20);
			for (n=4;n<24;n++) sprintf(&sha1display[(n-4) << 1],"%02X",sha1buf[n]);
			sha1display[41] = 0;
			debug(INFO, 1,"SHA1: %s\n",sha1display);
                        if (memcmp(fileBuf,sha1buf,24)) {
                                debug(INFO, 0,"SHA1SUM mismatch!\n");
                                debug(INFO, 0,"     Got: %s\n",sha1display);
				for (n=4;n<24;n++) sprintf(&sha1display[(n-4) << 1],"%02X",fileBuf[n]);
				debug(INFO, 0,"Expected: %s\n",sha1display);
				return -1;
                                }
		}
	return 0;
	}

// int readCompressFile(unsigned char *buf, int size, archive *arch) {


// -1 == problem, 0 == nothing else, n = buffer read
int readFile(unsigned char *buf, int size, archive *arch, int inflate) {
	int n, res;
	if (!size) return 0;
	if ((arch->state & BUFFERED) == BUFFERED) { // just return values from the buffer (typically MBR stuff)
		if ((arch->fileBytes + size) > arch->originalBytes) return -1; // out-of-bounds
		memcpy(buf,&mbrBuf[arch->fileBytes],size);
		arch->fileBytes += size;
		return size;
		}
	if (inflate && (arch->state & COMPRESSED)) return readCompressed(buf, size, arch);
	unsigned long remaining = arch->fileSizePosition - arch->fileBytes;
	if (remaining < size) size = remaining;
	if (!remaining) return readSignature(arch,1); //  1 == validate SHA1SUM
	if ((n = readBufferFromArchive(buf,size,arch)) > 0) {
		sha1Update(buf,n);
		arch->fileBytes += n;
		if (inflate) arch->originalBytes += n;
		}
	else return -1; // read error
	return n;
	}

// reads from archive and writes to fd
int readBlock(int fd, unsigned long size, archive *arch, bool progress) {
        int n, i, offset;
        unsigned long bytes = 0;

        while (bytes < size && (n = readFile(fileBuf,((size-bytes) < FBUFSIZE)?(size-bytes):FBUFSIZE,arch,1)) > 0) {
                bytes += n;
                // if (writeFile(fileBuf,n,arch) != n) return -1;  // should do error checking on operations
		offset = 0;
		while (n > 0 && ((i = write(fd,&fileBuf[offset],n)) > 0)) { offset += i; n -= i; }
		if (n != 0) return -1; // should have written all
		if (progress) {
			if (progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_UPDATE)) { feedbackComplete("*** CANCELLED ***"); return -2; }
			progressBar(arch->originalBytes,arch->originalBytes,PROGRESS_UPDATE | 1); // global counter
			}
                }
        if (bytes != size) return -1;
        return 0; // all ok
        }

int readNextFile(archive *arch, char decompress) {
	char size;
	int n;
	unsigned long remaining;
	if (arch->currentFD == -1) return -1;
	if (arch->fileHeaderFD != -1 && (readSignature(arch,0) != 0)) return -1;
	remaining = arch->splitSize - arch->segmentOffset;
	if (remaining < L2SIZE) {
		arch->totalOffset += remaining; // skip these bytes
		n = readArchiveHeader(arch);
		if (n != 1) return n; // could be 0, implying no more files
		}
	if (readBufferFromArchive((char *)&arch->fileSizePosition,LSIZE,arch) != LSIZE) return -1; // expected size of file
	if (readBufferFromArchive((char *)&arch->expectedOriginalBytes,LSIZE,arch) != LSIZE) return -1; // uncompressed file size
	if (readBufferFromArchive((char *)&arch->state,1,arch) != 1) return -1;
	arch->state |= ARCH_READ;
	if (!decompress) arch->state &= ~COMPRESSED; // don't decompress

	/* IF STATE IS PART_ALIAS, then go to the major/minor encapsulated in the fileSizePosition location. expectedOriginalBytes is zero */
	/* there is no MD5SUM endpoint for this type of file. The filesize of 0 should be enough location data. This is transparent to the top-level application. Implement this later. */
	// if (readBufferFromArchive(&size,1,arch) != 1) return -1;
	// if (readBufferFromArchive(arch->filename,size,arch) != size) return -1;
	// arch->filename[size] = 0;
	if (readBufferFromArchive((char *)&arch->major,sizeof(unsigned int),arch) != sizeof(unsigned int)) return -1;
	if (readBufferFromArchive((char *)&arch->minor,sizeof(unsigned int),arch) != sizeof(unsigned int)) return -1;
	// printf("Current file: %i:%i, length: %lu, uncompressed %lu, compression %s\n",arch->major,arch->minor,arch->fileSizePosition,arch->expectedOriginalBytes,(arch->state & GZIP)?"gzip":(arch->state & LZMA)?"lzma":"none");
	arch->fileBytes = 0;
	arch->originalBytes = 0;
	arch->fileHeaderFD = 1; // so we know we're reading a file
	sha1Init();
	if (arch->state & COMPRESSED) return initCompressor(arch,1);
	return 1;
	}

int readSpecificFile(archive *arch, int major, int minor, char decompress) {
	int n;
	char lastPass = 0;
	if (arch->fileHeaderFD != -1 && arch->fileBytes == 0 && arch->major == major && arch->minor == minor) {
#ifndef LIBLZMA
		if (decompress && ((arch->state & COMPRESSED) == LZMA)) return -1;
#endif
		return 1; // already there
		}
	if (arch->currentFD == -1) { // re-open archive from beginning
		lastPass = 1;
		if ((n = readImageArchive(arch->archiveName,arch)) == -1) return -1;
		if (!n) return 0; // nothing found
		}
	while (1) {
		if ((n = readNextFile(arch,decompress)) == -1) return -1;
		if (!n) {
			if (lastPass) return 0; // couldn't find file; archive should already be closed
			if ((n = readImageArchive(arch->archiveName,arch)) == -1) return -1;
			if (!n) return 0;
			lastPass = 1;
			}
		else if (arch->major == major && arch->minor == minor) return 1;
		}
	}

int readBufferFromArchive(unsigned char *buf, unsigned int limit, archive *arch) {
	int n;
	unsigned int offset = 0;
	if (arch->currentFD == -1) return -1;
	while(limit && (n =
#ifdef NETWORK_ENABLED
		(!strncmp(arch->archiveName,"http://",7))?readHTTPFile(arch->currentFD,&buf[offset],limit):
#endif
		read(arch->currentFD,&buf[offset],limit)) > 0) {
		offset += n;
		limit -= n;
		}
#ifdef NETWORK_ENABLED
	if (has_interrupted) return -1;
	else
#endif
	if (n < 0) perror("File error");
	arch->totalOffset += offset;
	arch->segmentOffset += offset;
	if (limit) { // Assume EOF. See if there's another file
		if (readArchiveHeader(arch) != 1) { closeArchive(arch); return -1; }
		n = readBufferFromArchive(&buf[offset],limit,arch);
		offset += n;
		limit -= n;
		}
	if (limit) { closeArchive(arch); return -1; }
	return offset;
	}

// 0 = no more, 1 = ok, -1 = not ok
int readArchiveHeader(archive *arch) {
	unsigned char hdr[HDRSIZE];
	int n, size = 0, remaining = VSIZE+ISIZE+LSIZE;
	time_t timestamp;
	unsigned int segment, offset = 0;
	struct stat64 stats;
	if (arch->currentFD != -1) closeArchive(arch); // { close(arch->currentFD); arch->currentFD = -1; }
	if (arch->currentSplit) {
		offset = strlen(arch->archiveName);
		sprintf(&arch->archiveName[offset],".%i",arch->currentSplit);
		}
#ifdef NETWORK_ENABLED
	if (!strncmp(arch->archiveName,"http://",7)) {
		stats.st_size = statHTTPFile(arch->archiveName);
		if (!stats.st_size) { if (offset) arch->archiveName[offset] = 0 ; return 0; }
		}
	else
#endif
	if (stat64(arch->archiveName,&stats)) {
		if (offset) arch->archiveName[offset] = 0;
		return 0;
		}
	arch->archiveSize += stats.st_size;
	arch->splitSize = stats.st_size; // total size of this file being read
	if (arch->splitSize < remaining) { if (offset) arch->archiveName[offset] = 0; return -1; }
#ifdef NETWORK_ENABLED
	if (!strncmp(arch->archiveName,"http://",7)) {
		if ((arch->currentFD = openHTTPFile(arch->archiveName)) < 0 ) { if (offset) arch->archiveName[offset] = 0; return -1; }
		}
	else
#endif
	if ((arch->currentFD = open(arch->archiveName,O_RDONLY | O_LARGEFILE)) < 0) { if (offset) arch->archiveName[offset] = 0; return -1; }
	if (offset) arch->archiveName[offset] = 0;
	arch->segmentOffset = 0;
	arch->currentSplit++;
	n = readBufferFromArchive(hdr,remaining,arch);
	if (n != remaining) { closeArchive(arch); return -1; } // need full header
	if (memcmp(VERSTRING,hdr,VSIZE)) { closeArchive(arch); debug(INFO, 0,"Segment header mismatch.\n"); return -1; }
	memcpy(&timestamp,&hdr[VSIZE+ISIZE],LSIZE);
	if (arch->currentSplit == 1) arch->timestamp = timestamp;
	else if (arch->timestamp != timestamp) { closeArchive(arch); debug(INFO, 0,"Segment timestamp mismatch for file %s\n",arch->archiveName); return -1; }
	memcpy(&segment,&hdr[VSIZE],ISIZE);
	if (segment != (arch->currentSplit-1)) { closeArchive(arch); debug(INFO, 0,"Incorrect segment number [%i, expected %i].\n",segment,arch->currentSplit-1); return -1; }
	return 1;
	}

int readImageArchive(char *filename, archive *arch) {
	arch->currentFD = -1;
	arch->archiveName = filename;
	arch->currentSplit = arch->totalOffset = arch->archiveSize = 0;
	arch->fileHeaderFD = -1;
	arch->state = ARCH_READ;
	return readArchiveHeader(arch);
	}

/**********************************
     IMAGE ENGINE
********************************/

int archiveSegments(char *path, unsigned long *archSize) {
        archive arch;
        int n;
        int splitCount = 1;
        if (readImageArchive(path,&arch) != 1) return 0;
        while ((n = readArchiveHeader(&arch)) > 0) splitCount++;
        closeArchive(&arch);
        if (n) return 0; // damaged
        *archSize = arch.archiveSize;
        return splitCount;
        }

// this should only run if the source or target starts with /mnt/ram
int copySingleFile(char *source, char *target) { // includes verification
	int fd1, fd2;
	sha1Init();
	int n, n2, offset;
	if ((fd1 = open(source,O_RDONLY | O_LARGEFILE)) < 0) return 0; // file doesn't exist
	if (target != NULL) { // copy it here
		if ((fd2 = open(target,O_WRONLY | O_TRUNC | O_CREAT | O_LARGEFILE, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) { debug(INFO, 1,"Unable to create file %s.\n",target); return -1; }
		}
	while((n = read(fd1,fileBuf,FBUFSIZE)) > 0) {
		sha1Update(fileBuf,n);
		if (target != NULL) {
			offset = 0;
			while(n > 0 && ((n2 = write(fd2,&fileBuf[offset],n))> 0)) { offset += n2; n-=n2; }
			if (n) { close(fd1); close(fd2); debug(INFO, 1,"Unable to copy to file %s.\n",target); return -1; }
			}
		}
	sha1Finalize();
	close(fd1);
	if (target != NULL) { // verify what we copied
		fsync(fd2);
		close(fd2);
		memcpy(sha1buf,md_value,20); // store original sha1sum
		return copySingleFile(target,NULL);
		}
	else return (!memcmp(sha1buf,md_value,20))?1:-1; // verify the sha1sum
	}

#define CUSTOMFILES 2
void customFilesAppend(char *source, char *target) { // source and target need to match here for simplicity
	const char *customFiles[] = { "arcsight_license", "nodes.properties" };
	char tmpStorage[MAX_FILE]; // 256
	int i, res, count=0;
	char *sfile, *tfile;
	if ((sfile = strrchr(source,'/')) == NULL) { debug(INFO, 1,"File path error for customFilesAppend().\n"); return; }
	if ((tfile = strrchr(target,'/')) == NULL) { debug(INFO, 1,"File target error for customFilesAppend().\n"); return; }
	if (strcmp(sfile,tfile)) { debug(INFO, 1,"File pair mismatch for customFilesAppend().\n"); return; }
	sfile++; tfile++;
	if (strlen(sfile) >= MAX_FILE) { debug(INFO, 1,"Filename exceeds MAX_FILE for customFilesAppend.\n"); return; }
	strcpy(tmpStorage,sfile);
	for (i = 0; i < CUSTOMFILES; i++) {
		strcpy(sfile,customFiles[i]);
		strcpy(tfile,customFiles[i]);
		res = copySingleFile(source,target);
		if (res == 1) debug(INFO, 1,"Copied %s to %s\n",source,target);
		else if (res == -1) debug(INFO, 1,"Unable to copy %s to %s\n",source,target);
		else unlink(target); // delete the corresponding file on the target, just in case
		if (res) count++;
		}
	*sfile = 0;
	if (!count) debug(INFO, 1,"No append files found in %s\n",source);
	// revert source/target so we don't mangle globalPath, globalPath2
	strcpy(sfile,tmpStorage);
	strcpy(tfile,tmpStorage);
	}

// target == NULL => verify; dev == NULL => verify file ONLY (not a copy/verify operation)
int copyImage(char *dev, char *source, char *target, bool verify) {
        unsigned long imageSize = 0;
        unsigned long totalWrite = 0;
        int splitCount;
        int i, n, n2, srclen, trglen, offset;
        int fd1, fd2;
        if (!(splitCount = archiveSegments(source,&imageSize))) return 0;
        srclen=strlen(source);
	// pre-calculate max width
	if (target != NULL) trglen = strlen(target);
	if (verify) sha1Init();
        for (i=0;i<splitCount;i++) {
		if (target != NULL) setProgress(PROGRESS_COPY,NULL,dev,splitCount,i+1,0,NULL,imageSize,0,NULL);
		else if (dev == NULL) setProgress(PROGRESS_VERIFY,NULL,source,splitCount,i+1,0,NULL,imageSize,0,NULL);
		else setProgress(PROGRESS_VERIFYCOPY,NULL,source,splitCount,i+1,0,NULL,imageSize,0,NULL);
                if (i) sprintf(&source[srclen],".%i",i);
		else progressBar(imageSize,(target != NULL)?PROGRESS_RED:PROGRESS_BLUE,PROGRESS_INIT);
#ifdef NETWORK_ENABLED
        if (!strncmp(source,"http://",7)) {
                if ((fd1 = openHTTPFile(source)) < 0 ) { debug(ABORT, 1,"Unable to read archive file %s",source); return -1; }
                }
        else
#endif
                if ((fd1 = open(source,O_RDONLY | O_LARGEFILE)) < 0) { debug(ABORT, 1,"Unable to read archive file %s",source); return -1; }
		if (target != NULL) {
			if (i) sprintf(&target[trglen],".%i",i);
			unlink(target); // just in case
                	if ((fd2 = open(target,O_WRONLY | O_TRUNC | O_CREAT | O_LARGEFILE, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) { debug(ABORT, 1,"Unable to write archive file"); return -1; }
			}
                while((n =
#ifdef NETWORK_ENABLED
			(!strncmp(source,"http://",7))?readHTTPFile(fd1,fileBuf,FBUFSIZE):
#endif
			read(fd1,fileBuf,FBUFSIZE)
			) > 0) {
			if (verify) sha1Update(fileBuf,n);
			if (target != NULL) {
                        	offset = 0;
                        	while(n > 0 && ((n2 = write(fd2,&fileBuf[offset],n)) > 0)) { offset += n2; n-= n2; }
                        	if (n != 0) {
#ifdef NETWORK_ENABLED
					if (!strncmp(source,"http://",7)) closeHTTPFile(fd1); else
#endif
					close(fd1);
					close(fd2); debug(ABORT, 1,"Unable to completely write archive file"); return -1; }
				}
			else offset = n;
                        totalWrite += offset;
                        if (progressBar(totalWrite, totalWrite, PROGRESS_UPDATE)) {
				if (target != NULL) close(fd2);
#ifdef NETWORK_ENABLED
				if (!strncmp(source,"http://",7)) closeHTTPFile(fd1); else
#endif
				close(fd1);
				feedbackComplete("*** CANCELLED ***");
				return -2;
				}
			if (target != NULL) progressBar(totalWrite,totalWrite,PROGRESS_UPDATE | 1); // global counter
                        }
		if (target != NULL) {
			startTimer(2);
			progressBar(totalWrite, totalWrite, PROGRESS_SYNC);
			fsync(fd2);
			stopTimer();
			close(fd2);
			target[trglen] = 0;
			}
#ifdef NETWORK_ENABLED
		if (!strncmp(source,"http://",7)) {
			closeHTTPFile(fd1);
			if (has_interrupted) {
				if (target != NULL) close(fd2);
				progressBar(totalWrite,totalWrite,PROGRESS_UPDATE);
				feedbackComplete("*** CANCELLED ***");
				return -2;
				}
			} else
#endif
                close(fd1);
                if (n != 0) { debug(ABORT, 1,"Unable to completely read archive file"); return -1; }
                source[srclen] = 0;
                }
	if (verify) sha1Finalize();
	if (verify && md_value != NULL && arch_md_len) {
		if (target == NULL && dev != NULL) return (!memcmp(dev,md_value,20))?1:-1; // md_val only if target == NULL
		// bzero(sha1buf,20);
		memcpy(sha1buf,md_value,20); // verify next
		if (target != NULL) {
			prepareOperation("  VERIFYING TARGET RESTORE IMAGE  ");
			if (copyImage(sha1buf,target,NULL,true) > 0) { // verify a copy operation -- do not wait for enter key after this
				progressBar(0,totalWrite,PROGRESS_COMPLETE_NOWAIT);
				}
			else {
				debug(INFO, 1,"Image did not verify.\n");
				progressBar(0,totalWrite,PROGRESS_FAIL); return -1; }
			}
		else { // print SHA1 of entire archive
			for (i=0;i<20;i++) sprintf(&globalBuf[40+i*2],"%02x",md_value[i]);
			sprintf(&globalBuf[40+strlen(&globalBuf[40])]," %i segment%s",splitCount,(splitCount > 1)?"s":"");
			progressBar(0,totalWrite,PROGRESS_COMPLETE);
			}
		}
	else if (target == NULL) return -1;
	else if (!verify) {
		progressBar(0,totalWrite,PROGRESS_COMPLETE); return 1; } // didn't verify
	else progressBar(0,totalWrite,PROGRESS_FAIL); // can't verify
	return 1; // all ok
        }

unsigned long getImageTitle(char *filename, char *buf, archive *arch) {
	unsigned long archSize = 1;
        char closeOnEnd = 0;
	int n;
        archive tmpArch;
        if (arch == NULL) { arch = &tmpArch; closeOnEnd = 1; }
	else if (!archiveSegments(filename,&archSize)) return 0;
        if (readImageArchive(filename,arch) != 1) return 0;
        if (readNextFile(arch,1) != 1) { closeArchive(arch); return 0; }
        if (!arch->major && !arch->minor) {
                if (readFile(buf,256,arch,1) != 256) { closeArchive(arch); return 0; } // read loops are implemented in the actual readFile command, so it's all or nothing here
                buf[255] = 0;
                }
        else { closeArchive(arch); return 0; }
        if (closeOnEnd) closeArchive(arch);
	return archSize;
        }

char getType(char *path) {
        char mime[12];
        int n, fd = open(path,O_RDONLY | O_LARGEFILE);
        if (fd < 0) return -1;
        n = read(fd,mime,12);
        close(fd);
        if (n != 12 || memcmp(mime,VERSTRING,8)) return -1; // didn't read 16, or not an image
        if (*((int *)&mime[8])) return 0; // check sequence number; needs to be 0 (first archive)
        return 1; // archive, sequence #0
        }

void reSignIndex(char *path) {
        archive arch;
        int n, fd, m;
        unsigned long offset;
        if (readImageArchive(path, &arch) != 1) debug(EXIT, 1,"Unable to open archive\n");
        if (readSpecificFile(&arch,0,0,0) != 1) debug(EXIT, 1,"Unable to find index\n");
        bzero(sha1buf,24);
        while ((n = readFile(fileBuf,FBUFSIZE,&arch,0)) > 0) { ; } // calculate sha1sum
        if (strncmp(sha1buf,"SHA1",4)) debug(EXIT, 1,"Signature issue; archive damaged.\n");
        if (arch.currentSplit > 1) debug(EXIT, 1,"Can only re-sign primary segment.\n");
        offset = arch.segmentOffset - 24;
        closeArchive(&arch);
        if (n != 0) {
		if ((fd = open(path,O_WRONLY | O_LARGEFILE)) < 0) debug(EXIT, 1,"Unable to write to %s\n",path);
		lseek64(fd,offset,SEEK_SET);
		m = 0;
		while((m < 24) && ((n = write(fd,&sha1buf[m],24-m)) > 0)) m += n;
		if (m < 24) debug(EXIT, 1,"Error writing to %s\n",path);
		close(fd);
		}
        }

int verifyImage(char *path) {
	archive arch;
	int n;
	unsigned long imageSize = 0; // total image size
	int startSegment, splitCount;
	if (!(splitCount = archiveSegments(path,&imageSize))) { debug(ABORT, 0,"Archive damaged"); return -1; }
       	if (readImageArchive(path,&arch) != 1) return -1;
	setProgress(PROGRESS_VALIDATE,NULL,path,0,0,0,NULL,imageSize,0,&arch);
	progressBar(imageSize,PROGRESS_BLUE,PROGRESS_INIT | 1);
        while((n = readNextFile(&arch,1)) == 1) {
		setProgress(PROGRESS_VALIDATE,NULL,path,arch.major,arch.minor,0,NULL,imageSize,0,&arch);
		startSegment = arch.currentSplit;
		progressBar(arch.expectedOriginalBytes,PROGRESS_BLUE,PROGRESS_INIT);
		while((n = readFile(fileBuf,FBUFSIZE,&arch,1)) > 0) {
			if (progressBar(arch.originalBytes,arch.fileBytes,PROGRESS_UPDATE)) { closeArchive(&arch); feedbackComplete("*** CANCELLED ***"); return -2; }
			progressBar(arch.totalOffset,arch.totalOffset,PROGRESS_UPDATE | 1);
			}
#ifdef NETWORK_ENABLED
                if (has_interrupted) {
			progressBar(arch.originalBytes,arch.fileBytes,PROGRESS_UPDATE);
			closeArchive(&arch);
                        feedbackComplete("*** CANCELLED ***");
                        return -2;
                        }
#endif
		if (n) { closeArchive(&arch); progressBar(0,arch.fileBytes,PROGRESS_FAIL); return -1; }
		progressBar(0,arch.fileBytes,PROGRESS_OK);
		}
	progressBar(imageSize,imageSize,PROGRESS_UPDATE | 1); // make 100%
	progressBar(0,arch.fileBytes,PROGRESS_COMPLETE);
	closeArchive(&arch);
	return 1;
	}

void initHashEngine(void) {
#ifdef SSL
        OpenSSL_add_all_digests();
        md = EVP_get_digestbyname("sha1");
#else
#ifdef GCRYPT
        gcry_control(GCRYCTL_DISABLE_SECMEM,0);
        if (gcry_md_open(&digest,GCRY_MD_SHA1,GCRY_MD_FLAG_SECURE)) debug(EXIT, 1,"Can't create SHA1SUM digest.\n");
        if ((arch_md_len = gcry_md_get_algo_dlen(GCRY_MD_SHA1)) != 20) arch_md_len = 0;
#endif
#endif
	}

/*************************************
     TEST CODE
**************************************/

// see fileEngineTest.c
