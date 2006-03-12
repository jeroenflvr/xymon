/*----------------------------------------------------------------------------*/
/* Hobbit eventlog generator tool.                                            */
/*                                                                            */
/* This displays the "eventlog" found on the "All non-green status" page.     */
/* It also implements a CGI tool to show an eventlog for a given period of    */
/* time, as a reporting function.                                             */
/*                                                                            */
/* Copyright (C) 2002-2005 Henrik Storner <henrik@storner.dk>                 */
/* Host/test/color/start/end filtering code by Eric Schwimmer 2005            */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: bb-eventlog.c,v 1.28 2006-03-12 16:38:32 henrik Exp $";

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "libbbgen.h"

int	maxcount = 100;		/* Default: Include last 100 events */
int	maxminutes = 240;	/* Default: for the past 4 hours */
char	*totime = NULL;
char	*fromtime = NULL;
char	*hostregex = NULL;
char	*testregex = NULL;
char	*colrregex = NULL;
int	ignoredialups = 0;
cgidata_t *cgidata = NULL;

static void errormsg(char *msg)
{
	printf("Content-type: text/html\n\n");
	printf("<html><head><title>Invalid request</title></head>\n");
	printf("<body>%s</body></html>\n", msg);
	exit(1);
}

static void parse_query(void)
{
	cgidata_t *cwalk;

	cwalk = cgidata;
	while (cwalk) {
		/*
		 * cwalk->name points to the name of the setting.
		 * cwalk->value points to the value (may be an empty string).
		 */

		if (strcasecmp(cwalk->name, "MAXCOUNT") == 0) {
			maxcount = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "MAXTIME") == 0) {
			maxminutes = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "FROMTIME") == 0) {
			if (*(cwalk->value)) fromtime = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "TOTIME") == 0) {
			if (*(cwalk->value)) totime = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "HOSTMATCH") == 0) {
			if (*(cwalk->value)) hostregex = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "TESTMATCH") == 0) {
			if (*(cwalk->value)) testregex = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "COLORMATCH") == 0) {
			if (*(cwalk->value)) colrregex = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "NODIALUPS") == 0) {
			ignoredialups = 1;
		}

		cwalk = cwalk->next;
	}
}

int main(int argc, char *argv[])
{
	int argi;
	char *envarea = NULL;

	for (argi=1; (argi < argc); argi++) {
		if (argnmatch(argv[argi], "--env=")) {
			char *p = strchr(argv[argi], '=');
			loadenv(p+1, envarea);
		}
		else if (argnmatch(argv[argi], "--area=")) {
			char *p = strchr(argv[argi], '=');
			envarea = strdup(p+1);
		}
	}

	redirect_cgilog("bb-eventlog");

	cgidata = cgi_request();
	if (cgidata == NULL) {
		/* Present the query form */
		sethostenv("", "", "", colorname(COL_BLUE), NULL);
		showform(stdout, "event", "event_form", COL_BLUE, getcurrenttime(NULL), NULL);
		return 0;
	}

	parse_query();
	load_hostnames(xgetenv("BBHOSTS"), NULL, get_fqdn());

	/* Now generate the webpage */
	printf("Content-Type: text/html\n\n");

	headfoot(stdout, "event", "", "header", COL_GREEN);
	fprintf(stdout, "<center>\n");
	do_eventlog(stdout, maxcount, maxminutes, fromtime, totime, hostregex, testregex, colrregex, ignoredialups);
	fprintf(stdout, "</center>\n");
	headfoot(stdout, "event", "", "footer", COL_GREEN);

	return 0;
}

