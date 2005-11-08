/*----------------------------------------------------------------------------*/
/* Hobbit CGI for generating the Hobbit NK page                               */
/*                                                                            */
/* Copyright (C) 2004-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: hobbit-nkview.c,v 1.2 2005-11-08 22:24:18 henrik Exp $";

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>

#include "libbbgen.h"

typedef struct nkconf_t {
	char *key;
	int priority;
	char *resolvergroup;
	char *ttgroup;
	char *ttextra;
} nkconf_t;
static RbtHandle rbconf;

typedef struct hstatus_t {
	char *hostname;
	char *testname;
	char *key;
	int color;
	time_t lastchange, logtime, validtime, acktime;
	nkconf_t *config;
} hstatus_t;

static RbtHandle rbstate;
static time_t oldlimit = 3600;


void errormsg(char *s)
{
	fprintf(stderr, "%s\n", s);
}


static int key_compare(void *a, void *b)
{
	return strcasecmp((char *)a, (char *)b);
}


void loadconfig(char *fn, char *wantclass)
{
	FILE *fd;
	char *inbuf = NULL;
	int inbufsz = 0;

	rbconf = rbtNew(key_compare);

	fd = stackfopen(fn, "r");
	if (fd == NULL) {
		errormsg("Cannot open configuration file\n");
		return;
	}

	while (stackfgets(&inbuf, &inbufsz, "include", NULL)) {
		/* Class  Host  service  TIME  Resolvergroup TTPrio TTGroup TTExtra */
		char *eclass, *ehost, *eservice, *etime, *rgroup, *ttgroup, *ttextra;
		int ttprio = 0;
		nkconf_t *newitem;
		RbtStatus status;

		eclass = gettok(inbuf, "|\n"); if (!eclass) continue;
		if (wantclass && eclass && (strcmp(eclass, wantclass) != 0)) continue;
		ehost = gettok(NULL, "|\n"); if (!ehost) continue;
		eservice = gettok(NULL, "|\n"); if (!eservice) continue;
		etime = gettok(NULL, "|\n"); if (!etime) continue;
		rgroup = gettok(NULL, "|\n");
		ttprio = atoi(gettok(NULL, "|\n"));
		ttgroup = gettok(NULL, "|\n");
		ttextra = gettok(NULL, "|\n");

		if ((ehost == NULL) || (eservice == NULL) || (ttprio == 0)) continue;
		if (etime && *etime && !within_sla(etime, 0)) continue;

		newitem = (nkconf_t *)malloc(sizeof(nkconf_t));
		newitem->key = (char *)malloc(strlen(ehost) + strlen(eservice) + 2);
		sprintf(newitem->key, "%s|%s", ehost, eservice);
		newitem->priority = ttprio;
		newitem->resolvergroup = strdup(urlencode(rgroup));
		newitem->ttgroup = strdup(urlencode(ttgroup));
		newitem->ttextra = strdup(urlencode(ttextra));

		status = rbtInsert(rbconf, newitem->key, newitem);
	}

	stackfclose(fd);
}

void loadstatus(int maxprio, time_t maxage, int mincolor)
{
	int hobbitdresult;
	char *board = NULL;
	char *bol, *eol;
	time_t now;

	hobbitdresult = sendmessage("hobbitdboard color=red,yellow fields=hostname,testname,color,lastchange,logtime,validtime,acktime", NULL, NULL, &board, 1, BBTALK_TIMEOUT);
	if (hobbitdresult != BB_OK) {
		errormsg("Unable to fetch current status\n");
		return;
	}

	now = getcurrenttime(NULL);
	rbstate = rbtNew(key_compare);

	bol = board;
	while (bol && (*bol)) {
		char *endkey;
		RbtStatus status;
		RbtIterator handle;

		eol = strchr(bol, '\n'); if (eol) *eol = '\0';

		/* Find the config entry */
		endkey = strchr(bol, '|'); if (endkey) endkey = strchr(endkey+1, '|'); 
		if (endkey) {
			*endkey = '\0';
			handle = rbtFind(rbconf, bol);
			*endkey = '|';

			if (handle != rbtEnd(rbconf)) {
				hstatus_t *newitem;
				void *k1, *k2;
				nkconf_t *cfg;

				rbtKeyValue(rbconf, handle, &k1, &k2);
				cfg = (nkconf_t *)k2;

				newitem = (hstatus_t *)malloc(sizeof(hstatus_t));
				newitem->config     = cfg;
				newitem->hostname   = gettok(bol, "|");
				newitem->testname   = gettok(NULL, "|");
				newitem->color      = parse_color(gettok(NULL, "|"));
				newitem->lastchange = atoi(gettok(NULL, "|"));
				newitem->logtime    = atoi(gettok(NULL, "|"));
				newitem->validtime  = atoi(gettok(NULL, "|"));
				newitem->acktime    = atoi(gettok(NULL, "|"));
				if ( (newitem->config->priority > maxprio) ||
				     ((now - newitem->lastchange) > maxage) ||
				     (newitem->color < mincolor) ) {
					xfree(newitem);
				}
				else {
					newitem->key        = (char *)malloc(strlen(newitem->hostname) + strlen(newitem->testname) + 2);
					sprintf(newitem->key, "%s|%s", newitem->hostname, newitem->testname);
					status = rbtInsert(rbstate, newitem->key, newitem);
				}
			}
		}

		bol = (eol ? (eol+1) : NULL);
	}
}


RbtHandle columnlist(RbtHandle statetree)
{
	RbtHandle rbcolumns;
	RbtIterator hhandle;

	rbcolumns = rbtNew(key_compare);
	for (hhandle = rbtBegin(statetree); (hhandle != rbtEnd(statetree)); hhandle = rbtNext(statetree, hhandle)) {
		void *k1, *k2;
		hstatus_t *itm;
		RbtStatus status;

	        rbtKeyValue(statetree, hhandle, &k1, &k2);
		itm = (hstatus_t *)k2;

		status = rbtInsert(rbcolumns, itm->testname, NULL);
	}

	return rbcolumns;
}

static char *nameandcomment(namelist_t *host)
{
	static char *result = NULL;
	char *cmt, *disp, *hname;

	if (result) xfree(result);

	hname = bbh_item(host, BBH_HOSTNAME);
	disp = bbh_item(host, BBH_DISPLAYNAME);
	cmt = bbh_item(host, BBH_COMMENT);
	if (disp == NULL) disp = hname;

	if (cmt) {
		result = (char *)malloc(strlen(disp) + strlen(cmt) + 4);
		sprintf(result, "%s (%s)", disp, cmt);
		return result;
	}
	else 
		return disp;
}

void print_colheaders(FILE *output, RbtHandle rbcolumns)
{
	int colcount;
	RbtIterator colhandle;

	colcount = 1;	/* Remember the hostname column */

	/* Group column headings */
	fprintf(output, "<TR>");
	fprintf(output, "<TD ROWSPAN=2>&nbsp;</TD>\n");	/* For the prio column - in both row headers+dash rows */
	fprintf(output, "<TD ROWSPAN=2>&nbsp;</TD>\n");	/* For the host column - in both row headers+dash rows */
	for (colhandle = rbtBegin(rbcolumns); (colhandle != rbtEnd(rbcolumns)); colhandle = rbtNext(rbcolumns, colhandle)) {
		void *k1, *k2;
		char *colname;

	        rbtKeyValue(rbcolumns, colhandle, &k1, &k2);
		colname = (char *)k1;
		colcount++;

		fprintf(output, " <TD ALIGN=CENTER VALIGN=BOTTOM WIDTH=45>\n");
		fprintf(output, " <A HREF=\"%s\"><FONT %s><B>%s</B></FONT></A> </TD>\n",
			columnlink(colname), xgetenv("MKBBCOLFONT"), colname);
	}
	fprintf(output, "</TR>\n");
	fprintf(output, "<TR><TD COLSPAN=%d><HR WIDTH=\"100%%\"></TD></TR>\n\n", colcount);
}

void print_hoststatus(FILE *output, hstatus_t *itm, RbtHandle columns, int prio, int firsthost)
{
	namelist_t *hinfo;
	char *dispname, *ip, *key;
	time_t now;
	RbtIterator colhandle;

	now = time(NULL);
	hinfo = hostinfo(itm->hostname);
	dispname = bbh_item(hinfo, BBH_DISPLAYNAME);
	ip = bbh_item(hinfo, BBH_IP);

	fprintf(output, "<TR>\n");

	/* Print the priority */
	fprintf(output, "<TD ALIGN=LEFT VALIGN=TOP WIDTH=25%%>");
	if (firsthost) 
		fprintf(output, "<FONT %s>PRIO %d</FONT>", xgetenv("MKBBROWFONT"), prio);
	else 
		fprintf(output, "&nbsp;");
	fprintf(output, "</TD>\n");

	/* Print the hostname with a link to the NK info page */
	fprintf(output, "<TD ALIGN=LEFT>");
	fprintf(output, "<A HREF=\"%s/bb-hostsvc.sh?HOSTSVC=%s.%s&amp;IP=%s&amp;DISPLAYNAME=%s\">",
		xgetenv("CGIBINURL"), commafy(itm->hostname), xgetenv("INFOCOLUMN"),
		ip, (dispname ? dispname : itm->hostname));
	fprintf(output, "<FONT %s>%s</FONT>", xgetenv("MKBBROWFONT"), nameandcomment(hinfo));
	fprintf(output, "</A>");
	fprintf(output, "</TD>\n");

	key = (char *)malloc(strlen(itm->hostname) + 1024);
	for (colhandle = rbtBegin(columns); (colhandle != rbtEnd(columns)); colhandle = rbtNext(columns, colhandle)) {
		void *k1, *k2;
		char *colname;
		RbtIterator sthandle;

		fprintf(output, "<TD ALIGN=CENTER>");

		rbtKeyValue(columns, colhandle, &k1, &k2);
		colname = (char *)k1;
		sprintf(key, "%s|%s", itm->hostname, colname);
		sthandle = rbtFind(rbstate, key);
		if (sthandle == rbtEnd(rbstate)) {
			fprintf(output, "-");
		}
		else {
			hstatus_t *column;
			char *htmlalttag;

			rbtKeyValue(rbstate, sthandle, &k1, &k2);
			column = (hstatus_t *)k2;
			if (column->config->priority != prio) 
				fprintf(output, "-");
			else {
				time_t age = now - column->lastchange;
				htmlalttag = alttag(colname, column->color, 0, 1, agestring(age));
				fprintf(output, "<A HREF=\"%s/bb-hostsvc.sh?HOSTSVC=%s.%s&amp;IP=%s&amp;DISPLAYNAME=%s&amp;NKPRIO=%d&amp;NKRESOLVER=%s&amp;NKTTGROUP=%s&amp;NKTTEXTRA=%s\">",
					xgetenv("CGIBINURL"), commafy(itm->hostname), colname,
					ip, (dispname ? dispname : itm->hostname),
					prio, 
					column->config->resolvergroup, 
					column->config->ttgroup,
					column->config->ttextra);
				fprintf(output, "<IMG SRC=\"%s/%s\" ALT=\"%s\" TITLE=\"%s\" HEIGHT=\"%s\" WIDTH=\"%s\" BORDER=0></A>",
					xgetenv("BBSKIN"), dotgiffilename(column->color, 0, (age > oldlimit)),
					htmlalttag, htmlalttag, 
					xgetenv("DOTHEIGHT"), xgetenv("DOTWIDTH"));
			}
		}

		fprintf(output, "</TD>\n");
	}
	xfree(key);

	fprintf(output, "</TR>\n");
}


void print_oneprio(FILE *output, RbtHandle rbstate, RbtHandle rbcolumns, int prio)
{
	RbtIterator hhandle;
	int firsthost = 1;
	char *curhost = "";

	/* Then output each host and their column status */
	for (hhandle = rbtBegin(rbstate); (hhandle != rbtEnd(rbstate)); hhandle = rbtNext(rbstate, hhandle)) {
		void *k1, *k2;
		hstatus_t *itm;

	        rbtKeyValue(rbstate, hhandle, &k1, &k2);
		itm = (hstatus_t *)k2;

		if (itm->config->priority != prio) continue;
		if (strcmp(curhost, itm->hostname) == 0) continue;

		/* New host */
		curhost = itm->hostname;
		print_hoststatus(output, itm, rbcolumns, prio, firsthost);
		firsthost = 0;
	}

	fprintf(output, "<TR><TD>&nbsp;</TD></TR>\n");
}



void generate_nkpage(FILE *output, char *hfprefix)
{
	RbtIterator hhandle;
	int color = COL_GREEN;
	int maxprio = 0;

	/* Determine background color and max. priority */
	for (hhandle = rbtBegin(rbstate); (hhandle != rbtEnd(rbstate)); hhandle = rbtNext(rbstate, hhandle)) {
		void *k1, *k2;
		hstatus_t *itm;
		RbtStatus status;

	        rbtKeyValue(rbstate, hhandle, &k1, &k2);
		itm = (hstatus_t *)k2;

		if (itm->color > color) color = itm->color;
		if (itm->config->priority > maxprio) maxprio = itm->config->priority;
	}

        headfoot(output, hfprefix, "", "header", color);
        fprintf(output, "<center>\n");

        if (color != COL_GREEN) {
		RbtHandle rbcolumns;
		int prio;

		rbcolumns = columnlist(rbstate);

		fprintf(output, "<TABLE BORDER=0 CELLPADDING=4>\n");
		print_colheaders(output, rbcolumns);

		for (prio = 1; (prio <= maxprio); prio++) {
			print_oneprio(output, rbstate, rbcolumns, prio);
		}

		fprintf(output, "</TABLE>\n");
		rbtDelete(rbcolumns);
        }
        else {
                /* "All Monitored Systems OK */
		fprintf(output, "<FONT SIZE=+2 FACE=\"Arial, Helvetica\"><BR><BR><I>All Monitored Systems OK</I></FONT><BR><BR>");
        }

        fprintf(output, "</center>\n");
        headfoot(output, hfprefix, "", "footer", color);
}


static int maxprio = 3;
static time_t maxage = INT_MAX;
static int mincolor = COL_YELLOW;

static void selectenv(char *name, char *val)
{
	char *env;
	char *p;

	env = (char *)malloc(strlen(name) + strlen(val) + 20);
	sprintf(env, "SELECT_%s_%s=SELECTED", name, val);
	for (p=env; (*p); p++) *p = toupper((int)*p);
	putenv(env);
}

static void parse_query(void)
{
	cgidata_t *cgidata = cgi_request();
	cgidata_t *cwalk;

	cwalk = cgidata;
	while (cwalk) {
		if (strcasecmp(cwalk->name, "MAXPRIO") == 0) {
			selectenv(cwalk->name, cwalk->value);
			maxprio = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "MAXAGE") == 0) {
			selectenv(cwalk->name, cwalk->value);
			maxage = 60*atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "MINCOLOR") == 0) {
			selectenv(cwalk->name, cwalk->value);
			mincolor = parse_color(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "OLDLIMIT") == 0) {
			selectenv(cwalk->name, cwalk->value);
			oldlimit = 60*atoi(cwalk->value);
		}

		cwalk = cwalk->next;
	}
}


int main(int argc, char *argv[])
{
	char configfn[PATH_MAX];

	parse_query();
	load_hostnames(xgetenv("BBHOSTS"), NULL, get_fqdn());
	sprintf(configfn, "%s/etc/hobbitnk.cfg", xgetenv("BBHOME"));
	loadconfig(configfn, NULL);
	load_all_links();
	loadstatus(maxprio, maxage, mincolor);
	use_recentgifs = 1;
	headfoot_unknowns = 0;

	fprintf(stdout, "Content-type: text/html\n\n");
	generate_nkpage(stdout, "hobbitnk");

	return 0;
}

