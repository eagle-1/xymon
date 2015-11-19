/*----------------------------------------------------------------------------*/
/* Xymon monitor library.                                                     */
/*                                                                            */
/* Copyright (C) 2002-2011 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __STRFUNC_H__
#define __STRFUNC_H__

extern strbuffer_t *newstrbuffer(size_t initialsize);
extern strbuffer_t *convertstrbuffer(char *buffer, size_t bufsz);
extern void addtobuffer(strbuffer_t *buf, char *newtext);
extern void addtobuffer_many(strbuffer_t *buf, ...);
extern void addtostrbuffer(strbuffer_t *buf, strbuffer_t *newtext);
extern void addtobufferraw(strbuffer_t *buf, char *newdata, size_t bytes);
extern void strbuf_addtobuffer(strbuffer_t *buf, char *newtext, size_t newlen);
extern void clearstrbuffer(strbuffer_t *buf);
extern void freestrbuffer(strbuffer_t *buf);
extern char *grabstrbuffer(strbuffer_t *buf);
extern strbuffer_t *dupstrbuffer(char *src);
extern void strbufferchop(strbuffer_t *buf, int count);
extern void strbufferrecalc(strbuffer_t *buf);
extern void strbuffergrow(strbuffer_t *buf, size_t bytes);
extern void strbufferuse(strbuffer_t *buf, size_t bytes);
extern char *htmlquoted(char *s);
extern char *prehtmlquoted(char *s);
extern strbuffer_t *replacetext(char *original, char *oldtext, char *newtext);

#endif

