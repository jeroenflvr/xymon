/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* Copyright (C) 2002-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __STACKIO_H__
#define __STACKIO_H__

#define MAX_LINE_LEN 16384

extern FILE *stackfopen(char *filename, char *mode);
extern int stackfclose(FILE *fd);
extern char *stackfgets(char *buffer, unsigned int bufferlen, char *includetag1, char *includetag2);
extern char *unlimfgets(char **buffer, int *bufsz, FILE *fd);

#endif

