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

#ifdef NETWORK_ENABLED
#define __USE_LARGEFILE64
// #define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli.h"
#include "fileEngine.h"	// arch_md_len
#include "mount.h"			// globalBuf, currentLine
#include "partition.h"
#include "partutil.h"		// readable
#include "sha1.h"
#include "window.h"     // globalPath, options

extern bool remoteEntry;
extern char localmount;

extern unsigned char md_value[];
extern unsigned char sha1buf[];

extern char imageSizeString[];
extern unsigned long httpImageSize;
extern char signature[];
extern unsigned long httpBytesRead;

#define MAXURLS 10
int urlCount = 0;
char *url[MAXURLS];

#define HTTP_OK		1	// 200
#define HTTP_PERMREDIR	2	// 301
#define HTTP_TEMPREDIR	3	// 307
#define HTTP_INTERRUPT  4	// Ctrl-C
#define HTTP_RANGE_OK   5	// 206

int httpResult;

/* HTTP file variables for reading HTTP files like a buffered file */
#define MAXHTTPFILES 1
#define HTTPBUFSIZE 10485760 // 10 MB per read operation
// #define HTTPBUFSIZE 131072

typedef struct __httpBuffer httpBuffer;

struct __httpBuffer {
	char *currentURL;	// set to NULL when no longer being used
	unsigned char *httpBuffer; // allocate on first-use
	unsigned long currentHTTPOffset;
	unsigned int currentHTTPBufIndex;
	unsigned int currentHTTPBufLimit; // up to HTTPBUFSIZE
        };

httpBuffer httpBuf[MAXHTTPFILES];
int allocatedHTTPBuffers = 0;
unsigned char *currentDownloadBuffer;
unsigned long startRange;
int totalCharsRead, maxCharsAllowed;
bool recentSeek = true;

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include <curl/curl.h>

int bufIndex;
selection *httpSel;
CURL *curl = NULL;
int totalRead;
unsigned long totalWrite;

char hasHeader;

char *getStringPtr(poolset *sel, char *str);

char getHTTPType(char *path);

unsigned long contentLength;

char *addNetworkMount(selection *sel, char *existing) {
	char *device, *mpoint, *type;
	int i;
        if (openFile("/proc/mounts")) return NULL; // implies /proc isn't mounted
        while(readFileLine()) {
                device = mpoint = type = NULL;
                if ( ((device = strtok(currentLine," ")) != NULL) &&
                     ((mpoint = strtok(NULL," ")) != NULL) &&
                     ((type = strtok(NULL," ")) != NULL) ) {
			if (strcmp(type,"cifs")) continue;
			*(type-1) = 0;
			if (sel != NULL) addSelection(sel,mpoint,CIFSMOUNT,0);
			else if (!strcmp(device,existing)) return mpoint; // should map to mpoint
			options |= OPT_NETWORK; // might as well enable it if there are already network mounts
			}
		}
//	addSelection(sel,"/net/cifs1","Network Filesystem",1);
// //15.214.200.215/fileserver /mnt/cifs1 cifs

        if (sel != NULL) for (i=0;i<urlCount;i++) { addSelection(sel,url[i],HTTPMOUNT,1); }  // can keep the pointers
	return NULL;
	}

#define MAX_TEMP 512
#define MAX_USER 60
#define MAX_PASS 60
#define CIFSTIMEOUT 30

int showField(WINDOW *win, char *desc) {
	if (strchr(desc,':') != NULL) {
		mvwprintw(win,1,1,"%s",desc);
		return strlen(desc)+1;
		}
	mvwprintw(win,1,1,"%s: ",desc);
	return strlen(desc)+3;
	}

char *globalCifs;
char tempBuf[MAX_TEMP];
extern volatile pthread_t threadTID;
extern volatile sig_atomic_t has_interrupted;
#include <pthread.h>


void *threadCIFSMount(void *b) {
	int *i = b;
	*i = mount(globalCifs,tempBuf,"cifs",0,globalBuf);
	}

void addCIFSPoint(selection *sel, int selMap, char *cifs, char *dblPath, char *usrbuf, char *passbuf) {
	int i, pid;
        struct stat64 sbuf;
	char *path;
	char *fbuf = &cifs[2];
	sigset_t set; sigemptyset(&set); sigaddset(&set,SIGINT);
	memcpy(cifs,"//",2);
	dblPath++;
	if ((path = strchr(dblPath,'/')) != NULL) {
		*path++ = 0;
		if (!strcmp(path,".")) *path = 0;
		}
	sprintf(tempBuf,"/mnt/cifs/%s",fbuf);
	for (i=0;i<sel->count;i++) { // see if this is already mounted
		if (sel->sources[i].diskNum == -2 && !strcmp(tempBuf,sel->sources[i].location)) break;
		}
	if (i == sel->count) { // create the mount point, and try to mount it
		sprintf(globalBuf,"user=%s,pass=%s",usrbuf,passbuf);
		dirChain(tempBuf); mkdir(tempBuf,0);
		if (ui_mode) {
			WINDOW *win = wins[location(sel)];
			wattron(win,COLOR_PAIR(CLR_GB) | A_BOLD);
			mvwprintw(win,1,1,"Attempting cifs:%s...",cifs);
			wattroff(win,COLOR_PAIR(CLR_GB) | A_BOLD);
			wrefresh(win);
			}
		startTimer(4);

		pthread_t tid;
		long status;
		globalCifs = cifs;
		has_interrupted = 0;
		pthread_create(&tid,NULL,threadCIFSMount,&i);
		threadTID = tid;
		sigprocmask(SIG_BLOCK,&set,NULL);
		pthread_join(tid,(void **)&status);
		sigprocmask(SIG_UNBLOCK,&set,NULL);
		if (has_interrupted) i = 0;
		threadTID = has_interrupted = 0;
		stopTimer();
		if (!i) { // successful mount; reload drive pages and set sel->position
			if (!sels[LIST_IMAGE].currentPathStart) populateList(&sels[LIST_IMAGE]);
			if (!sels[LIST_SOURCE].currentPathStart) populateList(&sels[LIST_SOURCE]);
			if (!sels[LIST_TARGET].currentPathStart) populateList(&sels[LIST_TARGET]);
			// highlight new mountpoint
			for (i=0;i<sel->count;i++) {
				if (sel->sources[i].diskNum == -2 && !strcmp(tempBuf,sel->sources[i].location)) sel->position = i;
				}
			}
		else path = NULL;
		}
	if (path != NULL) { // navigate to it
		localmount |= selMap;
		if (!*path) { navigateDirectory(tempBuf,sel); }
		else {
			sprintf(globalBuf,"%s/%s",tempBuf,path);
			dblPath = "";
			while(stat64(globalBuf,&sbuf)) {
				if ((dblPath = strrchr(path,'/')) != NULL) { *dblPath++ = 0; }
				else { *path = 0; break; }
				}
			if (!*path) navigateDirectory(tempBuf,sel);
			else {
				if ((S_ISDIR(sbuf.st_mode))) {
					if (!*dblPath) { // only go to parent directory
						if ((dblPath = strrchr(path,'/')) != NULL) *dblPath++ = 0;
						}
					if (dblPath == NULL) { navigateDirectory(tempBuf,sel); dblPath = path; }
					else {
						strcat(tempBuf,"/");
						strcpy(sel->currentPath,tempBuf);
						sel->currentPathStart = strlen(tempBuf);
						navigateDirectory(path,sel);
						}
					}
				else if ((dblPath = strrchr(path,'/')) != NULL) {
					*dblPath++ = 0;
					strcat(tempBuf,"/");
					strcpy(sel->currentPath,tempBuf);
					sel->currentPathStart = strlen(tempBuf);
					navigateDirectory(path,sel);
					}
				else {
					navigateDirectory(tempBuf,sel);
					}
				if (dblPath != NULL && (*dblPath)) { // set the position for a specified entry
					for (i=0;i<sel->count;i++) {
						if (!strcmp(sel->sources[i].location,dblPath)) { sel->position = i; break; }
						}
					}
				}
			}
		}
	}

size_t readHTTPListing(char *ptr, size_t size, size_t nmemb, void *userdata) {
        unsigned long remaining = size * nmemb;
        // printf("\033[31mread: \033[32m%i\033[0m\n",size*nmemb);
        while(remaining--) {
                if (bufIndex) {
                        if (*ptr == '>') {
                                globalBuf[bufIndex] = 0;
                                if (!strncmp(globalBuf,"<a href=\"",9) && globalBuf[9] != '?') {
					if (httpSel->count >= httpSel->size && expandSelection(httpSel,0)) return size * nmemb;
					if ((strlen(&globalBuf[9]) + 1) >= MAX_FILE) continue;
                                        if (!strcmp("/\"",&globalBuf[bufIndex-2])) {
                                                globalBuf[bufIndex-2] = 0;
                                                if (globalBuf[9] && (strchr(&globalBuf[9],'/') == NULL)) {
							httpSel->sources[httpSel->count].location = getStringPtr(&httpSel->pool,&globalBuf[9]);
							httpSel->sources[httpSel->count].state = 0;
							httpSel->sources[httpSel->count].identifier = NULL;
							httpSel->count++;
                                                        // debug(INFO,5,"Directory: %s\n",&globalBuf[9]);
                                                        }
                                                }
                                        else {
                                                globalBuf[bufIndex-1] = 0;
                                                if (globalBuf[9] && (strchr(&globalBuf[9],'/') == NULL)) {
							httpSel->sources[httpSel->count].location = getStringPtr(&httpSel->pool,&globalBuf[9]);
                                                        httpSel->sources[httpSel->count].state = 0;
                                                        httpSel->sources[httpSel->count].identifier = "";
							// httpSel->sources[httpSel->count].state = CLR_D
                                                        httpSel->count++;
                                                        // debug(INFO,5,"File: %s\n",&globalBuf[9]);
                                                        }
                                                }
                                        }
                                bufIndex = 0;
                                }
                        else if (bufIndex < (BUFSIZE-1)) { globalBuf[bufIndex++] = *ptr; }
                        ptr++;
                        }
                else {
                        if (*ptr++ == '<') globalBuf[bufIndex++] = '<';
                        }
                }
// <a href="V7400/">V7400/</a>
        return size*nmemb;
        }

/*
void catBuffer(char *buf, char *ptr, int bufSize, int offset, int ptrSize) {
	int remaining = totalRead + ptrSize;
	if (remaining <= offset) return;
	if ((totalRead+offset) >= bufSize) return;
*/

typedef struct __readBuf readBuf;
struct __readBuf {
	char *buf;
	int offset;
	int limit;
	int current;
	};

char addToBuffer(char *ptr, int len, readBuf *rb) {
	int spaceLeft = rb->limit - rb->current;
	int ptrOffset = 0;
	if (totalRead > rb->limit+rb->offset) return 1; // out of bounds
	if (totalRead < rb->offset) { // take out the offset first
		ptrOffset = rb->offset - totalRead;
		if (ptrOffset > len) return -1; // out of bounds
		len -= ptrOffset;
		}
	if (spaceLeft < len) len = spaceLeft;
	if (spaceLeft) memcpy(&rb->buf[rb->current],&ptr[ptrOffset],spaceLeft);
	rb->current += spaceLeft;
	if (rb->current == rb->limit) return 1;
	return 0;
	}

char mime_buf[20]; // also allow reading of filesize/compsize/status/major/minor for fspec
char file_buf[25]; //
char title_buf[256];
imageEntry iE;
unsigned long *fileSize = (unsigned long *)file_buf;
unsigned long *uncompSize = (unsigned long *)&file_buf[8];
// compression = &file_buf[16]
unsigned int *majorNum = (unsigned int *)&file_buf[17];
unsigned int *minorNum = (unsigned int *)&file_buf[21];

readBuf mime;   // 20
readBuf title;	// 256
readBuf fspec;	// 25
readBuf sha1sum; // 24
readBuf part;

#define ISIZE 4
#define LSIZE 8
#define VSIZE 8

// size_t readHTTPContents

size_t readContentLength(char *ptr, size_t expected) {
	char *tmpPtr;
	if (!strcmp(ptr,"\r\n")) hasHeader = (hasHeader == 1)?2:0;
	if ((tmpPtr = strchr(ptr,':')) != NULL) {
		*tmpPtr++ = 0;
		tmpPtr[strlen(tmpPtr)-2] = 0; // \r\n
		while (*tmpPtr == ' ') tmpPtr++;
		if (!strcmp(ptr,"Content-Length")) contentLength = atol(tmpPtr);
		}
	return expected;
	}

char initARIBuffer(void) {
	mime.buf = mime_buf;
	mime.limit = 20;
	title.current = mime.current = mime.offset = 0;
	fspec.current = 0;
	fspec.offset = 20;
	fspec.limit = 25;
	fspec.buf = file_buf;
	title.buf = title_buf;
	title.limit = 256;
	title.offset = 45; // 20-byte header, 25-byte file descriptor, 256-byte title... e-records... sha1sum
	part.current = 0; part.buf = (char *)&iE; part.limit = sizeof(imageEntry);
	return (hasHeader)?1:0;
	}

unsigned long archiveBytesRead;
unsigned long uncompressedRead;

void initFspec(void) {
	fspec.buf = file_buf;
	fspec.limit = 25;
	fspec.current = 0;
	fspec.offset = 20;
	sha1sum.buf = title_buf;
	sha1sum.limit = 24;
	sha1sum.current = 24;
	sha1sum.offset = 0; // multi-part offset indicates the expected filesize from the filepointer
	archiveBytesRead = uncompressedRead = 0;
	}

size_t readHTTPType(char *ptr, size_t size, size_t nmemb, void *userdata) {
	// static char mime[12];
	// static char title[256];
	int partitionRecords;
	static int readTables;
	if (hasHeader == 1 || hasHeader == -1) return readContentLength(ptr,size*nmemb); // get Content-Length
        if (!totalRead) readTables = initARIBuffer(); // initialize .ari buffer
	if (addToBuffer(ptr, size * nmemb, &mime) == 1) { // 20
		if (memcmp(mime.buf,VERSTRING,VSIZE)) return -1; // not an .ari file
		if (*((int *)&mime.buf[VSIZE])) { httpResult = HTTP_INTERRUPT; return -1; } // not the primary .ari file
		if (addToBuffer(ptr,size*nmemb,&fspec) == 1) { // size of file
			if (readTables && (*fileSize > (contentLength - 45 - 24))) { debug(INFO,5,"Archive index exceeds archive segment.\n"); return -1; } // very rare; don't bother coding for it
			if ((*fileSize - 256) % sizeof(imageEntry)) { debug(INFO,5,"Archive index does not satisfy boundary condition.\n"); return -1; }
			partitionRecords = (*fileSize - 256)/sizeof(imageEntry);
			if (addToBuffer(ptr,size*nmemb,&title) == 1) { // read the title
				title.buf[255] = 0;
				httpResult = HTTP_OK;
				if (!readTables) return -1;
				debug(INFO,5,"Reading %i records\n",partitionRecords);
				while(readTables <= partitionRecords) {
					part.offset = 256+45+part.limit*(readTables-1);
					if (addToBuffer(ptr,size*nmemb,&part) == 1) {
						part.current = 0;
						if (!iE.label[IMAGELABEL-1]) addImageEntry(&iE);
						else { debug(INFO,0,"Archive index error %i\n",readTables); return -1; }
						readTables++;
						}
					else { totalRead += size*nmemb; return size*nmemb; }
					}
				return -1;
				}
			}
		}
	totalRead += size*nmemb;
	return size * nmemb;
	}

size_t readHTTPRange(char *ptr, size_t size, size_t nmemb, void *userdata) {
	unsigned int cread = totalCharsRead + size * nmemb;
	if (cread > maxCharsAllowed) { debug(INFO,5,"HTTP RANGE ERROR %i %i %i\n",maxCharsAllowed,totalCharsRead,size*nmemb); return 0; }
	if (has_interrupted) return 0;
	memcpy(&currentDownloadBuffer[totalCharsRead],ptr,size*nmemb);
	totalCharsRead = cread;
	return size * nmemb;
	}

/*

unsigned long currentSize;
unsigned long archFileSize;
unsigned int archMajor;
unsigned int archMinor;
*/

char addToMultiBuffer(char *ptr, size_t bufsize, readBuf *rb) {
	size_t remaining = rb->limit - rb->current;
	if (!remaining) return 1; // already matched
	if (remaining > bufsize) { // only a partial match
		memcpy(&rb->buf[rb->current],ptr,bufsize);
		rb->current += bufsize;
		return 0; // not yet done
		}
	memcpy(&rb->buf[rb->current],ptr,remaining);
	rb->current = rb->limit;
	return 1;
	}

/*
size_t readEachHTTPFile(char *ptr, size_t size, size_t nmemb, void *userdata) {
	size_t bufsize = size * nmemb;
	size_t offset = 0;
	size_t lastCurrent;
	if (!bufsize) return 0; // nothing to read

	if (hasHeader == 1 || hasHeader == -1) return readContentLength(ptr,size*nmemb); // get Content-Length
	if (!totalRead) { mime.buf = mime_buf; mime.limit = 20; mime.current = 0; } // init mime stuff; file buf initialized elsewhere...
	lastCurrent = sha1sum.current;
	offset = sha1sum.offset - archiveBytesRead;
	if (totalRead < 20) {
		if (addToBuffer(ptr,bufsize,&mime) == 1) {
			if (memcmp(mime.buf,VERSTRING,VSIZE)) return -1; // not an .ari file
			// check signature, etc.
			offset = 20-totalRead;
			totalRead += offset;
debug(INFO,5,"Read mime %i [%lu]\n",offset,totalRead);
			if (readEachHTTPFile(&ptr[offset],bufsize-offset,1,NULL) != -1) { return bufsize; }
			return -1;
			}
		totalRead += bufsize;
debug(INFO,5,"Small [%lu]\n",totalRead);
		return bufsize;
		}
	else if (sha1sum.current == 24) { // read the next filepointer
		lastCurrent = fspec.current;
		if ((contentLength - totalRead) < L2SIZE) {  totalRead += bufsize; return bufsize; } // check to make sure we can multi-read the file
		if (addToMultiBuffer(ptr,bufsize,&fspec) == 1) {
			sha1sum.current = 0;
			sha1sum.offset = *fileSize;
			archiveBytesRead = 0;
			uncompressedRead = 0;
			offset = fspec.limit - lastCurrent;
			totalRead += offset;
debug(INFO,5,"Read file spec %i [%lu]\n",offset,totalRead);
			if (readEachHTTPFile(&ptr[offset],bufsize-offset,1,NULL) != -1) { return bufsize; }
			return -1;
			}
		}
	else if (offset) { // keep processing file
		if (offset >= bufsize) {
			debug(INFO,5,"BSIZE [%i, %i]\n",offset,bufsize);
			archiveBytesRead += bufsize; } // just process bufsize
		else {
			// process offset
			archiveBytesRead += offset;
			totalRead += offset;
debug(INFO,5,"Done reading file %i [%lu]\n",offset,totalRead);
			if (readEachHTTPFile(&ptr[offset],bufsize-offset,1,NULL) != -1) { return bufsize; }
			return -1;
			}
		}
	else if (addToMultiBuffer(ptr,bufsize,&sha1sum) == 1) {
		offset = sha1sum.limit - lastCurrent;
		totalRead += offset;
debug(INFO,5,"Done reading SHA1SUM %i [%lu]\n",offset,totalRead);
		if (readEachHTTPFile(&ptr[offset],bufsize-offset,1,NULL) != -1) { return bufsize; }
		return -1;
		}
	totalRead += bufsize;
debug(INFO,5,"Standard %i [%lu]\n",bufsize,totalRead);
	return bufsize;
	}
*/

size_t readHTTPChecksum(char *ptr, size_t size, size_t nmemb, void *userdata) {
	unsigned int n = size *nmemb;
	// make sure this is less than UINT_MAX <limits.h> (should be mostly ok); doubt curl will buffer too much
	if (n) sha1Update(ptr,n);
	totalWrite += n;
	if (progressBar(totalWrite, totalWrite, PROGRESS_UPDATE)) return 0;
	return n;
	}

size_t headerFunc(char *ptr, size_t size, size_t nmemb, void *userdata) {
        size_t totalSize = size * nmemb;
        char *tmpPtr;
        char *resCode;
        if (!totalSize || ptr[totalSize]) return totalSize; // not null-terminated
        int len = strlen(ptr);
        if (ptr[0] == '\r') return totalSize;
        if ((tmpPtr = strchr(ptr,':')) != NULL) { // see if there's a re-direct location (could also have libcurl handle this directly)
		if (httpResult != HTTP_PERMREDIR && httpResult != HTTP_TEMPREDIR) return totalSize; // only handle permanent/temporary redirects
		*tmpPtr++ = 0;
                tmpPtr[strlen(tmpPtr)-2] = 0;  // \r\n
                while(*tmpPtr == ' ') tmpPtr++;
                if (strcmp(ptr,"Location")) return totalSize;
                if (strlen(tmpPtr) >= BUFSIZE) return totalSize;
                strcpy(globalBuf,tmpPtr); // redirect this
                return totalSize;
                }
        if ((len < 5) || strncmp(ptr,"HTTP",4)) return totalSize;
        if ((resCode = strchr(ptr,' ')) == NULL) return totalSize;
        if ((tmpPtr = strchr(++resCode,' ')) == NULL) return totalSize;
        *tmpPtr = 0;
	switch(atoi(resCode)) { // don't really need this since we can use CURLINFO_RESPONSE_CODE, but let's keep it for the Location check above
		case 200: httpResult = HTTP_OK; break;
		case 301: httpResult = HTTP_PERMREDIR; break;
		case 307: httpResult = HTTP_TEMPREDIR; break;
		default: httpResult = 0; break;
		}
        return totalSize;
        }

void *performCurl(void *ptr) {
	CURLcode res;
	long responseCode;
	res = curl_easy_perform(curl);
	if (res != CURLE_OK && (ptr != NULL)) { debug(INFO,5,"CURL error: %s\n",curl_easy_strerror(res)); httpResult = 0; }
	if (res == CURLE_OK) {
		curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&responseCode);
		if (!httpResult && responseCode == 206) httpResult = HTTP_RANGE_OK;
		}
	}

void getHeaderResponse(char *url, char curlFunc) {
	int pid;
	sigset_t set; sigemptyset(&set); sigaddset(&set,SIGINT);
	if (curl == NULL) curl = curl_easy_init();
        if (!curl) return;
        curl_easy_reset(curl);
        curl_easy_setopt(curl,CURLOPT_URL,url);
	hasHeader = 0;
	totalRead = 0;
	struct curl_slist *slist = NULL;
	char tmpBuf[64];
	if (!curlFunc) { // see if URL exists ONLY (HEAD command does not include content-length)
        	curl_easy_setopt(curl,CURLOPT_HEADER,1);
        	curl_easy_setopt(curl,CURLOPT_NOBODY,1);
        	curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)headerFunc);
		}
	else if (curlFunc == 1) curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readHTTPListing); // reads directory contents
	else if (curlFunc == 2) curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readHTTPType); // reads first few bytes of file to determine type
	else if (curlFunc == 3) { // reads both content size as well as the index contents
		hasHeader = 1;
		curl_easy_setopt(curl,CURLOPT_HEADER,1);
		curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readHTTPType);
		}
	else if (curlFunc == 4) { // read content size ONLY
		hasHeader = -1;
		curl_easy_setopt(curl,CURLOPT_HEADER,1);
                curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readHTTPType);
		}
	else if (curlFunc == 5) curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readHTTPChecksum); // read entire file and add to sha1sum buffer (Ctrl-V)
	else if (curlFunc == 6) { // download a range of bytes from a file
		sprintf(tmpBuf,"Range: bytes=%lu-%lu",startRange,startRange+(unsigned long)maxCharsAllowed-1L);
		slist = curl_slist_append(slist,tmpBuf);
		curl_easy_setopt(curl,CURLOPT_HTTPHEADER,slist);
		curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readHTTPRange);
		}
/*
	else if (curlFunc == 6) { // UNUSED -- F10 Verify Image
		hasHeader = 1;
		curl_easy_setopt(curl,CURLOPT_HEADER,1);
		curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,(void *)readEachHTTPFile);
		}
*/
        httpResult = 0;
        startTimer(3);

        pthread_t tid;
        long status;
        has_interrupted = 0;
        pthread_create(&tid,NULL,performCurl,(curlFunc >= 2 && curlFunc <= 4)?NULL:&curlFunc);
	threadTID = tid;
	sigprocmask(SIG_BLOCK,&set,NULL); // block ctrl-c from main process
        pthread_join(tid,(void **)&status);
	sigprocmask(SIG_UNBLOCK,&set,NULL); // re-enable ctrl-c for main process
	if (slist != NULL) curl_slist_free_all(slist);
        if (has_interrupted) {
		httpResult = HTTP_INTERRUPT; curl_easy_cleanup(curl); curl = NULL;
		}
        threadTID = has_interrupted = 0;
        stopTimer();
        }

unsigned long statHTTPFile(char *url) { // find size of a single HTTP file
	contentLength = 0;
	getHeaderResponse(url,4);
	if (httpResult == HTTP_OK || httpResult == HTTP_INTERRUPT) return contentLength;
	return 0; // something wrong
	}

// an HTTP-centric open that keeps a larger read buffer
int openHTTPFile(char *url) { // don't need to determine if file exists since a subsequent HTTP read will error out
	int i;
	for (i=0;i<allocatedHTTPBuffers;i++) {
		if (!*httpBuf[i].currentURL) break;
		}
	if (i == allocatedHTTPBuffers) { // create some more
		if (i == MAXHTTPFILES) return -1;
		if ((httpBuf[i].httpBuffer = malloc(sizeof(char)*HTTPBUFSIZE)) == NULL) return -1;
		if ((httpBuf[i].currentURL = malloc(sizeof(char)*MAX_PATH)) == NULL) return -1;
		allocatedHTTPBuffers++;
		}
	strcpy(httpBuf[i].currentURL,url);
	httpBuf[i].currentHTTPOffset = 0;
	httpBuf[i].currentHTTPBufIndex = 0;
	httpBuf[i].currentHTTPBufLimit = 0;
	recentSeek = true;
	return i;
        }

void seekHTTPFile(int fd, unsigned int start) {
	httpBuf[fd].currentHTTPOffset += start - (httpBuf[fd].currentHTTPBufLimit - httpBuf[fd].currentHTTPBufIndex); // SEEK_CUR
        httpBuf[fd].currentHTTPBufIndex = 0;
        httpBuf[fd].currentHTTPBufLimit = 0;
	recentSeek = true;
	}

int continueHTTPRange(int fd, unsigned int start, unsigned int length) {
	currentDownloadBuffer = httpBuf[fd].httpBuffer;
	startRange = start;
	maxCharsAllowed = length;
	totalCharsRead = 0;
	getHeaderResponse(httpBuf[fd].currentURL,6);
	httpBytesRead += totalCharsRead;
	return (httpResult)?totalCharsRead:0; // should be HTTP_RANGE_OK; otherwise indicates EOF
	}

// read from an 'open' HTTP file, at a particular location
int readHTTPFile(int fd, unsigned char *buf, int size) {
	unsigned int available = httpBuf[fd].currentHTTPBufLimit - httpBuf[fd].currentHTTPBufIndex;
	int remaining = size;
	int totalBuffered = 0;
	unsigned char *downloadBuffer = httpBuf[fd].httpBuffer;
	unsigned int lastRange;
	while (remaining > available) { // return what's left, then read another buffer
		if (available) { // flush the buffer
			memcpy(buf,&(downloadBuffer[httpBuf[fd].currentHTTPBufIndex]),available); // flush the buffer
			remaining -= available;
			buf += available; // increment the pointer
			totalBuffered += available;
			}
		httpBuf[fd].currentHTTPBufIndex = 0;
		lastRange = (recentSeek)?301:HTTPBUFSIZE; // read archive beginning first to see if it's what we want
		recentSeek = false;
		httpBuf[fd].currentHTTPBufLimit = continueHTTPRange(fd,httpBuf[fd].currentHTTPOffset,lastRange);
		if (httpResult == HTTP_INTERRUPT) { has_interrupted = 1; return -1; }
		httpBuf[fd].currentHTTPOffset += httpBuf[fd].currentHTTPBufLimit; // what we've read from the HTTP file
		if (!httpBuf[fd].currentHTTPBufLimit) return totalBuffered; // EOF; return what we have left
		if (httpBuf[fd].currentHTTPBufLimit < lastRange) break; // nothing left to read
		available = httpBuf[fd].currentHTTPBufLimit;  // what's left in the buffer
		}
	if (remaining) {
		if (remaining > httpBuf[fd].currentHTTPBufLimit) remaining = httpBuf[fd].currentHTTPBufLimit;
		memcpy(buf,&(downloadBuffer[httpBuf[fd].currentHTTPBufIndex]),remaining);
		httpBuf[fd].currentHTTPBufIndex += remaining;
		totalBuffered += remaining;
		}
	return totalBuffered;
	}

void closeHTTPFile(int fd) {
	*httpBuf[fd].currentURL = 0;
	}

char *scanDirs(char *inUrl) {
        static char *div;
        char *url = globalBuf;
        char hasDiv = 0;
        if (inUrl != url) strcpy(url,inUrl);
        if (strncmp(url,"http://",7)) return NULL;
        if ((div = strrchr(&url[7],'/')) != NULL) {
                div++; // leave '/'
                // *div++ = 0;
		if (*div) {
                	memmove(&div[1],div,strlen(div)+1); // move to the right
			*div++ = 0;
			}
                hasDiv = 1;
                }
        else div = "";
//      printf("Mount point is: %s [%s]\n",url,div);
        getHeaderResponse(url,0);
        if (httpResult == HTTP_PERMREDIR || httpResult == HTTP_TEMPREDIR) {
                getHeaderResponse(url,0);
                }
        if (httpResult == HTTP_OK) return div; // URL OK
	else if (httpResult == HTTP_INTERRUPT) return NULL; // user interrupt
        else if (hasDiv) {
                url[strlen(url)-1] = 0; // remove trailing '/'
                return scanDirs(url);
                }
        else return NULL; // no valid result code
        }

void removeHTTPPoint(selection *sel) {
	int i;
	for (i=0;i<urlCount;i++) {
		if (!strcmp(url[i],sel->sources[sel->position].location)) {
			free(url[i]);
			if (urlCount-i-1) memmove(&url[i],&url[i+1],urlCount-i-1);
			urlCount--;
			break;
			}
		}
	}

int addHTTPPoint(selection *sel, char *cifs) {
	char *fileMatch;
	char hasDir = 0;
	char *urlEntry;
	int i;
	urlEntry = malloc(strlen(cifs)+8);
	if (urlEntry == NULL) return 0;
	strcpy(urlEntry,"http://");
	strcat(urlEntry,cifs);

	// check validity of URL here (HEAD request)
	strcpy(globalBuf,urlEntry);
	if (*globalBuf && globalBuf[strlen(globalBuf)-1] == '/') hasDir = 1;
	if (strchr(&globalBuf[7],'/') == NULL) strcat(globalBuf,"/"); // append '/' to scan
	fileMatch = scanDirs(globalBuf);
	*tempBuf = 0;
	if (fileMatch == NULL) { debug(INFO,5,"HTTP error. Last result: %i\n",httpResult); return 0; }
        else if (*fileMatch || hasDir) {
		if (strlen(fileMatch) <= MAX_TEMP) strcpy(tempBuf,fileMatch);
		hasDir = 1;
		// printf("Set mount point as [%s]\n",globalBuf);
		// printf("Navigate to mount point.\n");
                }
        else {
                // printf("Add mount point [%s] but do not navigate.\n",globalBuf);
                }
	if (*globalBuf && globalBuf[strlen(globalBuf)-1] == '/') globalBuf[strlen(globalBuf)-1] = 0; // get rid of trailing '/'
	// add entry to list
	if (sel == NULL) { // see if it's an ARI file
		if (getHTTPType(urlEntry) == 1) return 1;
		debug(EXIT,1,"Unable to find archive given by URL.\n");
		}
	for (i=0;i<sel->count;i++) {
		if (sel->sources[i].diskNum == -2 && !strcmp(globalBuf,sel->sources[i].location)) { sel->position = i; free(urlEntry); return 0; }
		}
	if (urlCount >= MAXURLS) { free(urlEntry); return 0; }
	if (strlen(globalBuf) > strlen(urlEntry)) {
		if ((urlEntry = realloc(urlEntry,strlen(globalBuf))) == NULL) return 0;
		}
	strcpy(urlEntry,globalBuf);
	url[urlCount++] = urlEntry;
	if (!sels[LIST_IMAGE].currentPathStart) populateList(&sels[LIST_IMAGE]);
        if (!sels[LIST_SOURCE].currentPathStart) populateList(&sels[LIST_SOURCE]);
        if (!sels[LIST_TARGET].currentPathStart) populateList(&sels[LIST_TARGET]);
        for (i=0;i<sel->count;i++) {
       		if (sel->sources[i].diskNum == -2 && !strcmp(urlEntry,sel->sources[i].location)) sel->position = i;
                }
	// getDirectoryListing(globalBuf, curl);
	if (hasDir) navigateDirectory(urlEntry,sel);
	if (*tempBuf) { // try to match (tempBuf) to a file/directory
		for (i=0;i<sel->count;i++) {
			if ((sel->sources[i].location != NULL) && !strcmp(sel->sources[i].location,tempBuf)) { sel->position = i; break; }
			}
		}
	populateImage(sel,0);
	return 1;
	}

char getHTTPType(char *path) {
	getHeaderResponse(path,2);
	if (httpResult == HTTP_OK) return 1; // the first .ari file
	else if (httpResult == HTTP_INTERRUPT) return 0; // not the first .ari file
	return -1; // not an .ari file
	}

void populateHTTPDirectory(selection *sel) {
	int i;
	char *path = sel->currentPath;
	int j = strlen(path);
	int res;
	httpSel = sel;
	bufIndex = 0;
        resetPool(&sel->pool,0);
        sel->count = sel->top = sel->position = 0;
	getHeaderResponse(sel->currentPath,1);
	for (i=0;i<sel->count;) {
		if (sel->sources[i].identifier == NULL) { i++; continue; }
		if ((strlen(sel->sources[i].location) + j) >= MAX_PATH) { i++; continue; }
		strcat(path,sel->sources[i].location);
		// debug(INFO,5,"Scan: %s\n",path); // read first few bytes to determine if it's an .ari file, then read getImageTitle
		res = getHTTPType(path); // read first few bytes to determine if it's an .ari file, then another few bytes to read the title, then another few bytes to read the contents(?)
				// then headers of all parts to get total file size...?
		path[j] = 0;
		if (res == -1) sel->sources[i].state = CLR_WDIM;
		else if (res) {
			sel->sources[i].state = CLR_GB;
			sel->sources[i].identifier = getStringPtr(&sel->pool,title_buf); // display title
			}
		else if (i < sel->count-1) { // remove this; it's a non-initial .ari file
			memmove(&sel->sources[i],&sel->sources[i+1],(sel->count-i-1)*sizeof(item));
			sel->count--;
			continue;
			}
		i++;
		// call populateImage(sel) when necessary
		}
	// populateImage(sel,0);
	}

void validateHTTPFiles(int segments, unsigned long imageSize) {
	int i;
	int startPath = strlen(globalPath);
	unsigned long currentSize = 0;
	totalWrite = 0;
	// start the process of sha1sum'ing the .ari fileset (ctrl-v)
	sha1Init();
	for (i=0;i<segments;i++) {
		setProgress(PROGRESS_VERIFY,NULL,globalPath,segments,i+1,0,NULL,imageSize,0,NULL);
		if (i) sprintf(&globalPath[startPath],".%i",i);
		else progressBar(imageSize,PROGRESS_BLUE,PROGRESS_INIT);
		debug(INFO,5,"Validating %s\n",globalPath);
		getHeaderResponse(globalPath,5);
		if (httpResult) {
			if (httpResult == HTTP_INTERRUPT) {
				has_interrupted = 1;
				progressBar(totalWrite, totalWrite, PROGRESS_UPDATE);
				feedbackComplete("*** CANCELLED ***");
				}
			else progressBar(0,totalWrite,PROGRESS_FAIL);
			return;
			}
		globalPath[startPath] = 0;
		}
	sha1Finalize();
        if (md_value != NULL && arch_md_len) {
                memcpy(sha1buf,md_value,20); // verify next
                for (i=0;i<20;i++) sprintf(&globalBuf[40+i*2],"%02x",md_value[i]);
                sprintf(&globalBuf[40+strlen(&globalBuf[40])]," %i segment%s",segments,(segments > 1)?"s":"");
                progressBar(0,totalWrite,PROGRESS_COMPLETE);
                }
	else progressBar(0,totalWrite,PROGRESS_FAIL);
	}

/*
int verifyImage(char *path) {
        archive arch;
        int n;
        unsigned long imageSize = 0; // total image size
        int startSegment, splitCount;
        if (!(splitCount = archiveSegments(path,&imageSize))) { debug(ABORT,0,"Archive damaged"); return -1; }
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
                if (n) { closeArchive(&arch); progressBar(0,arch.fileBytes,PROGRESS_FAIL); return -1; }
                progressBar(0,arch.fileBytes,PROGRESS_OK);
                }
        progressBar(imageSize,imageSize,PROGRESS_UPDATE | 1); // make 100%
        progressBar(0,arch.fileBytes,PROGRESS_COMPLETE);
        closeArchive(&arch);
        return 1;
        }
*/

/*
void verifyHTTPImage(int segments, unsigned long imageSize) {
	int i;
	archive arch;
	int startPath = strlen(globalPath);
	initFspec();
	arch.totalOffset = 0;
	arch.fileBytes = 0;
	unsigned long fileBytes;
	setProgress(PROGRESS_VALIDATE,NULL,globalPath,0,0,0,NULL,imageSize,0,&arch);
        progressBar(imageSize,PROGRESS_BLUE,PROGRESS_INIT | 1);
	for(i=0;i<segments;i++) {
		if (sha1sum.current == 24) { fspec.current = 0; fspec.offset = 20; } // wasn't enough space in the previous file
		if (i) sprintf(&globalPath[startPath],".%i",i);
debug(INFO,5,"Segment %s\n",globalPath);
		getHeaderResponse(globalPath,6);
		globalPath[startPath] = 0;
		}
	progressBar(imageSize,imageSize,PROGRESS_UPDATE | 1); // make 100%
	progressBar(0,fileBytes,PROGRESS_COMPLETE);
	}
*/

void httpFilesystemTest(unsigned long totalSize) {
	unsigned long fileSize = statHTTPFile(globalPath);
	endwin();
	printf("Size of: %s [%lu]\n",globalPath,totalSize);
	printf("Size of individual file: %lu\n",fileSize);



	printf("Done.\n");
	getch();
	exitConsole();
	}

#define MAXSEGMENTS 32768

char getHTTPInfo(char readIndex) {
	unsigned long timestamp;
	unsigned long totalSize = 0;
	int startPath = strlen(globalPath);
	int i = 0;
	while(1) {
		contentLength = 0;
		if (i) sprintf(&globalPath[startPath],".%i",i);
        	getHeaderResponse(globalPath,(readIndex == 1)?3:4);
		if (!contentLength || ((httpResult != HTTP_OK) && (httpResult != HTTP_INTERRUPT))) break; // empty segment signifies no further segments
		totalSize += contentLength;
		if (!i) {
			if (httpResult != HTTP_OK) { debug(INFO,5,"HTTP primary segment error\n"); return 0; } // bad call
			memcpy(&timestamp,&mime_buf[VSIZE+ISIZE],LSIZE);
			}
		else if (httpResult != HTTP_INTERRUPT) { debug(INFO,5,"HTTP secondary segment %i error [%i]\n",i,httpResult); return 0; } // bad call
		if (memcmp(&timestamp,&mime_buf[VSIZE+ISIZE],LSIZE)) break; // bad call
		if (i++ > MAXSEGMENTS) return 0; // too many; something's wrong
		}
	globalPath[startPath] = 0; // reset it
	if (!i) return 0; // empty first segment (shouldn't happen; some kind of error)
	if(!readIndex) {  // Ctrl-V
		validateHTTPFiles(i,totalSize);
		return i;
		}
/*
	else if (readIndex == 2) {  // F10
//		verifyHTTPImage(i,totalSize); // DEFUNCT?
		httpFilesystemTest(totalSize);
		return i;
		}
*/
	readableSize(totalSize);
	httpImageSize = totalSize;
	strcpy(imageSizeString,readable);
	for(i=0;i<4;i++) sprintf(&signature[i*2],"%02X",((unsigned char *)&timestamp)[i]);
	signature[8] = 0;
	return 2;
	}

void addNetworkPoint(selection *sel) {
	char cifs[MAX_FILE];
	char *fbuf = &cifs[2];
	char usrbuf[MAX_USER];
	char passbuf[MAX_PASS];
	WINDOW *win = wins[location(sel)];
	displaySelector(sel,0);
	*fbuf = 0;
	int xlen = -4, remoteType = 1, i;
	char *dblPath;
	remoteEntry = true;
	int selMap = getSelMap(sel);
	do {
		if (xlen == -4) remoteType++;
		if ((remoteType > 3) || (remoteType != 2 && (urlCount >= MAXURLS))) remoteType = 2;
		showCancel(remoteType);
		switch(remoteType) {
			case 2: xlen = showField(win,"cifs://"); break;
			default: xlen = showField(win,"http://"); break;
			}
	} while (((xlen = readLine(win,1,xlen,COLS-4-xlen,fbuf,NULL,MAX_FILE-3,NULL,false)) == -4) || (xlen == -5));
	remoteEntry = false;
	if (*fbuf) { // see what was entered; ask for user/pass accordingly
		while(((dblPath = strstr(fbuf,"//")) != NULL) || ((dblPath = strstr(fbuf,"/./")) != NULL)) memmove(dblPath,dblPath+1,strlen(dblPath)); // move to the left
		// while(fbuf[strlen(fbuf)-1] == '/') fbuf[strlen(fbuf)-1] = 0; // remove '/' character at the end

		if (
			((remoteType == 2) && ((dblPath = strchr(fbuf,'/')) == NULL)) ||
                    	((strlen(fbuf) >= 3) && (!strncmp(fbuf,"../",3) || !strncmp(&fbuf[strlen(fbuf)-3],"/..",3) || (strstr(fbuf,"/../") != NULL)))
			) { ; }  // invalid input
		else {
			showCancel(0);
			xlen = showField(win,"Username");
			*usrbuf = 0;
			if ((remoteType == 2) && readLine(win,1,xlen,COLS-4-xlen,usrbuf,NULL,MAX_USER-1,NULL,false) != -1) manageWindow(sel,CLEAR);
			else { // ask for password (usrbuf may be empty, but ask for password anyway)
				xlen = showField(win,"Password");
				*passbuf = 0;
				if ((remoteType == 2) && readLine(win,1,xlen,COLS-4-xlen,passbuf,NULL,MAX_PASS-1,NULL,true) != -1) manageWindow(sel,CLEAR);
				else { // process user/pass
					switch(remoteType) {
						case 2: // cifs
							addCIFSPoint(sel,selMap,cifs,dblPath,usrbuf,passbuf);
							break;
						default: // http
							addHTTPPoint(sel,&cifs[2]);
							break;
						}
					}
				}
			}
		}
	manageWindow(sel,CLEAR);
	showFunctionMenu(0);
	wrefresh(win);
	}
#endif
