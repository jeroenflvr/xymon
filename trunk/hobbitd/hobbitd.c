/*----------------------------------------------------------------------------*/
/* Big Brother message daemon.                                                */
/*                                                                            */
/* Copyright (C) 2004 Henrik Storner <henrik@hswn.dk>                         */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: hobbitd.c,v 1.1 2004-10-03 16:13:59 henrik Exp $";

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#if !defined(HPUX)              /* HP-UX has select() and friends in sys/types.h */
#include <sys/select.h>         /* Someday I'll move to GNU Autoconf for this ... */
#endif
#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <netdb.h>
#include <ctype.h>
#include <signal.h>

#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/shm.h>

#if defined(__GNU_LIBRARY__) && !defined(_SEM_SEMUN_UNDEFINED)
/* union semun is defined by including <sys/sem.h> */
#else
/* according to X/OPEN we have to define it ourselves */
union semun {
	int val;                  /* value for SETVAL */
	struct semid_ds *buf;     /* buffer for IPC_STAT, IPC_SET */
	unsigned short *array;    /* array for GETALL, SETALL */
};
#endif

#include "bbgen.h"
#include "util.h"
#include "debug.h"
#include "bbd_net.h"
#include "bbdutil.h"

/* These are dummy vars needed by stuff in util.c */
hostlist_t      *hosthead = NULL;
link_t          *linkhead = NULL;
link_t  null_link = { "", "", "", NULL };


bbd_hostlist_t *hosts = NULL;
bbd_testlist_t *tests = NULL;
bbd_log_t *logs = NULL;

#define NOTALK 0
#define RECEIVING 1
#define RESPONDING 2

typedef struct conn_t {
	int sock;
	struct sockaddr_in addr;
	unsigned char *buf, *bufp;
	int buflen, bufsz;
	int doingwhat;
	struct conn_t *next;
} conn_t;

static volatile int running = 1;
bbd_channel_t *statuschn = NULL;
bbd_channel_t *stachgchn = NULL;
bbd_channel_t *pagechn   = NULL;
bbd_channel_t *datachn   = NULL;
bbd_channel_t *noteschn  = NULL;

#define NO_COLOR (COL_COUNT)
static char *colnames[COL_COUNT+1];
int alertcolors = ( (1 << COL_RED) | (1 << COL_YELLOW) | (1 << COL_PURPLE) );


void posttochannel(bbd_channel_t *channel, char *msg, int msglen, 
		   char *sender, char *hostname, char *testname, time_t validity, 
		   int newcolor, int oldcolor, time_t lastchange)
{
	struct sembuf s;
	struct shmid_ds chninfo;
	int clients;
	int n;
	struct timespec tmo;
	struct timeval tstamp;
	struct timezone tz;

	/* If any message outstanding, wait until it has been noticed.... */
	s.sem_num = 0;
	s.sem_op = 0;  /* wait until all reads are done (semaphore is 0) */
	s.sem_flg = 0; /* no flags - perhaps IPC_NOWAIT .... */
	dprintf("Waiting for readers to notice last message\n");
	n = semop(channel->semid, &s, 1);
	dprintf("All readers have seen it (sem 0 is 0)\n");
	
	/* ... and picked up. */
	n = shmctl(channel->shmid, IPC_STAT, &chninfo);
	clients = chninfo.shm_nattch-1;
	if (clients == 0) {
		dprintf("Dropping message - no readers\n");
		return;
	}

	s.sem_num = 1;
	s.sem_op = -clients;
	s.sem_flg = 0;
	dprintf("Waiting for %d readers to finish with message\n", clients);
	tmo.tv_sec = 1; tmo.tv_nsec = 0;
	n = semtimedop(channel->semid, &s, 1, &tmo);
	if ((n == -1) && (errno == EAGAIN)) {
		union semun su;
		dprintf("Hung reader, ignoring it\n");
		su.val = 0;
		semctl(channel->semid, 1, SETVAL, &su);
	}
	dprintf("Readers are done with last message\n");

	/* All clear, post the message */
	gettimeofday(&tstamp, &tz);
	n = sprintf(channel->channelbuf, "@@%s|%d.%06d|%s|%s|%s|%d|%s|%s|%d\n", 
		    channelnames[channel->channelid], tstamp.tv_sec, tstamp.tv_usec, 
		    sender, hostname, testname, validity, colnames[newcolor], colnames[oldcolor], lastchange);
	memcpy(channel->channelbuf+n, msg, msglen);
	*(channel->channelbuf + msglen + n) = '\0';

	/* Let the readers know it is there.  */
	s.sem_num = 0;
	s.sem_op = clients;
	s.sem_flg = 0;
	dprintf("Posting message to %d readers\n", clients);
	n = semop(channel->semid, &s, 1);
	dprintf("Message posted\n");

	return;
}

void get_hts(char *msg, bbd_hostlist_t **host, bbd_testlist_t **test, bbd_log_t **log, 
	     int *color, int createhost, int createlog)
{
	/* "msg" contains an incoming message. First list is of the form "KEYWORD host,domain.test COLOR" */
	char *l, *p;
	char *hosttest, *hostname, *testname, *colstr;
	bbd_hostlist_t *hwalk;
	bbd_testlist_t *twalk;
	bbd_log_t *lwalk;

	*host = NULL;
	*test = NULL;
	*log = NULL;
	*color = -1;

	hosttest = hostname = testname = colstr = NULL;
	p = strchr(msg, '\n');
	if (p == NULL) {
		l = strdup(msg);
	}
	else {
		*p = '\0';
		l = strdup(msg); 
		*p = '\n';
	}

	p = strtok(l, " \t");
	if (p) {

		hosttest = strtok(NULL, " \t");
	}
	if (hosttest) colstr = strtok(NULL, " \n\t");

	if (hosttest) {
		hostname = hosttest;
		testname = strrchr(hosttest, '.');
		if (testname) { *testname = '\0'; testname++; }
		p = hostname;
		while (p = strchr(p, ',')) *p = '.';
	}

	for (hwalk = hosts; (hwalk && strcasecmp(hostname, hwalk->hostname) && strcasecmp(hostname, hwalk->alias)); hwalk = hwalk->next) ;
	if (createhost && (hwalk == NULL)) {
		hwalk = (bbd_hostlist_t *)malloc(sizeof(bbd_hostlist_t));
		hwalk->hostname = strdup(hostname);
		hwalk->alias = "";
		hwalk->logs = NULL;
		hwalk->next = hosts;
		hosts = hwalk;
	}
	for (twalk = tests; (twalk && strcasecmp(testname, twalk->testname)); twalk = twalk->next);
	if (createlog && (twalk == NULL)) {
		twalk = (bbd_testlist_t *)malloc(sizeof(bbd_testlist_t));
		twalk->testname = strdup(testname);
		twalk->next = tests;
		tests = twalk;
	}
	if (hwalk && twalk) {
		for (lwalk = hwalk->logs; (lwalk && (lwalk->test != twalk)); lwalk = lwalk->next);
		if (createlog && (lwalk == NULL)) {
			lwalk = (bbd_log_t *)malloc(sizeof(bbd_log_t));
			lwalk->color = NO_COLOR;
			lwalk->test = twalk;
			lwalk->message = NULL;
			lwalk->msgsz = 0;
			lwalk->dismsg = NULL;
			lwalk->lastchange = lwalk->validtime = lwalk->enabletime = 0;
			lwalk->next = hwalk->logs;
			hwalk->logs = lwalk;
		}
	}

	*host = hwalk;
	*test = twalk;
	*log = lwalk;
	if (colstr) *color = parse_color(colstr);
	free(l);
}


void handle_status(char *msg, int msglen, char *sender, char *hostname, char *testname, 
		   bbd_log_t *log, int newcolor)
{
	int validity = 30*60;
	int oldcolor = log->color;
	time_t now = time(NULL);
	char *umsg = msg;
	int umsglen = msglen;

	if (strncmp(msg, "status+", 7) == 0) {
		validity = atoi(msg+7);
	}

	if (log->enabletime > now) {
		/* The test is currently disabled. */
		newcolor = COL_BLUE;
		log->validtime = log->enabletime;
		umsglen = msglen + strlen(log->dismsg) + 2;
		umsg = (unsigned char *)malloc(umsglen);
		strcpy(umsg, log->dismsg);
		strcat(umsg, "\n");
		strcat(umsg, msg);
	}
	else {
		log->validtime = now + validity;
	}

	log->color = newcolor;
	if ((log->message == NULL) || (log->msgsz = 0)) {
		log->message = strdup(umsg);
		log->msgsz = umsglen;
	}
	else if (log->msgsz >= umsglen) {
		strcpy(log->message, umsg);
	}
	else {
		log->message = realloc(log->message, umsglen);
		strcpy(log->message, umsg);
		log->msgsz = umsglen;
	}

	if (oldcolor != newcolor) {
		int oldalertstatus = ((alertcolors & (1 << oldcolor)) != 0);
		int newalertstatus = ((alertcolors & (1 << newcolor)) != 0);

		dprintf("oldcolor=%d, oldas=%d, newcolor=%d, newas=%d\n", 
			oldcolor, oldalertstatus, newcolor, newalertstatus);

		if (oldalertstatus != newalertstatus) {
			/* alert status changed. Tell the pagers */
			dprintf("posting to page channel\n");
			posttochannel(pagechn, umsg, umsglen, 
					sender, hostname, testname, validity, newcolor, oldcolor, log->lastchange);
		}

		dprintf("posting to stachg channel\n");
		posttochannel(stachgchn, umsg, umsglen, 
				sender, hostname, testname, validity, newcolor, oldcolor, log->lastchange);
		log->lastchange = time(NULL);
	}

	dprintf("posting to status channel\n");
	posttochannel(statuschn, umsg, umsglen, sender, hostname, testname, validity, newcolor, oldcolor, log->lastchange);
}

void handle_data(char *msg, int msglen, char *sender, char *hostname, char *testname)
{
	posttochannel(datachn, msg, msglen, sender, hostname, testname, 0, NO_COLOR, NO_COLOR, -1);
}

void handle_notes(char *msg, int msglen, char *sender, char *hostname)
{
	posttochannel(noteschn, msg, msglen, sender, hostname, "", 0, NO_COLOR, NO_COLOR, -1);
}

void handle_enadis(int enabled, char *msg, int msglen, char *sender)
{
	char firstline[200];
	char hosttest[200];
	char *tname = NULL;
	int duration = 0;
	int assignments;
	int alltests = 0;
	bbd_hostlist_t *hwalk = NULL;
	bbd_testlist_t *twalk = NULL;
	bbd_log_t *log;
	char *p;

	p = strchr(msg, '\n'); 
	if (p == NULL) {
		strncpy(firstline, msg, sizeof(firstline)-1);
	}
	else {
		*p = '\0';
		strncpy(firstline, msg, sizeof(firstline)-1);
		*p = '\n';
	}
	assignments = sscanf(firstline, "%*s %199s %d", hosttest, &duration);
	if (assignments < 1) return;
	p = hosttest + strlen(hosttest) - 1;
	if (*p == '*') {
		/* It ends with a '*' so assume this is for all tests */
		alltests = 1;
		*p = '\0';
		p--;
		if (*p == '.') *p = '\0';
	}
	else {
		/* No wildcard -> get the test name */
		p = strrchr(hosttest, '.');
		if (p == NULL) return; /* "enable foo" ... surely you must be joking. */
		*p = '\0';
		tname = (p+1);
	}
	p = hosttest; while (p = strchr(p, ',')) *p = '.';

	for (hwalk = hosts; (hwalk && strcasecmp(hosttest, hwalk->hostname) && strcasecmp(hosttest, hwalk->alias)); hwalk = hwalk->next) ;
	if (hwalk == NULL) {
		/* Unknown host */
		return;
	}

	if (tname) {
		for (twalk = tests; (twalk && strcasecmp(tname, twalk->testname)); twalk = twalk->next);
		if (twalk == NULL) {
			/* Unknown test */
			return;
		}
	}

	if (enabled) {
		/* Enable is easy - just clear the enabletime */
		if (alltests) {
			for (log = hwalk->logs; (log); log = log->next) {
				log->enabletime = 0;
				if (log->dismsg) {
					free(log->dismsg);
					log->dismsg = NULL;
				}
			}
		}
		else {
			for (log = hwalk->logs; (log && (log->test != twalk)); log = log->next) ;
			if (log) {
				log->enabletime = 0;
				if (log->dismsg) {
					free(log->dismsg);
					log->dismsg = NULL;
				}
			}
		}
	}
	else {
		/* disable code goes here */
		time_t expires = time(NULL) + duration*60;
		char *dismsg = malloc(msglen + 500);

		p = hosttest; while (p = strchr(p, '.')) *p = ',';
		sprintf(dismsg, "status %s.%s+%d blue disabled until %s\n", hosttest, tname, duration*60, ctime(&expires));
		p = msg;
		while (!isspace(*p)) p++;	/* Skip "disable".... */
		while (isspace(*p)) p++;	/* and the space ... */
		while (!isspace(*p)) p++;	/* and the host.test ... */
		while (isspace(*p)) p++;	/* and the space ... */
		while (!isspace(*p)) p++;	/* and the duration ... */
		while (isspace(*p)) p++;	/* and the space */
		strcat(dismsg, p);
		strcat(dismsg, "\nStatus received while disabled was:\n");

		if (alltests) {
			for (log = hwalk->logs; (log); log = log->next) {
				log->enabletime = expires;
				if (dismsg) {
					if (log->dismsg) free(log->dismsg);
					log->dismsg = strdup(dismsg);
				}
			}
		}
		else {
			for (log = hwalk->logs; (log && (log->test != twalk)); log = log->next) ;
			if (log) {
				log->enabletime = expires;
				if (dismsg) {
					if (log->dismsg) free(log->dismsg);
					log->dismsg = strdup(dismsg);
				}
			}
		}
	}

	return;
}

void do_message(conn_t *msg)
{
	bbd_hostlist_t *h;
	bbd_testlist_t *t;
	bbd_log_t *log;
	int color;

	/* Most likely, we will not send a response */
	msg->doingwhat = NOTALK;

	if (strncmp(msg->buf, "combo\n", 6) == 0) {
		char *currmsg, *nextmsg;

		currmsg = msg->buf+6;
		do {
			nextmsg = strstr(currmsg, "\n\nstatus");
			if (nextmsg) { *(nextmsg+1) = '\0'; nextmsg += 2; }

			get_hts(currmsg, &h, &t, &log, &color, 1, 1);
			handle_status(currmsg, strlen(currmsg), inet_ntoa(msg->addr.sin_addr), 
					h->hostname, t->testname, log, color);

			currmsg = nextmsg;
		} while (currmsg);
	}
	else if (strncmp(msg->buf, "status", 6) == 0) {
		get_hts(msg->buf, &h, &t, &log, &color, 1, 1);
		handle_status(msg->buf, msg->buflen, inet_ntoa(msg->addr.sin_addr), 
				h->hostname, t->testname, log, color);
	}
	else if (strncmp(msg->buf, "data", 4) == 0) {
		get_hts(msg->buf, &h, &t, &log, &color, 1, 1);
		if (h && t) handle_data(msg->buf, msg->buflen, inet_ntoa(msg->addr.sin_addr), h->hostname, t->testname);
	}
	else if (strncmp(msg->buf, "summary", 7) == 0) {
		get_hts(msg->buf, &h, &t, &log, &color, 1, 1);
		handle_status(msg->buf, msg->buflen, inet_ntoa(msg->addr.sin_addr), 
				h->hostname, t->testname, log, color);
	}
	else if (strncmp(msg->buf, "notes", 5) == 0) {
		get_hts(msg->buf, &h, &t, &log, &color, 1, 0);
		if (h) handle_notes(msg->buf, msg->buflen, inet_ntoa(msg->addr.sin_addr), h->hostname);
	}
	else if (strncmp(msg->buf, "enable", 6) == 0) {
		handle_enadis(1, msg->buf, msg->buflen, inet_ntoa(msg->addr.sin_addr));
	}
	else if (strncmp(msg->buf, "disable", 7) == 0) {
		handle_enadis(0, msg->buf, msg->buflen, inet_ntoa(msg->addr.sin_addr));
	}
	else if (strncmp(msg->buf, "config", 6) == 0) {
		char conffn[1024];
		FILE *conffd = NULL;

		if ( (sscanf(msg->buf, "config %1023s", conffn) == 1) &&
		     (strstr("../", conffn) == NULL) && get_config(conffn, msg) ) {
			msg->doingwhat = RESPONDING;
			msg->bufp = msg->buf;
		}
	}
	else if (strncmp(msg->buf, "query", 5) == 0) {
		get_hts(msg->buf, &h, &t, &log, &color, 0, 0);
		if (log) {
			unsigned char *p;

			msg->doingwhat = RESPONDING;
			p = strchr(log->message, '\n');
			if (p) msg->buflen = (p - log->message + 1); else msg->buflen = strlen(log->message);

			strncpy(msg->buf, log->message, msg->buflen);
			*(msg->buf + msg->buflen) = '\0';
			msg->bufp = msg->buf;

			if (strncmp(msg->bufp, "status", 6) == 0) { 
				p = strchr(msg->bufp, '.'); /* Skip to the '.' in host.test */
				if (p) {
					int skipped;

					do { p++; } while (!isspace((int) *p));
					while (isspace((int) *p)) p++;

					skipped = (p - msg->bufp); 
					msg->buflen -= skipped;
					msg->bufp += skipped;
				}
			}
		}
	}

	if (msg->doingwhat == RESPONDING) {
		shutdown(msg->sock, SHUT_RD);
	}
	else {
		shutdown(msg->sock, SHUT_RDWR);
		close(msg->sock);
		msg->sock = -1;
	}
}


int get_config(char *fn, conn_t *msg)
{
}

void sig_handler(int signum)
{
	running = 0;
}

int main(int argc, char *argv[])
{
	conn_t *chead = NULL;
	char *listenip = "0.0.0.0";
	int listenport = 1984;
	struct sockaddr_in laddr;
	int lsocket, opt;
	int listenq = 512;
	int argi;

	colnames[COL_GREEN] = "green";
	colnames[COL_YELLOW] = "yellow";
	colnames[COL_RED] = "red";
	colnames[COL_CLEAR] = "clear";
	colnames[COL_BLUE] = "blue";
	colnames[COL_PURPLE] = "purple";
	colnames[NO_COLOR] = "none";

	for (argi=1; (argi < argc); argi++) {
		if (strcmp(argv[argi], "--debug") == 0) {
			debug = 1;
		}
		else if (strncmp(argv[argi], "--listen=", 9) == 0) {
			char *p = strchr(argv[argi], '=') + 1;

			listenip = strdup(p);
			p = strchr(listenip, ':');
			if (p) {
				*p = '\0';
				listenport = atoi(p+1);
			}
		}
		else if (strncmp(argv[argi], "--alertcolors=", 14) == 0) {
		}
	}

	/* Set up a socket to listen for new connections */
	memset(&laddr, 0, sizeof(laddr));
	inet_aton(listenip, (struct in_addr *) &laddr.sin_addr.s_addr);
	laddr.sin_port = htons(listenport);
	laddr.sin_family = AF_INET;
	lsocket = socket(AF_INET, SOCK_STREAM, 0);
	if (lsocket == -1) {
		errprintf("Cannot create listen socket (%s)\n", strerror(errno));
		return 1;
	}
	opt = 1;
	setsockopt(lsocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	fcntl(lsocket, F_SETFL, O_NONBLOCK);
	if (bind(lsocket, (struct sockaddr *)&laddr, sizeof(laddr)) == -1) {
		errprintf("Cannot bind to listen socket (%s)\n", strerror(errno));
		return 1;
	}
	if (listen(lsocket, listenq) == -1) {
		errprintf("Cannot listen (%s)\n", strerror(errno));
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	statuschn = setup_channel(C_STATUS, (IPC_CREAT|0600));
	stachgchn = setup_channel(C_STACHG, (IPC_CREAT|0600));
	pagechn   = setup_channel(C_PAGE, (IPC_CREAT|0600));
	datachn   = setup_channel(C_DATA, (IPC_CREAT|0600));
	noteschn  = setup_channel(C_NOTES, (IPC_CREAT|0600));

	do {
		fd_set fdread, fdwrite;
		int maxfd, n;
		conn_t *cwalk;

		FD_ZERO(&fdread); FD_ZERO(&fdwrite);
		FD_SET(lsocket, &fdread); maxfd = lsocket;

		for (cwalk = chead; (cwalk); cwalk = cwalk->next) {
			switch (cwalk->doingwhat) {
				case RECEIVING:
					FD_SET(cwalk->sock, &fdread);
					if (cwalk->sock > maxfd) maxfd = cwalk->sock;
					break;
				case RESPONDING:
					FD_SET(cwalk->sock, &fdwrite);
					if (cwalk->sock > maxfd) maxfd = cwalk->sock;
					break;
			}
		}

		n = select(maxfd+1, &fdread, &fdwrite, NULL, NULL);
		if (n <= 0) break;

		for (cwalk = chead; (cwalk); cwalk = cwalk->next) {
			switch (cwalk->doingwhat) {
			  case RECEIVING:
				if (FD_ISSET(cwalk->sock, &fdread)) {
					n = read(cwalk->sock, cwalk->bufp, (cwalk->bufsz - cwalk->buflen - 1));
					if (n <= 0) {
						if (cwalk->buflen) do_message(cwalk);
					}
					else {
						cwalk->bufp += n;
						cwalk->buflen += n;
						*(cwalk->bufp) = '\0';
						if ((cwalk->bufsz - cwalk->buflen) < 2048) {
							cwalk->bufsz += 2048;
							cwalk->buf = (unsigned char *) realloc(cwalk->buf, cwalk->bufsz);
							cwalk->bufp = cwalk->buf + cwalk->buflen;
						}
					}
				}
				break;

			  case RESPONDING:
				if (FD_ISSET(cwalk->sock, &fdwrite)) {
					n = write(cwalk->sock, cwalk->bufp, cwalk->buflen);

					if (n < 0) {
						cwalk->buflen = 0;
					}
					else {
						cwalk->bufp += n;
						cwalk->buflen -= n;
					}

					if (cwalk->buflen == 0) {
						shutdown(cwalk->sock, SHUT_WR);
						close(cwalk->sock); 
						cwalk->sock = -1; 
						cwalk->doingwhat = NOTALK;
					}
				}
				break;
			}
		}

		while (chead && (chead->doingwhat == NOTALK)) {
			conn_t *tmp = chead;
			chead = chead->next;
			free(tmp->buf);
			free(tmp);
		}

		if (FD_ISSET(lsocket, &fdread)) {
			/* New connection */
			struct sockaddr_in addr;
			int addrsz = sizeof(addr);
			int sock = accept(lsocket, (struct sockaddr *)&addr, &addrsz);

			if (sock >= 0) {
				conn_t *newconn = (conn_t *)malloc(sizeof(conn_t));

				newconn->sock = sock;
				memcpy(&newconn->addr, &addr, sizeof(addr));
				newconn->doingwhat = RECEIVING;
				newconn->buf = (unsigned char *)malloc(MAXMSG);
				newconn->bufp = newconn->buf;
				newconn->buflen = 0;
				newconn->bufsz = MAXMSG;
				newconn->next = chead;
				chead = newconn;
			}
		}
	} while (running);

	close_channel(statuschn, 1);
	close_channel(stachgchn, 1);
	close_channel(pagechn, 1);
	close_channel(datachn, 1);
	close_channel(noteschn, 1);

	return 0;
}

