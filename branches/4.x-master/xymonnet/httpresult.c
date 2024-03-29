/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* This is used to implement the testing of HTTP service.                     */
/*                                                                            */
/* Copyright (C) 2004-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id$";

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <regex.h>
#include <ctype.h>
#include <sys/stat.h>

#include "libxymon.h"

#include "xymonnet.h"
#include "contest.h"
#include "httpcookies.h"
#include "httpresult.h"

static int statuscolor(testedhost_t *h, int status)
{
	int result = COL_YELLOW;

	/* Default behavior is to treat HTTP codes as follows:
	 *     <100 = connection error (clear if dialup, otherwise red)
	 *	1xx = yellow (xymonnet will handle 100 Continues already, so if we're here then something went wrong)
	 *	2xx = green
	 *	3xx = yellow
	 *	4xx/5xx = red
	 * Exceptions are listed below.
	 * 
	 * TODO: Site-specific defaults; extending 'httpstatus' to be layered ontop of other http modifiers
	 */

	switch(status) {
	  case 200: /* OK - most common case */
	  case 302: /* Temp Redirect */
	  case 303: /* See Other */
	  case 307: /* Temp Redirect (HTTP 1.1) */
		result = COL_GREEN;
		break;
	  case 306: /* Defunct HTTP response */
		result = COL_RED;
		break;
	  case STATUS_CONTENTMATCH_FAILED:
		result = COL_RED;		/* Pseudo status: content match fails */
		break;
	  case STATUS_CONTENTMATCH_BADREGEX:	/* Pseudo status: bad regex to match against */
	  case STATUS_CONTENTMATCH_NOFILE:	/* Pseudo status: content match requested, but no match-file */
		result = COL_YELLOW;
		break;
	  case 000:				/* transport layer reports error */
		result = (h->dialup ? COL_CLEAR : COL_RED);
		break;
	  default:
		/* Unknown or custom status */
		result = (status < 100) ? (h->dialup ? COL_CLEAR : COL_RED) :
			 (status < 200) ? COL_YELLOW :
			 (status < 300) ? COL_GREEN  :
			 (status < 400) ? COL_YELLOW :
			 COL_RED;
		break;
	}

	return result;
}


static int statuscolor_by_set(testedhost_t *h, int status, char *okcodes, char *badcodes)
{
	int result = -1;
	char codestr[15];
	pcre *ptn;

	/* Use code 999 to indicate we could not fetch the URL */
	snprintf(codestr, sizeof(codestr), "%d", (status ? status : 999));

	if (okcodes) {
		ptn = compileregex(okcodes);
		if (matchregex(codestr, ptn)) result = COL_GREEN; else result = COL_RED;
		freeregex(ptn);
	}

	if (badcodes) {
		ptn = compileregex(badcodes);
		if (matchregex(codestr, ptn)) result = COL_RED; else result = COL_GREEN;
		freeregex(ptn);
	}

	if (result == -1) result = statuscolor(h, status);

	dbgprintf("Host %s status %s [%s:%s] -> color %s\n", 
		  h->hostname, codestr, 
		  (okcodes ? okcodes : "<null>"),
		  (badcodes ? badcodes : "<null>"),
		  colorname(result));

	return result;
}


void send_http_results(service_t *httptest, testedhost_t *host, testitem_t *firsttest,
		       char *nonetpage, int failgoesclear, int usebackfeedqueue, int dosavecookies)
{
	testitem_t *t;
	int	color = -1;
	char    *svcname;
	strbuffer_t *msgtext;
	SBUF_DEFINE(nopagename);
	int     nopage = 0;
	int	anydown = 0, totalreports = 0;
	char    *contsave;
	int 	contentsavenum = 0;
	contsave = (char *)malloc(128);

	if (firsttest == NULL) return;

	svcname = strdup(httptest->testname);
	if (httptest->namelen) svcname[httptest->namelen] = '\0';

	/* Check if this service is a NOPAGENET service. */
	SBUF_MALLOC(nopagename, strlen(svcname)+3);
	snprintf(nopagename, nopagename_buflen, ",%s,", svcname);
	nopage = (strstr(nonetpage, nopagename) != NULL);
	xfree(nopagename);

	dbgprintf("Calc http color host %s : ", host->hostname);

	msgtext = newstrbuffer(0);
	for (t=firsttest; (t && (t->host == host)); t = t->next) {
		http_data_t *req = (http_data_t *) t->privdata;

		/* Skip the data-reports and client-reports for now */
		if ((t->senddata == 1) || (t->sendclient == 1)) continue;

		/* Grab session cookies */
		if (dosavecookies) update_session_cookies(host->hostname, req->weburl.desturl->host, req->headers);

		totalreports++;
		if (req->weburl.okcodes || req->weburl.badcodes) {
			req->httpcolor = statuscolor_by_set(host, req->httpstatus, req->weburl.okcodes, req->weburl.badcodes);
		}
		else {
			req->httpcolor = statuscolor(host, req->httpstatus);
		}
		if (req->httpcolor == COL_RED) anydown++;

		/* Dialup hosts and dialup tests report red as clear */
		if ((req->httpcolor != COL_GREEN) && (host->dialup || t->dialup)) req->httpcolor = COL_CLEAR;

		/* If ping failed, report CLEAR unless alwaystrue */
		if ( ((req->httpcolor == COL_RED) || (req->httpcolor == COL_YELLOW)) && /* Test failed */
		     (host->downcount > 0)                   && /* The ping check did fail */
		     (!host->noping && !host->noconn)        && /* We are doing a ping test */
		     (failgoesclear)                         &&
		     (!t->alwaystrue)                           )  /* No "~testname" flag */ {
			req->httpcolor = COL_CLEAR;
		}

		/* If test we depend on has failed, report CLEAR unless alwaystrue */
		if ( ((req->httpcolor == COL_RED) || (req->httpcolor == COL_YELLOW)) && /* Test failed */
		      failgoesclear && !t->alwaystrue )  /* No "~testname" flag */ {
			char *faileddeps = deptest_failed(host, t->service->testname);

			if (faileddeps) {
				req->httpcolor = COL_CLEAR;
				req->faileddeps = strdup(faileddeps);
			}
		}

		dbgprintf("%s(%s) ", t->testspec, colorname(req->httpcolor));
		if (req->httpcolor > color) color = req->httpcolor;

		/* Build the short msgtext which goes on line 1 of the status message. */
		addtobuffer(msgtext, (STRBUFLEN(msgtext) ? " ; " : ": ") );
		if (req->tcptest->errcode != CONTEST_ENOERROR) {
			switch (req->tcptest->errcode) {
			  case CONTEST_ETIMEOUT: 
				  req->errorcause = "Server timeout"; break;
			  case CONTEST_ENOCONN : 
				  req->errorcause =  strdup(strerror(req->tcptest->connres)); break;
			  case CONTEST_EDNS    : 
				  switch (req->parsestatus) {
					  case 1 : req->errorcause =  "Invalid URL"; break;
					  case 2 : req->errorcause =  "Hostname not in DNS"; break;
					  default: req->errorcause =  "DNS error"; break;
				  }
				  break;
			  case CONTEST_EIO     : 
				  req->errorcause =  "I/O error"; break;
			  case CONTEST_ESSL    : 
				  req->errorcause =  "SSL error"; break;
			  default: 
				  req->errorcause =  "Xfer failed";
			}

			addtobuffer(msgtext, req->errorcause);
		} 
		else if (req->tcptest->open == 0) {
			req->errorcause = "Connect failed";
			addtobuffer(msgtext, req->errorcause);
		}
		else if ((req->httpcolor == COL_RED) || (req->httpcolor == COL_YELLOW)) {
			char m1[100];

			if (req->weburl.okcodes || req->weburl.badcodes) {
				snprintf(m1, sizeof(m1), "Unwanted HTTP status %d", req->httpstatus);
			}
			else if (req->headers) {
				char *p = req->headers;

				/* Skip past "HTTP/1.x 200 " and pick up the explanatory text, if any */
				if (strncasecmp(p, "http/", 5) == 0) {
					p += 5;
					p += strspn(p, "0123456789. ");
				}

				strncpy(m1, p, sizeof(m1)-1);
				m1[sizeof(m1)-1] = '\0';

				/* Only show the first line of the HTTP status description */
				p = m1 + strcspn(m1, "\n\r"); *p = '\0';
			}
			else {
				snprintf(m1, sizeof(m1), "Connected, but got empty response (code: %d)", req->httpstatus);
			}
			addtobuffer(msgtext, m1);
			req->errorcause = strdup(m1);
		}
		else {
			addtobuffer(msgtext, "OK");
			if (req->weburl.okcodes || req->weburl.badcodes) {
				char m1[100];

				snprintf(m1, sizeof(m1), " (HTTP status %d)", req->httpstatus);
				addtobuffer(msgtext, m1);
			}
		}
	}

	/* It could be that we have 0 http tests - if we only do the apache one */
	if (totalreports > 0) {
		char msgline[4096];

		if (anydown) {
			firsttest->downcount++; 
			if(firsttest->downcount == 1) firsttest->downstart = getcurrenttime(NULL);
		} 
		else firsttest->downcount = 0;

		/* Handle the "badtest" stuff for http tests */
		if ((color == COL_RED) && (firsttest->downcount < firsttest->badtest[2])) {
			if      (firsttest->downcount >= firsttest->badtest[1]) color = COL_YELLOW;
			else if (firsttest->downcount >= firsttest->badtest[0]) color = COL_CLEAR;
			else                                                    color = COL_GREEN;
		}

		if (nopage && (color == COL_RED)) color = COL_YELLOW;
		dbgprintf(" --> %s\n", colorname(color));

		/* Send off the http status report */
		init_status(color);
		snprintf(msgline, sizeof(msgline), "status+%d %s.%s %s %s", 
			validity, commafy(host->hostname), svcname, colorname(color), timestamp);
		addtostatus(msgline);
		addtostrstatus(msgtext);
		addtostatus("\n");

		for (t=firsttest; (t && (t->host == host)); t = t->next) {
			SBUF_DEFINE(urlmsg);
			http_data_t *req = (http_data_t *) t->privdata;

			/* Skip the "data" and "client" (status as client) reports */
			if ((t->senddata == 1) || (t->sendclient == 1)) continue;

			SBUF_MALLOC(urlmsg, 1024 + strlen(req->url));
			snprintf(urlmsg, urlmsg_buflen, "\n&%s %s - ", colorname(req->httpcolor), req->url);
			addtostatus(urlmsg);

			if (req->httpcolor == COL_GREEN) addtostatus("OK");
			else {
				if (req->errorcause) addtostatus(req->errorcause);
				else addtostatus("failed");
			}
			if (req->weburl.okcodes || req->weburl.badcodes) {
				char m1[100];

				snprintf(m1, sizeof(m1), " (HTTP status %d)", req->httpstatus);
				addtostatus(m1);
			}
			addtostatus("\n");

			if (req->headers) {
				addtostatus("\n");
				addtostatus(req->headers);
			}
			if (req->faileddeps) addtostatus(req->faileddeps);

			snprintf(urlmsg, urlmsg_buflen, "\nSeconds: %u.%.9ld\n\n", 
				(unsigned int)req->tcptest->totaltime.tv_sec, 
				req->tcptest->totaltime.tv_nsec);
			addtostatus(urlmsg);
			xfree(urlmsg);
		}
		addtostatus("\n\n");
		finish_status();
	}

	/* Send of any HTTP status tests in separate columns */
	for (t=firsttest; (t && (t->host == host)); t = t->next) {
		int color;
		char msgline[4096];
		SBUF_DEFINE(urlmsg);
		http_data_t *req = (http_data_t *) t->privdata;

		if ((t->senddata == 1) || (t->senddata == 2) || (t->sendclient == 1) || (t->sendclient == 2) || (!req->weburl.columnname) || (req->contentcheck != CONTENTCHECK_NONE)) continue;

		/* Handle the "badtest" stuff */
		color = req->httpcolor;
		if ((color == COL_RED) && (t->downcount < t->badtest[2])) {
			if      (t->downcount >= t->badtest[1]) color = COL_YELLOW;
			else if (t->downcount >= t->badtest[0]) color = COL_CLEAR;
			else                                    color = COL_GREEN;
		}

		if (nopage && (color == COL_RED)) color = COL_YELLOW;

		/* Send off the http status report */
		init_status(color);
		snprintf(msgline, sizeof(msgline), "status+%d %s.%s %s %s", 
			validity, commafy(host->hostname), req->weburl.columnname, colorname(color), timestamp);
		addtostatus(msgline);

		addtostatus(" : ");
		addtostatus(req->errorcause ? req->errorcause : "OK");
		if (req->weburl.okcodes || req->weburl.badcodes) {
			char m1[100];

			snprintf(m1, sizeof(m1), " (HTTP status %d)", req->httpstatus);
			addtostatus(m1);
		}
		addtostatus("\n");

		SBUF_MALLOC(urlmsg, 1024 + strlen(req->url));
		snprintf(urlmsg, urlmsg_buflen, "\n&%s %s - ", colorname(req->httpcolor), req->url);
		addtostatus(urlmsg);
		xfree(urlmsg);

		if (req->httpcolor == COL_GREEN) addtostatus("OK");
		else {
			if (req->errorcause) addtostatus(req->errorcause);
			else addtostatus("failed");
		}
		addtostatus("\n");

		if (req->headers) {
			addtostatus("\n");
			addtostatus(req->headers);
		}
		if (req->faileddeps) addtostatus(req->faileddeps);

		snprintf(msgline, sizeof(msgline), "\nSeconds: %u.%.9ld\n\n", 
			(unsigned int)req->tcptest->totaltime.tv_sec, 
			req->tcptest->totaltime.tv_nsec);
		addtostatus(msgline);

		addtostatus("\n\n");
		finish_status();
	}

	/* Send off any "data" messages now */
	for (t=firsttest; (t && (t->host == host)); t = t->next) {
		http_data_t *req;
		char *data = "";
		strbuffer_t *msg = newstrbuffer(0);
		char msgline[1024];

		if (!t->senddata) continue;

		req = (http_data_t *) t->privdata;
		if (req->output) data = req->output;
		if (req->weburl.columnname) {
			strcpy(contsave, req->weburl.columnname);
		}
		else {
			if (contentsavenum > 0) sprintf(contsave, "%s%d", "httpdata", contentsavenum);
			else strcpy(contsave, "httpdata");

			contentsavenum++;
		}

		snprintf(msgline, sizeof(msgline), "data %s.%s\n", commafy(host->hostname), contsave);
		addtobuffer(msg, msgline);
		addtobuffer(msg, data);
		combo_add(msg);

		freestrbuffer(msg);
	}

	/* Send off any "client" pseudo-status messages now */
	/* These leverage the old BB compatibility syntax to store the data portion of the HTTP result */
	/* as a client message section for the hostname in question */
	{
	 int hadclientrecords = 0;
	 static char msgline[4096];
	 static strbuffer_t *msg;
	 if (!msg) msg = newstrbuffer(0);

	 for (t=firsttest; (t && (t->host == host)); t = t->next) {
		http_data_t *req;

		if (!t->sendclient) continue;

		req = (http_data_t *) t->privdata;
		/* 
		 * We want to mirror regular client semantics here. A blank client message
		 * is not very useful as a forensic snapshot since it will overwrite the
		 * previous content. Therefore, only send section if at least *some* content.
		 */
		if (!req->output || !req->outlen) continue;

		if (req->weburl.columnname) {
			strcpy(contsave, req->weburl.columnname);
		}
		else {
			if (contentsavenum > 0) sprintf(contsave, "%s%d", "httpdata", contentsavenum);
			else strcpy(contsave, "httpdata");

			contentsavenum++;
		}

		if (!hadclientrecords++) {
			/* first time - clear previous message and add our header line */
			clearstrbuffer(msg);
			sprintf(msgline, "status %s.xymonnet client \n[date]\n%s\n", commafy(host->hostname), timestamp);
			addtobuffer(msg, msgline);
		}
		sprintf(msgline, "[%s]\n", contsave);
		addtobuffer(msg, msgline);
		addtobuffer(msg, req->output);
		addtobuffer(msg, "\n");
	 }
	 /* Send off the http data as a fake-status client report */
	 if (hadclientrecords) combo_add(msg);
	}

	xfree(svcname);
	xfree(contsave);
	freestrbuffer(msgtext);
}


static testitem_t *nextcontenttest(service_t *httptest, testedhost_t *host, testitem_t *current)
{
	testitem_t *result;

	result = current->next;

	if ((result == NULL) || (result->host != host)) {
		result = NULL;
	}

	return result;
}

void send_content_results(service_t *httptest, testedhost_t *host,
			  char *nonetpage, char *contenttestname, int failgoesclear)
{
	testitem_t *t, *firsttest;
	int	color = -1;
	SBUF_DEFINE(nopagename);
	int     nopage = 0;
	SBUF_DEFINE(conttest);
	int 	contentnum = 0;

	SBUF_MALLOC(conttest, 128);

	if (host->firsthttp == NULL) return;

	/* Check if this service is a NOPAGENET service. */
	SBUF_MALLOC(nopagename, strlen(contenttestname)+3);
	snprintf(nopagename, nopagename_buflen, ",%s,", contenttestname);
	nopage = (strstr(nonetpage, nopagename) != NULL);
	xfree(nopagename);

	dbgprintf("Calc content color host %s : ", host->hostname);

	firsttest = host->firsthttp;

	for (t=firsttest; (t && (t->host == host)); t = nextcontenttest(httptest, host, t)) {
		http_data_t *req = (http_data_t *) t->privdata;
		void *hinfo = hostinfo(host->hostname);
		int headermatch = (hinfo && xmh_item(hinfo, XMH_FLAG_HTTP_HEADER_MATCH));
		char cause[100];
		SBUF_DEFINE(msgline);
		int got_data = 1;

		/* Skip the "data"-only and client-only messages */
		if ((t->senddata == 1) || (t->senddata == 2) || (t->sendclient == 1) || (t->sendclient == 2)) continue;
		if (!req->contentcheck) continue;

		/* We have a content check */
		strncpy(cause, "Content OK", sizeof(cause));
		if (req->contstatus == 0) {
			/* The content check passed initial checks of regexp etc. */
			color = statuscolor(t->host, req->httpstatus);
			if (color == COL_GREEN) {
				/* We got the data from the server */
				regmatch_t foo[1];
				int status = 0;

				switch (req->contentcheck) {
				  case CONTENTCHECK_REGEX:
					if (headermatch && req->headers) {
						/* regex() returns 0 on success, REG_NOMATCH on failure */
						status = (regexec((regex_t *) req->exp, req->headers, 0, foo, 0) == 0) ? 0 : 
							    (req->output && (regexec((regex_t *) req->exp, req->output, 0, foo, 0) == 0)) ? 0 : 1;
						regfree((regex_t *) req->exp);
					}
					else if (req->output) {
						status = regexec((regex_t *) req->exp, req->output, 0, foo, 0);
						regfree((regex_t *) req->exp);
					}
					else {
						/* output may be null if we only got a redirect */
						status = STATUS_CONTENTMATCH_FAILED;
					}
					break;

				  case CONTENTCHECK_NOREGEX:
					if (headermatch && req->headers) {
						status = ( (!regexec((regex_t *) req->exp, req->headers, 0, foo, 0)) && 
							   (!req->output || (!regexec((regex_t *) req->exp, req->output, 0, foo, 0))) );
						regfree((regex_t *) req->exp);
					}
					else if (req->output) {
						status = (!regexec((regex_t *) req->exp, req->output, 0, foo, 0));
						regfree((regex_t *) req->exp);
					}
					else {
						/* output may be null if we only got a redirect */
						status = STATUS_CONTENTMATCH_FAILED;
					}
					break;

				  case CONTENTCHECK_DIGEST:
					if (req->digest == NULL) req->digest = strdup("");
					if (strcmp(req->digest, (char *)req->exp) != 0) {
						status = STATUS_CONTENTMATCH_FAILED;
					}
					else status = 0;

					req->output = (char *) malloc(strlen(req->digest)+strlen((char *)req->exp)+strlen("Expected:\nGot     :\n")+1);
					sprintf(req->output, "Expected:%s\nGot     :%s\n", 
						(char *)req->exp, req->digest);
					break;

				  case CONTENTCHECK_CONTENTTYPE:
					if (req->contenttype && (strcasecmp(req->contenttype, (char *)req->exp) == 0)) {
						status = 0;
					}
					else {
						status = STATUS_CONTENTMATCH_FAILED;
					}

					if (req->contenttype == NULL) req->contenttype = strdup("No content-type provdied");

					req->output = (char *) malloc(strlen(req->contenttype)+strlen((char *)req->exp)+strlen("Expected content-type: %s\nGot content-type     : %s\n")+1);
					sprintf(req->output, "Expected content-type: %s\nGot content-type     : %s\n",
						(char *)req->exp, req->contenttype);
					break;
				}

				req->contstatus = ((status == 0)  ? 200 : STATUS_CONTENTMATCH_FAILED);
				color = statuscolor(t->host, req->contstatus);
				if (color != COL_GREEN) strncpy(cause, "Content match failed", sizeof(cause));
			}
			else {
				/*
				 * Failed to retrieve the webpage.
				 * Report CLEAR, unless "alwaystrue" is set.
				 */
				if (failgoesclear && !t->alwaystrue) color = COL_CLEAR;
				got_data = 0;
				strncpy(cause, "Failed to get webpage", sizeof(cause));
			}

			if (nopage && (color == COL_RED)) color = COL_YELLOW;
		}
		else {
			/* This only happens upon internal errors in Xymon test system */
			color = statuscolor(t->host, req->contstatus);
			strncpy(cause, "Internal Xymon error", sizeof(cause));
		}

		/* Send the content status message */
		dbgprintf("Content check on %s is %s\n", req->url, colorname(color));

		if (req->weburl.columnname) {
			strncpy(conttest, req->weburl.columnname, conttest_buflen);
		}
		else {
			if (contentnum > 0) snprintf(conttest, conttest_buflen, "%s%d", contenttestname, contentnum);
			else strncpy(conttest, contenttestname, conttest_buflen);

			contentnum++;
		}

		SBUF_MALLOC(msgline, 4096 + (2 * strlen(req->url)));
		init_status(color);
		snprintf(msgline, msgline_buflen, "status+%d %s.%s %s %s: %s\n", 
			validity, commafy(host->hostname), conttest, colorname(color), timestamp, cause);
		addtostatus(msgline);

		if (!got_data) {
			if (host->hidehttp) {
				snprintf(msgline, msgline_buflen, "\nContent check failed\n");
			}
			else {
				snprintf(msgline, msgline_buflen, "\nAn error occurred while testing <a href=\"%s\">URL %s</a>\n", 
					req->url, req->url);
			}
		}
		else {
			if (host->hidehttp) {
				snprintf(msgline, msgline_buflen, "\n&%s Content check %s\n",
					colorname(color), ((color == COL_GREEN) ? "OK" : "Failed"));
			}
			else {
				snprintf(msgline, msgline_buflen, "\n&%s %s - Testing <a href=\"%s\">URL</a> yields:\n",
					colorname(color), req->url, req->url);
			}
		}
		addtostatus(msgline);
		xfree(msgline);
		addtostatus("\n");

		if (headermatch && req->headers) {
			addtostatus(req->headers);
			addtostatus("\n");
		}

		if (req->output == NULL) {
			addtostatus("\nNo body output received from server\n\n");
		}
		else if (!host->hidehttp) {
			/* Don't flood xymond with data */
			if (req->outlen > MAX_CONTENT_DATA) {
				*(req->output + MAX_CONTENT_DATA) = '\0';
				req->outlen = MAX_CONTENT_DATA;
			}

			if ( (req->contenttype && (strncasecmp(req->contenttype, "text/html", 9) == 0)) ||
			     (strncasecmp(req->output, "<html", 5) == 0) ) {
				char *bodystart = NULL;
				char *bodyend = NULL;

				bodystart = strstr(req->output, "<body");
				if (bodystart == NULL) bodystart = strstr(req->output, "<BODY");
				if (bodystart) {
					char *p;

					p = strchr(bodystart, '>');
					if (p) bodystart = (p+1);
				}
				else bodystart = req->output;

				bodyend = strstr(bodystart, "</body");
				if (bodyend == NULL) bodyend = strstr(bodystart, "</BODY");
				if (bodyend) {
					*bodyend = '\0';
				}

				addtostatus("<div>\n");
				addtostatus(bodystart);
				addtostatus("\n</div>\n");
			}
			else {
				addtostatus(req->output);
			}
		}

		addtostatus("\n\n");
		finish_status();
	}

	xfree(conttest);
}


void show_http_test_results(service_t *httptest)
{
	http_data_t *req;
	testitem_t *t;

	for (t = httptest->items; (t); t = t->next) {
		req = (http_data_t *) t->privdata;

		printf("URL                      : %s\n", req->url);
		printf("HTTP status              : %d\n", req->httpstatus);
		printf("HTTP headers\n%s\n", textornull(req->headers));
		printf("HTTP output\n%s\n", textornull(req->output));
		printf("------------------------------------------------------\n");
	}
}

