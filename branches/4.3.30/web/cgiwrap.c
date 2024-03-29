/*----------------------------------------------------------------------------*/
/* Xymon CGI wrapper tool                                                     */
/*                                                                            */
/* This is a wrapper for running the Xymon CGI programs                       */
/*                                                                            */
/* Copyright (C) 2014      Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: $";

#include <libgen.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#include "libxymon.h"

static char **options = NULL;
static int optcount = 0;
static int haveenvopt = 0;

void addopt(char *opt)
{
	options = (char **)realloc(options, (++optcount)*sizeof(char *));
	options[optcount-1] = strdup(opt);
	if (strncmp(opt, "--env=", 6) == 0) haveenvopt = 1;
}

void addoptl(char *ename)
{
	char *optlist, *opt;

	if (getenv(ename) == NULL) return;
	optlist = strdup(getenv(ename));
	opt = strtok(optlist, " \t");
	while (opt) {
		addopt(opt);
		opt = strtok(NULL, " \t");
	}
	free(optlist);
}

void addoptdone(void)
{
	options = (char **)realloc(options, (++optcount)*sizeof(char *));
	options[optcount-1] = NULL;
}
	

int main(int argc, char **argv)
{
	char *cgifile = XYMONHOME "/etc/cgioptions.cfg";
	char *pgm = strdup(argv[0]);
	SBUF_DEFINE(cgipgm);
	char executable[PATH_MAX];
	char xymoncmd[PATH_MAX];

	if (getenv("CGIDEBUG")) debug = 1;
	set_debugfile("stderr", 1);

	loadenv(cgifile, NULL);

	SBUF_MALLOC(cgipgm, strlen(pgm) + 5);
	strncpy(cgipgm, basename(pgm), cgipgm_buflen);
	if (strstr(cgipgm, ".sh")) { char *p = strstr(cgipgm, ".sh"); strncpy(p, ".cgi", (cgipgm_buflen - (p - cgipgm))); }

	if (strcmp(cgipgm, "cgiwrap") == 0) {
		printf("\ncgiwrap is an executable wrapper program for xymon CGI scripts. It sets the environment according to the configuration specified in %s and then launches the CGI to response to the request.\n\nCGI scripts in the xymon-cgi or xymon-seccgi directories should be symlinked or hardlinked to this file. cgiwrap will do nothing if called directly.\n", cgifile);
		return 1;
	}
	addopt("");	// Reserve one for the program name

	if      (strcmp(cgipgm, "ackinfo.cgi") == 0)             {                              addoptl("CGI_ACKINFO_OPTS");     }
	else if (strcmp(cgipgm, "acknowledge.cgi") == 0)         {                              addoptl("CGI_ACK_OPTS");         }
	else if (strcmp(cgipgm, "appfeed-critical.cgi") == 0)    { cgipgm = "appfeed.cgi";      addoptl("CGI_APPFEED_OPTS");     addopt("--critical=/etc/xymon/critical.cfg"); }
	else if (strcmp(cgipgm, "appfeed.cgi") == 0)             {                              addoptl("CGI_APPFEED_OPTS");     }
	else if (strcmp(cgipgm, "certreport.cgi") == 0)          { cgipgm = "statusreport.cgi";                                  addopt("--column=sslcert"); addopt("--filter=color=red,yellow"); addopt("--all"); addopt("--no-colors"); }
	else if (strcmp(cgipgm, "columndoc.cgi") == 0)           { cgipgm = "csvinfo.cgi";      addoptl("CGI_COLUMNDOC_OPTS");   if (getenv("QUERY_STRING")) { SBUF_DEFINE(t); SBUF_MALLOC(t, strlen(getenv("QUERY_STRING")) + 35); snprintf(t, t_buflen, "QUERY_STRING=db=columndoc.csv&key=%s", getenv("QUERY_STRING")); putenv(t); } }
	else if (strcmp(cgipgm, "confreport-critical.cgi") == 0) { cgipgm = "confreport.cgi";   addoptl("CGI_CONFREPORT_OPTS");  addopt("--critical"); }
	else if (strcmp(cgipgm, "confreport.cgi") == 0)          {                              addoptl("CGI_CONFREPORT_OPTS");  }
	else if (strcmp(cgipgm, "criticaleditor.cgi") == 0)      {                              addoptl("CGI_CRITEDIT_OPTS");    }
	else if (strcmp(cgipgm, "criticalview.cgi") == 0)        {                              addoptl("CGI_CRITVIEW_OPTS");    }
	else if (strcmp(cgipgm, "csvinfo.cgi") == 0)             {                              addoptl("CGI_CSVINFO_OPTS");     }
	else if (strcmp(cgipgm, "datepage.cgi") == 0)            {                              addoptl("CGI_DATEPAGE_OPTS");    }
	else if (strcmp(cgipgm, "enadis.cgi") == 0)              {                              addoptl("CGI_ENADIS_OPTS");      }
	else if (strcmp(cgipgm, "eventlog.cgi") == 0)            {                              addoptl("CGI_EVENTLOG_OPTS");    }
	else if (strcmp(cgipgm, "findhost.cgi") == 0)            {                              addoptl("CGI_FINDHOST_OPTS");    }
	else if (strcmp(cgipgm, "ghostlist.cgi") == 0)           {                              addoptl("CGI_GHOSTS_OPTS");      }
	else if (strcmp(cgipgm, "history.cgi") == 0)             {                              addoptl("CGI_HIST_OPTS");        }
	else if (strcmp(cgipgm, "historylog.cgi") == 0)          { cgipgm = "svcstatus.cgi";    addoptl("CGI_SVCHIST_OPTS");     addopt("--historical"); }
	else if (strcmp(cgipgm, "hostgraphs.cgi") == 0)          {                              addoptl("CGI_HOSTGRAPHS_OPTS");  }
	else if (strcmp(cgipgm, "hostlist.cgi") == 0)            {                              addoptl("CGI_HOSTLIST_OPTS");    }
	else if (strcmp(cgipgm, "nongreen.cgi") == 0)            { cgipgm = "statusreport.cgi";                                  addopt("--filter=\"color=red,yellow\""); addopt("--all"); addopt("--heading=\"All non-green systems\""); addopt("--show-column"); addopt("--show-summary"); addopt("--link"); }
	else if (strcmp(cgipgm, "notifications.cgi") == 0)       {                              addoptl("CGI_NOTIFYLOG_OPTS");   }
	else if (strcmp(cgipgm, "acknowledgements.cgi") == 0)    {                              addoptl("CGI_ACKNOWLEDGEMENTSLOG_OPTS");   }
	else if (strcmp(cgipgm, "perfdata.cgi") == 0)            {                              addoptl("CGI_PERFDATA_OPTS");    }
	else if (strcmp(cgipgm, "reportlog.cgi") == 0)           {                              addoptl("CGI_REPLOG_OPTS");      }
	else if (strcmp(cgipgm, "report.cgi") == 0)              {                              addoptl("CGI_REP_OPTS");         addoptl("XYMONGENREPOPTS"); }
	else if (strcmp(cgipgm, "showgraph.cgi") == 0)           {                              addoptl("CGI_SHOWGRAPH_OPTS");   }
	else if (strcmp(cgipgm, "snapshot.cgi") == 0)            {                              addoptl("CGI_SNAPSHOT_OPTS");    addopt("XYMONGENSNAPOPTS"); }
	else if (strcmp(cgipgm, "svcstatus-hist.cgi") == 0)      { cgipgm = "svcstatus.cgi";    addoptl("CGI_SVCHIST_OPTS");     addopt("--historical"); }
	else if (strcmp(cgipgm, "svcstatus.cgi") == 0)           {                              addoptl("CGI_SVC_OPTS");         }
	else if (strcmp(cgipgm, "svcstatus2.cgi") == 0)          { cgipgm = "svcstatus.cgi";    addoptl("CGI_SVC_OPTS");         }
	else if (strcmp(cgipgm, "topchanges.cgi") == 0)          { cgipgm = "eventlog.cgi";     addoptl("CGI_TOPCHANGE_OPTS");   }
	else if (strcmp(cgipgm, "useradm.cgi") == 0)             {                              addoptl("CGI_USERADM_OPTS");     }
	else if (strcmp(cgipgm, "chpasswd.cgi") == 0)            {                              addoptl("CGI_CHPASSWD_OPTS");    }
	else {
		/* Make sure we're being called as a CGI */
                if ((strlen(cgipgm) <= 4) || (strcmp(cgipgm+strlen(cgipgm)-4, ".cgi") != 0)) {
			fprintf(stderr, "ERROR: cgiwrap was called as an unexpected CGI name: %s\n", cgipgm);
			printf("Status: 403\nContent-type: text/plain\n\nUnexpected CGI name: %s\n", cgipgm);
			return 1;
		}
		dbgprintf("called as an unexpected CGI name: %s", cgipgm);
	}

	addoptdone();
	if (!haveenvopt && getenv("XYMONENV")) loadenv(getenv("XYMONENV"), NULL);
	snprintf(executable, sizeof(executable), "%s/bin/%s", xgetenv("XYMONHOME"), cgipgm);
	options[0] = executable;

	execv(executable, options);

	printf("Status: 500\nContent-type: text/plain\n\nExec failed for %s: %s\n", executable, strerror(errno));
	errprintf("Exec failed for %s: %s\n", executable, strerror(errno));
	return 1;
}

