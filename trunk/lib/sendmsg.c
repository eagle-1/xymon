/*----------------------------------------------------------------------------*/
/* Xymon monitor library.                                                     */
/*                                                                            */
/* This is a library module, part of libxymon.                                */
/* It contains routines for communicating with the Xymon daemon               */
/*                                                                            */
/* Copyright (C) 2002-2011 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id$";

#include "config.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <stdio.h>

#include <limits.h>
#include <sys/resource.h>
#include <time.h>

#include <sys/ipc.h>
#include <sys/msg.h>

#include "libxymon.h"

#define SENDRETRIES 2

/* These commands go to all Xymon servers */
static char *multircptcmds[] = { "status", "combo", "data", "notify", "enable", "disable", "drop", "rename", "client", NULL };
static char errordetails[1024];

static strbuffer_t *msgbuf = NULL;	/* message buffer for one status message */
static int msgcolor;			/* color of status message in msgbuf */
static int combo_is_local = 0;
static strbuffer_t *xymonmsg = NULL;	/* Complete combo message buffer */

static int msgsincombo = 0;		/* # of messages queued in a combo */
static int maxmsgspercombo = 100;	/* 0 = no limit. 100 is a reasonable default. */
static int sleepbetweenmsgs = 0;

static int backfeedqueue = -1;
static int max_backfeedsz = 16384;


#define USERBUFSZ 4096

typedef struct myconn_t {
	/* Plain-text protocols */
	char szbuf[20]; char *szptr;
	char tlsbuf[10]; char *tlsptr;
	char *readbuf, *writebuf;
	char *readp, *writep;
	size_t readbufsz, lefttowrite;
	char *peer;
	int port, readmore, starttlspending;
	enum sslhandling_t usessl;
	sendresult_t result;
	sendreturn_t *response;
	struct myconn_t *next;
} myconn_t;

typedef struct mytarget_t {
	char *targetip;
	int defaultport;
	enum sslhandling_t usessl;
} mytarget_t;

static myconn_t *myhead = NULL, *mytail = NULL;

static enum conn_cbresult_t client_callback(tcpconn_t *connection, enum conn_callback_t id, void *userdata)
{
	enum conn_cbresult_t res = CONN_CBRESULT_OK;
	int n, sslhandshakeinprogress = 0;
	size_t used;
	time_t start, expire;
	myconn_t *rec = (myconn_t *)userdata;

	// dbgprintf("CB: %s\n", conn_callback_names[id]);

	switch (id) {
	  case CONN_CB_CONNECT_COMPLETE:       /* Client mode: New outbound connection succeded */
		rec->writep = rec->writebuf;
		rec->readbufsz = USERBUFSZ;
		rec->readbuf = rec->readp = malloc(rec->readbufsz);
		*(rec->readbuf) = '\0';
		rec->readmore = 1;
		break;

	  case CONN_CB_SSLHANDSHAKE_OK:        /* Client/server mode: SSL handshake completed OK (peer certificate ready) */
		rec->starttlspending = 0;
		rec->readp = rec->readbuf;
		*(rec->readbuf) = '\0';
		break;

	  case CONN_CB_READCHECK:              /* Client/server mode: Check if application wants to read data */
		res = (rec->readmore || rec->starttlspending) ? CONN_CBRESULT_OK : CONN_CBRESULT_FAILED;
		break;

	  case CONN_CB_READ:                   /* Client/server mode: Ready for application to read data w/ conn_read() */
		used = (rec->readp - rec->readbuf);
		if ((rec->readbufsz - used) < USERBUFSZ) {
			rec->readbufsz += USERBUFSZ;
			rec->readbuf = (char *)realloc(rec->readbuf, rec->readbufsz);
			rec->readp = rec->readbuf + used;
		}

		/*
		 * When doing an SSL handshake, CB_READ is invoked. Eventually there
		 * is enough data arriving so that *during* a CB READ, the callback
		 * is recursively invoked with CB_SSL_HANDSHAKE_OK. After that has
		 * completed, we return here from conn_read, and the status has
		 * now been changed to CONN_SSL_READY. We should NOT clear the readmore
		 * flag in this case, since it will happen before we have read
		 * any data. So we use a local flag variable to remember if we
		 * were doing a conn_read as part of the handshake.
		 */
		switch (connection->connstate) {
		  case CONN_SSL_ACCEPT_READ:
		  case CONN_SSL_ACCEPT_WRITE:
		  case CONN_SSL_CONNECT_READ:
		  case CONN_SSL_CONNECT_WRITE:
		  case CONN_SSL_STARTTLS_READ:
		  case CONN_SSL_STARTTLS_WRITE:
		  case CONN_SSL_READ:
		  case CONN_SSL_WRITE:
			sslhandshakeinprogress = 1; break;
		  default:
			sslhandshakeinprogress = 0; break;
		}

		n = conn_read(connection, rec->readp, (rec->readbufsz - used - 1));

		if ((n > 0) && rec->starttlspending) {
			*(rec->readp + n) = '\0';
			// dbgprintf("Read %d bytes while waiting for starttls OK: %s\n", n, rec->readp);
			if ((strncmp(rec->readp, "OK", 2) == 0) && strchr(rec->readp, '\n')) {
				rec->starttlspending = 0;
				return CONN_CBRESULT_STARTTLS;
			}
			else if ((strncmp(rec->readp, "ERR", 3) == 0) && strchr(rec->readp, '\n')) {
				errprintf("Server does not accept STARTTLS\n");
				conn_close_connection(connection, NULL);
				rec->result = XYMONSEND_ECONNFAILED;
				return CONN_CBRESULT_OK;
			}
			else {
				rec->readp += n;
			}
		}
		else if (n > 0) {
			// dbgprintf("Read %d bytes data\n", n);
			if (rec->response) {
				if (rec->response->respfd) {
					fwrite(rec->readp, n, 1, rec->response->respfd);
				}
				else {
					*(rec->readp + n) = '\0';
					addtobuffer(rec->response->respstr, rec->readp);
					if (!rec->response->fullresponse && strchr(rec->readp, '\n'))
						conn_close_connection(connection, NULL);
				}
			}
		}
		else if ((n == 0) && !sslhandshakeinprogress) {
			// dbgprintf("No more data\n");
			rec->readmore = 0;
			conn_close_connection(connection, "r");
		}
		break;

	  case CONN_CB_WRITECHECK:             /* Client/server mode: Check if application wants to write data */
		if (rec->starttlspending)
			res = CONN_CBRESULT_FAILED;
		else
			res = (rec->lefttowrite != 0) ? CONN_CBRESULT_OK : CONN_CBRESULT_FAILED;
		break;

	  case CONN_CB_WRITE:                  /* Client/server mode: Ready for application to write data w/ conn_write() */
		if (rec->tlsptr) {
			n = conn_write(connection, rec->tlsptr, strlen(rec->tlsptr));
			if (n > 0) {
				rec->tlsptr += n;
				if (*rec->tlsptr == '\0') {
					rec->tlsptr = NULL;
					rec->starttlspending = 1;
					// dbgprintf("Switching to STARTTLS pending\n");
				}
				n = 0;	/* Didn't send any message data yet */
			}
		}
		else if (rec->szptr) {
			n = conn_write(connection, rec->szptr, strlen(rec->szptr));
			// dbgprintf("Sent %d bytes of size command\n", n);
			if (n > 0) {
				rec->szptr += n;
				if (*rec->szptr == '\0') rec->szptr = NULL;
				n = 0;	/* Didn't send any message data yet */
			}
		}
		else {
			n = conn_write(connection, rec->writep, rec->lefttowrite);
			// dbgprintf("Sent %d bytes of data\n", n);
			if (n > 0) {
				rec->writep += n;
				rec->lefttowrite -= n;
				if (rec->lefttowrite == 0) {
					conn_close_connection(connection, "w");
				}
			}
		}
		break;

	  case CONN_CB_TIMEOUT:
		conn_close_connection(connection, NULL);
		rec->result = XYMONSEND_ETIMEOUT;
		break;

	  case CONN_CB_CLOSED:                 /* Client/server mode: Connection has been closed */
		break;

	  case CONN_CB_CLEANUP:                /* Client/server mode: Connection cleanup */
		connection->userdata = NULL;
		break;

	  case CONN_CB_SSLHANDSHAKE_FAILED:
	  case CONN_CB_CONNECT_FAILED:
		rec->result = XYMONSEND_ECONNFAILED;
		break;

	  default:
		break;
	}

	return res;
}


static int sendtoall(char *msg, int timeout, mytarget_t **targets, sendreturn_t *responsebuffer)
{
	int compressit = 0;
	myconn_t *myconn;
	int i, msglen;
	int maxfd;
	strbuffer_t *cbuf = NULL;
	int v4server = (getenv("XYMONV4SERVER") != NULL);

	conn_init_client();

	if (v4server) compressit = 0;

	msglen = strlen(msg);
	if (compressit) {
		cbuf = compress_buffer(msg, msglen);
		if (cbuf) {
			msg = STRBUF(cbuf);
			msglen = STRBUFLEN(cbuf);
		}
	}

	for (i = 0; (targets[i]); i++) {
		char *ip;
		int portnum;

		ip = conn_lookup_ip(targets[i]->targetip, &portnum);

		myconn = (myconn_t *)calloc(1, sizeof(myconn_t));
		strcpy(myconn->tlsbuf, "starttls\n");
		myconn->tlsptr = (targets[i]->usessl ? myconn->tlsbuf : NULL);
		myconn->usessl = targets[i]->usessl;
		myconn->lefttowrite = msglen;
		sprintf(myconn->szbuf, "size:%d\n", (int)myconn->lefttowrite);
		myconn->szptr = v4server ? NULL : myconn->szbuf;
		myconn->writebuf = msg;
		myconn->peer = strdup(ip ? ip : "");
		myconn->port = (portnum ? portnum : targets[i]->defaultport);
		if (mytail) {
			mytail->next = myconn;
			mytail = myconn;
		}
		else {
			myhead = mytail = myconn;
		}

		if (ip) {
			conn_prepare_connection(myconn->peer, myconn->port, CONN_SOCKTYPE_STREAM, NULL, 
					(myconn->usessl ? CONN_SSL_STARTTLS_CLIENT : CONN_SSL_NO), NULL, getenv("XYMONCLIENTCERT"), getenv("XYMONCLIENTKEY"), 
					1000000*timeout, client_callback, myconn);
		}
		else {
			myconn->result = XYMONSEND_EBADIP;
		}
	}

	myhead->response = responsebuffer;	/* Only want the response from the first server returned to the user */

	/* Loop to process data */
	do {
		fd_set fdread, fdwrite;
		int n;
		struct timeval tmo;

		maxfd = conn_fdset(&fdread, &fdwrite);

		if (maxfd > 0) {
			if (timeout) { tmo.tv_sec = 1; tmo.tv_usec = 0; }

			n = select(maxfd+1, &fdread, &fdwrite, NULL, (timeout ? &tmo : NULL));
			if (n < 0) {
				if (errno != EINTR) {
					if (cbuf) freestrbuffer(cbuf);
					return 1;
				}
			}

			conn_process_active(&fdread, &fdwrite);
		}

		conn_trimactive();
	} while (conn_active() && (maxfd > 0));

	if (cbuf) freestrbuffer(cbuf);

	return 0;
}

static mytarget_t **build_targetlist(char *recips, int defaultport, int defaultsslport)
{
	/*
	 * Target format: [ssl:][IP|hostname][/portnumber|/servicename|:portnumber|:servicename]
	 */
	mytarget_t **targets;
	char *multilist, *r, *p;
	int tcount = 0;

	multilist = strdup(recips);

	targets = (mytarget_t **)malloc(sizeof(mytarget_t *));
	r = strtok(multilist, " ,");
	while (r) {
		targets = (mytarget_t **)realloc(targets, (tcount+2)*sizeof(mytarget_t *));
		targets[tcount] = (mytarget_t *)calloc(1, sizeof(mytarget_t));
		if (strncmp(r, "ssl:", 4) == 0) {
			r += 4;
			targets[tcount]->usessl = CONN_SSL_STARTTLS_CLIENT;
		}
		else {
			targets[tcount]->usessl = CONN_SSL_NO;
		}
		p = strchr(r, '/');
		if (p) {
			targets[tcount]->defaultport = atoi(p+1);
			*p = '\0';
		}
		else {
			targets[tcount]->defaultport = defaultport;
		}

		targets[tcount]->targetip = strdup(r);
		tcount++;
		r = strtok(NULL, " ,");
	}
	targets[tcount] = NULL;
	xfree(multilist);

	return targets;
}

int sendmessage_init_local(void)
{
	backfeedqueue = setup_feedback_queue(CHAN_CLIENT);
	if (backfeedqueue == -1) return -1;

	max_backfeedsz = 1024*shbufsz(C_FEEDBACK_QUEUE)-1;
	return max_backfeedsz;
}

void sendmessage_finish_local(void)
{
	close_feedback_queue(backfeedqueue, CHAN_CLIENT);
}

sendresult_t sendmessage_local(char *msg)
{
	int n, done = 0;
	msglen_t msglen;

	if (backfeedqueue == -1) {
		return sendmessage(msg, NULL, XYMON_TIMEOUT, NULL);
	}

	/* Make sure we dont overflow the message buffer */
	msglen = strlen(msg);
	if (msglen > max_backfeedsz) {
		errprintf("Truncating backfeed channel message from %d to %d\n", msglen, max_backfeedsz);
		*(msg+max_backfeedsz) = '\0';
		msglen = max_backfeedsz;
	}

	/* This will block if queue is full, but that is OK */
	do {
		n = msgsnd(backfeedqueue, msg, msglen+1, 0);
		if ((n == 0) || ((n == -1) && (errno != EINTR))) done = 1;
	} while (!done);

	if (n == -1) {
		errprintf("Sending via backfeed channel failed: %s\n", strerror(errno));
		return XYMONSEND_ECONNFAILED;
	}

	return XYMONSEND_OK;
}


/* TODO: http targets, http proxy */
sendresult_t sendmessage(char *msg, char *recipient, int timeout, sendreturn_t *response)
{
	static mytarget_t **defaulttargets = NULL;
	static int defaultport = 0;
	static int defaultsslport = 0;
	mytarget_t **targets;
	int i;
	myconn_t *walk;
	sendresult_t res;

	if (dontsendmessages) {
		fprintf(stdout, "%s\n", msg);
		fflush(stdout);
		return XYMONSEND_OK;
	}

	if (defaultport == 0) {
		if (xgetenv("XYMONDPORT")) defaultport = atoi(xgetenv("XYMONDPORT"));
		if (defaultport == 0) defaultport = conn_lookup_portnumber("bb", 1984);
	}

	if (defaultsslport == 0) {
		if (xgetenv("XYMONDSSLPORT")) defaultsslport = atoi(xgetenv("XYMONDSSLPORT"));
		if (defaultsslport == 0) defaultsslport = conn_lookup_portnumber("bbssl", 1985);
	}

	if (defaulttargets == NULL) {
		char *recips;

		if ((strcmp(xgetenv("XYMSRV"), "0.0.0.0") != 0) || (strcmp(xgetenv("XYMSRV"), "0") != 0) || (strcmp(xgetenv("XYMSRV"), "::") == 0))
			recips = xgetenv("XYMSRV");
		else
			recips = xgetenv("XYMSERVERS");

		defaulttargets = build_targetlist(recips, defaultport, defaultsslport);
	}

	targets = ((recipient == NULL) ? defaulttargets : build_targetlist(recipient, defaultport, defaultsslport));
	sendtoall(msg, timeout, targets, response);

	res = myhead->result;	/* Always return the result from the first server */

	for (walk = myhead; (walk); walk = walk->next) {
		if (walk->result != XYMONSEND_OK) {
			char *statustext = "";
			char *eoln;

			switch (walk->result) {
				case XYMONSEND_OK            : statustext = "OK"; break;
				case XYMONSEND_EBADIP        : statustext = "Bad IP address"; break;
				case XYMONSEND_EIPUNKNOWN    : statustext = "Cannot resolve hostname"; break;
				case XYMONSEND_ENOSOCKET     : statustext = "Cannot get a socket"; break;
				case XYMONSEND_ECANNOTDONONBLOCK   : statustext = "Non-blocking I/O failed"; break;
				case XYMONSEND_ECONNFAILED   : statustext = "Connection failed"; break;
				case XYMONSEND_ESELFAILED    : statustext = "select(2) failed"; break;
				case XYMONSEND_ETIMEOUT      : statustext = "timeout"; break;
				case XYMONSEND_EWRITEERROR   : statustext = "write error"; break;
				case XYMONSEND_EREADERROR    : statustext = "read error"; break;
				case XYMONSEND_EBADURL       : statustext = "Bad URL"; break;
				default:                statustext = "Unknown error"; break;
			};

			eoln = strchr(msg, '\n'); if (eoln) *eoln = '\0';
			errprintf("Whoops ! Failed to send message (%s)\n", statustext);
			errprintf("->  %s\n", errordetails);
			errprintf("->  Recipient '%s', timeout %d\n", walk->peer, timeout);
			errprintf("->  1st line: '%s'\n", msg);
			if (eoln) *eoln = '\n';
		}
	}

	while (myhead) {
		walk = myhead; myhead = myhead->next;

		if (walk->peer) xfree(walk->peer);
		if (walk->readbuf) xfree(walk->readbuf);
		xfree(walk);
	}
	mytail = NULL;

	if (targets != defaulttargets) {
		int i;

		for (i = 0; (targets[i]); i++) {
			xfree(targets[i]->targetip);
			xfree(targets[i]);
		}

		xfree(targets);
	}

	/* Give it a break */
	if (sleepbetweenmsgs) usleep(sleepbetweenmsgs);

	return res;
}



void setproxy(char *proxy)
{
}


sendreturn_t *newsendreturnbuf(int fullresponse, FILE *respfd)
{
	sendreturn_t *result;

	result = (sendreturn_t *)calloc(1, sizeof(sendreturn_t));
	result->fullresponse = fullresponse;
	result->respfd = respfd;
	if (!respfd) {
		/* No response file, so return it in a strbuf */
		result->respstr = newstrbuffer(0);
	}
	result->haveseenhttphdrs = 1;

	return result;
}

void freesendreturnbuf(sendreturn_t *s)
{
	if (!s) return;
	if (s->respstr) freestrbuffer(s->respstr);
	xfree(s);
}

char *getsendreturnstr(sendreturn_t *s, int takeover)
{
	char *result = NULL;

	if (!s) return NULL;
	if (!s->respstr) return NULL;
	result = STRBUF(s->respstr);
	if (takeover) {
		/*
		 * We cannot leave respstr as NULL, because later calls 
		 * to sendmessage() might re-use this sendreturn_t struct
		 * and expect to get the data back. So allocate a new
		 * responsebuffer for future use - if it isn't used, it
		 * will be freed by freesendreturnbuf().
		 */
		s->respstr = newstrbuffer(0);
	}

	return result;
}



/* Routines for handling combo message transmission */
static void combo_params(void)
{
	static int issetup = 0;

	if (issetup) return;

	issetup = 1;

	if (xgetenv("MAXMSGSPERCOMBO")) maxmsgspercombo = atoi(xgetenv("MAXMSGSPERCOMBO"));
	if (maxmsgspercombo == 0) {
		/* Force it to 100 */
		dbgprintf("MAXMSGSPERCOMBO is 0, setting it to 100\n");
		maxmsgspercombo = 100;
	}

	if (xgetenv("SLEEPBETWEENMSGS")) sleepbetweenmsgs = atoi(xgetenv("SLEEPBETWEENMSGS"));
}

void combo_start(void)
{
	combo_params();

	if (xymonmsg == NULL) xymonmsg = newstrbuffer(0);
	clearstrbuffer(xymonmsg);
	addtobuffer(xymonmsg, "combo\n");
	msgsincombo = 0;
	combo_is_local = 0;
}

void combo_start_local(void)
{
	combo_start();
	combo_is_local = 1;
}

static void combo_flush(void)
{

	if (!msgsincombo) {
		dbgprintf("Flush, but xymonmsg is empty\n");
		return;
	}

	if (debug) {
		char *p1, *p2;

		dbgprintf("Flushing combo message\n");
		p1 = p2 = STRBUF(xymonmsg);

		do {
			p2++;
			p1 = strstr(p2, "\nstatus ");
			if (p1) {
				p1++; /* Skip the newline */
				p2 = strchr(p1, '\n');
				if (p2) *p2='\0';
				printf("      %s\n", p1);
				if (p2) *p2='\n';
			}
		} while (p1 && p2);
	}

	if (combo_is_local) {
		sendmessage_local(STRBUF(xymonmsg));
		combo_start_local();
	}
	else {
		sendmessage(STRBUF(xymonmsg), NULL, XYMON_TIMEOUT, NULL);
		combo_start();
	}
}

static void combo_add(strbuffer_t *buf)
{
	if (combo_is_local) {
		/* Check if message fits into the backfeed message buffer */
		if ( (STRBUFLEN(xymonmsg) + STRBUFLEN(buf)) >= max_backfeedsz) {
			combo_flush();
		}
	}
	else {
		/* Check if there is room for the message + 2 newlines */
		if (maxmsgspercombo && (msgsincombo >= maxmsgspercombo)) {
			/* Nope ... flush buffer */
			combo_flush();
		}
		else {
			/* Yep ... add delimiter before new status (but not before the first!) */
			if (msgsincombo) addtobuffer(xymonmsg, "\n\n");
		}
	}

	addtostrbuffer(xymonmsg, buf);
	msgsincombo++;
}

void combo_end(void)
{
	combo_flush();
	combo_is_local = 0;
}

void init_status(int color)
{
	if (msgbuf == NULL) msgbuf = newstrbuffer(0);
	clearstrbuffer(msgbuf);
	msgcolor = color;
}

void addtostatus(char *p)
{
	addtobuffer(msgbuf, p);
}

void addtostrstatus(strbuffer_t *p)
{
	addtostrbuffer(msgbuf, p);
}

void finish_status(void)
{
	if (debug) {
		char *p = strchr(STRBUF(msgbuf), '\n');

		if (p) *p = '\0';
		dbgprintf("Adding to combo msg: %s\n", STRBUF(msgbuf));
		if (p) *p = '\n';
	}

	combo_add(msgbuf);
}


multistatus_t *init_multi(multistatus_t **mhead, char *name, int testinterval, char *greentext, char *yellowtext, char *redtext)
{
	multistatus_t *result = NULL;

	if (*mhead) {
		result = *mhead;
		while (result && (strcmp(result->name, name) != 0)) result = result->next;

		if (result) {
			/* Bump duration to the longest duration of the sub-statuses */
			if (testinterval > result->testinterval) result->testinterval = testinterval;
			return result;
		}
	}

	result = (multistatus_t *)calloc(1, sizeof(multistatus_t));
	result->name = strdup(name);
	result->testinterval = testinterval;
	result->greentext = strdup(greentext);
	result->yellowtext = strdup(yellowtext);
	result->redtext = strdup(redtext);
	result->color = COL_GREEN;
	result->next = *mhead;
	*mhead = result;

	return result;
}

int add_multi_item(multistatus_t *item, int color, char *header)
{
	char colmsg[15];
	char *p;

	if (color > item->color) item->color = color;

	if (!item->headtext) item->headtext = newstrbuffer(0);
	sprintf(colmsg, "&%s ", colorname(color));
	p = STRBUF(item->headtext) + STRBUFLEN(item->headtext);
	addtobuffer(item->headtext, colmsg);
	addtobuffer(item->headtext, header);

	if (strstr(STRBUF(item->headtext), p) != p) {
		strbufferchop(item->headtext, strlen(p));
		return 1;
	}

	if (!item->detailtext) 
		item->detailtext = newstrbuffer(0);
	else {
		addtobuffer(item->detailtext, "--------------------------------------------------\n");
	}

	return 0;
}

void finish_multi(multistatus_t *head, char *hostname)
{
	char msgline[4096];

	while (head) {
		char *summary;
		multistatus_t *zombie = head;
		head = zombie->next;
		int validity;

		if (zombie->color == COL_RED) summary = zombie->redtext;
		else if (zombie->color == COL_YELLOW) summary = zombie->yellowtext;
		else summary = zombie->greentext;

		/* Validity = 6*(interval in minutes) = 6*(interval/(1000*60)) = interval/10000 */
		validity = zombie->testinterval / 10000;
		/* Force a minimum validity of 5 minutes for statuses */
		if (validity <= 0) validity = 5;

		init_status(zombie->color);
		sprintf(msgline, "status+%d %s.%s %s %s %s\n\n", 
			validity, hostname, zombie->name, colorname(zombie->color), timestamp, summary);
		addtostatus(msgline);
		addtostrstatus(zombie->headtext);

		if (STRBUFLEN(zombie->detailtext) > 0) {
			addtostatus("\n\n");
			addtostrstatus(zombie->detailtext);
		}
		finish_status();

		freestrbuffer(zombie->headtext);
		freestrbuffer(zombie->detailtext);
		xfree(zombie->name);
		xfree(zombie->greentext);
		xfree(zombie->yellowtext);
		xfree(zombie->redtext);
		xfree(zombie);
	}
}

