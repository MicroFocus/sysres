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

#include <string.h>
#include "partition.h"
#include "window.h"
#include <stdio.h>
#include <stdlib.h>
#include "sysres_debug.h"	// SYSRES_DEBUG_Debug(), SYSRES_DEBUG_level

extern long totalMem;
extern int mallocCount;

char pbuf[LINELIMIT];

void resetPool(poolset *sel, int size) { // size = 0, just reset it
	if (size) { sel->start = NULL; sel->size = size; }
	sel->pool = sel->start; sel->index = 0;
	}

char *getStringPtr(poolset *sel, char *str) {
	int len = strlen(str)+1;
	char *loc;
	if (sel->pool == NULL) { // create the first pool
		if ((sel->pool = sel->start = (strpool *)malloc(sizeof(strpool)+sel->size)) == NULL) debug(EXIT, 1,"Memory allocation error, string pool.\n");
		totalMem += sizeof(strpool)+sel->size; mallocCount++;
		sel->pool->next = NULL;
		sel->index = 0 ;
		}
	loc = &(sel->pool->stringpool[sel->index]);
	sel->index += len;
	if (sel->index > sel->size) {
		if (len > sel->size) debug(EXIT, 1,"String pool buffer overflow [%i/%i].\n",len,sel->size);
		if (sel->pool->next == NULL) {
			if ((sel->pool->next = (strpool *)malloc(sizeof(strpool)+sel->size)) == NULL) debug(EXIT, 1,"Memory allocation error, string pool.\n");
			totalMem += sizeof(strpool)+sel->size; mallocCount++;
			sel->pool->next->next = NULL;
			}
		sel->index = len;
		sel->pool = sel->pool->next;
		loc = sel->pool->stringpool;
		}
	strcpy(loc,str);
	return loc;
	}
