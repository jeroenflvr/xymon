/*----------------------------------------------------------------------------*/
/* Hobbit message daemon.    .                                                */
/*                                                                            */
/* This is a hobbitd worker module for the "stachg" channel.                  */
/* This module implements the file-based history logging, and keeps the       */
/* historical logfiles in bbvar/hist/ and bbvar/histlogs/ updated to keep     */
/* track of the status changes.                                               */
/*                                                                            */
/* Copyright (C) 2004-2005 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: hobbitd_history.c,v 1.36 2005-04-25 12:39:51 henrik Exp $";

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>
#include <limits.h>

#include "libbbgen.h"

#include "hobbitd_worker.h"

void sig_handler(int signum)
{
	/*
	 * Why this? Because we must have our own signal handler installed to call wait()
	 */
	switch (signum) {
	  case SIGCHLD:
		  break;
	}
}

int main(int argc, char *argv[])
{
	char *histdir = NULL;
	char *histlogdir = NULL;
	char *msg;
	int argi, seq;
	int save_allevents = 1;
	int save_hostevents = 1;
	int save_statusevents = 1;
	int save_histlogs = 1;
	FILE *alleventsfd = NULL;
	int running = 1;
	struct sigaction sa;
	char newcol2[3];
	char oldcol2[3];

	MEMDEFINE(newcol2);
	MEMDEFINE(oldcol2);

	/* Dont save the error buffer */
	save_errbuf = 0;

	if (xgetenv("BBALLHISTLOG")) save_allevents = (strcmp(xgetenv("BBALLHISTLOG"), "TRUE") == 0);
	if (xgetenv("BBHOSTHISTLOG")) save_hostevents = (strcmp(xgetenv("BBHOSTHISTLOG"), "TRUE") == 0);
	if (xgetenv("SAVESTATUSLOG")) save_histlogs = (strcmp(xgetenv("SAVESTATUSLOG"), "TRUE") == 0);

	for (argi = 1; (argi < argc); argi++) {
		if (argnmatch(argv[argi], "--histdir=")) {
			histdir = strchr(argv[argi], '=')+1;
		}
		else if (argnmatch(argv[argi], "--histlogdir=")) {
			histlogdir = strchr(argv[argi], '=')+1;
		}
		else if (argnmatch(argv[argi], "--debug")) {
			debug = 1;
		}
	}

	if (xgetenv("BBHIST") && (histdir == NULL)) {
		histdir = strdup(xgetenv("BBHIST"));
	}
	if (histdir == NULL) {
		errprintf("No history directory given, aborting\n");
		return 1;
	}

	if (save_histlogs && (histlogdir == NULL) && xgetenv("BBHISTLOGS")) {
		histlogdir = strdup(xgetenv("BBHISTLOGS"));
	}
	if (save_histlogs && (histlogdir == NULL)) {
		errprintf("No history-log directory given, aborting\n");
		return 1;
	}

	if (save_allevents) {
		char alleventsfn[PATH_MAX];

		MEMDEFINE(alleventsfn);

		sprintf(alleventsfn, "%s/allevents", histdir);
		alleventsfd = fopen(alleventsfn, "a");
		if (alleventsfd == NULL) {
			errprintf("Cannot open the all-events file '%s'\n", alleventsfn);
		}
		setvbuf(alleventsfd, (char *)NULL, _IOLBF, 0);

		MEMUNDEFINE(alleventsfn);
	}

	/* For picking up lost children */
	setup_signalhandler("hobbitd_history");
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_DFL);

	while (running) {
		char *items[20] = { NULL, };
		int metacount;
		char *p;
		char *statusdata = "";
		char *hostname, *hostnamecommas, *testname, *dismsg;
		time_t tstamp, lastchg, disabletime;
		int tstamp_i, lastchg_i;
		int newcolor, oldcolor;
		struct tm tstamptm;
		int trend;
		int childstat;

		msg = get_hobbitd_message("hobbitd_history", &seq, NULL);
		if (msg == NULL) {
			running = 0;
			continue;
		}

		p = strchr(msg, '\n'); 
		if (p) {
			*p = '\0'; 
			statusdata = msg_data(p+1);
		}
		p = gettok(msg, "|"); metacount = 0;
		while (p && (metacount < 20)) {
			items[metacount++] = p;
			p = gettok(NULL, "|");
		}

		if ((metacount > 9) && (strncmp(items[0], "@@stachg", 8) == 0)) {
			/* @@stachg#seq|timestamp|sender|origin|hostname|testname|expiretime|color|prevcolor|changetime|disabletime|disablemsg */
			sscanf(items[1], "%d.%*d", &tstamp_i); tstamp = tstamp_i;
			hostname = items[4];
			testname = items[5];
			newcolor = parse_color(items[7]);
			oldcolor = parse_color(items[8]);
			lastchg  = atoi(items[9]);
			disabletime = atoi(items[10]);
			dismsg   = items[11];

			if (save_histlogs) {
				char *hostdash;
				char fname[PATH_MAX];
				FILE *histlogfd;

				MEMDEFINE(fname);

				p = hostdash = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = '_';
				sprintf(fname, "%s/%s", histlogdir, hostdash);
				mkdir(fname, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
				p = fname + sprintf(fname, "%s/%s/%s", histlogdir, hostdash, testname);
				mkdir(fname, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
				p += sprintf(p, "/%s", histlogtime(tstamp));

				histlogfd = fopen(fname, "w");
				if (histlogfd) {
					/*
					 * When a host gets disabled or goes purple, the status
					 * message data is not changed - so it will include a
					 * wrong color as the first word of the message.
					 * Therefore we need to fixup this so it matches the
					 * newcolor value.
					 */
					int txtcolor = parse_color(statusdata);
					char *origstatus = statusdata;

					if (txtcolor != -1) {
						fprintf(histlogfd, "%s", colorname(newcolor));
						statusdata += strlen(colorname(txtcolor));
					}

					if ((disabletime > 0) && (strlen(dismsg) > 0)) {
						nldecode(dismsg);
						fprintf(histlogfd, " Disabled until %s\n%s\n\n", 
							ctime(&disabletime), dismsg);
						fprintf(histlogfd, "Status message when disabled follows:\n\n");
						statusdata = origstatus;
					}

					fwrite(statusdata, strlen(statusdata), 1, histlogfd);
					fprintf(histlogfd, "Status unchanged in 0.00 minutes\n");
					fprintf(histlogfd, "Message received from %s\n", items[2]);
					fclose(histlogfd);
				}
				else {
					errprintf("Cannot create histlog file '%s' : %s\n", fname, strerror(errno));
				}
				xfree(hostdash);

				MEMUNDEFINE(fname);
			}

			p = hostnamecommas = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = ',';

			if (save_statusevents) {
				char statuslogfn[PATH_MAX];
				int logexists;
				FILE *statuslogfd;
				char oldcol[100];
				char timestamp[40];
				struct stat st;

				MEMDEFINE(statuslogfn);
				MEMDEFINE(oldcol);
				MEMDEFINE(timestamp);

				sprintf(statuslogfn, "%s/%s.%s", histdir, hostnamecommas, testname);
				stat(statuslogfn, &st);
				statuslogfd = fopen(statuslogfn, "r+");
				logexists = (statuslogfd != NULL);
				*oldcol = '\0';

				if (logexists) {
					/*
					 * There is a fair chance hobbitd has not been
					 * running all the time while this system was monitored.
					 * So get the time of the latest status change from the file,
					 * instead of relying on the "lastchange" value we get
					 * from hobbitd. This is also needed when migrating from 
					 * standard bbd to hobbitd.
					 */
					long pos = -1;
					char l[1024];
					int gotit;

					MEMDEFINE(l);

					fseek(statuslogfd, 0, SEEK_END);
					if (ftell(statuslogfd) > 512) {
						/* Go back 512 from EOF, and skip to start of a line */
						fseek(statuslogfd, -512, SEEK_END);
						gotit = (fgets(l, sizeof(l)-1, statuslogfd) == NULL);
					}
					else {
						/* Read from beginning of file */
						fseek(statuslogfd, 0, SEEK_SET);
						gotit = 0;
					}


					while (!gotit) {
						long tmppos = ftell(statuslogfd);
						time_t dur;
						int dur_i;

						if (fgets(l, sizeof(l)-1, statuslogfd)) {
							/* Sun Oct 10 06:49:42 2004 red   1097383782 602 */

							if ((strlen(l) > 24) && 
							    (sscanf(l+24, " %s %d %d", oldcol, &lastchg_i, &dur_i) == 2)) {
								/* 
								 * Record the start location of the line
								 */
								pos = tmppos;
								lastchg = lastchg_i;
								dur = dur_i;
							}
						}
						else {
							gotit = 1;
						}
					}

					if (pos == -1) {
						/* 
						 * Couldnt find anything in the log.
						 * Take lastchg from the timestamp of the logfile,
						 * and just append the data.
						 */
						lastchg = st.st_mtime;
						fseek(statuslogfd, 0, SEEK_END);
					}
					else {
						/*
						 * lastchg was updated above.
						 * Seek to where the last line starts.
						 */
						fseek(statuslogfd, pos, SEEK_SET);
					}

					MEMUNDEFINE(l);
				}
				else {
					/*
					 * Logfile does not exist.
					 */
					lastchg = tstamp;
					statuslogfd = fopen(statuslogfn, "w");
				}

				if (strcmp(oldcol, colorname(newcolor)) == 0) {
					/* We wont update history unless the color did change. */
					errprintf("Will not update %s - color unchanged (%s)\n", statuslogfn, oldcol);
					if (hostnamecommas) xfree(hostnamecommas);
					if (statuslogfd) fclose(statuslogfd);

					MEMUNDEFINE(statuslogfn);
					MEMUNDEFINE(oldcol);
					MEMUNDEFINE(timestamp);

					continue;
				}

				if (statuslogfd) {
					if (logexists) {
						struct tm oldtm;

						/* Re-print the old record, now with the final duration */
						memcpy(&oldtm, localtime(&lastchg), sizeof(oldtm));
						strftime(timestamp, sizeof(timestamp), "%a %b %e %H:%M:%S %Y", &oldtm);
						fprintf(statuslogfd, "%s %s %d %d\n", 
							timestamp, oldcol, (int)lastchg, (int)(tstamp - lastchg));
					}

					/* And the new record. */
					memcpy(&tstamptm, localtime(&tstamp), sizeof(tstamptm));
					strftime(timestamp, sizeof(timestamp), "%a %b %e %H:%M:%S %Y", &tstamptm);
					fprintf(statuslogfd, "%s %s %d", timestamp, colorname(newcolor), (int)tstamp);

					fclose(statuslogfd);
				}
				else {
					errprintf("Cannot open status historyfile '%s' : %s\n", 
						statuslogfn, strerror(errno));
				}

				MEMUNDEFINE(statuslogfn);
				MEMUNDEFINE(oldcol);
				MEMUNDEFINE(timestamp);
			}

			strncpy(oldcol2, ((oldcolor >= 0) ? colorname(oldcolor) : "-"), 2);
			strncpy(newcol2, colorname(newcolor), 2);
			newcol2[2] = oldcol2[2] = '\0';

			if (oldcolor == -1)           trend = -1;	/* we dont know how bad it was */
			else if (newcolor > oldcolor) trend = 2;	/* It's getting worse */
			else if (newcolor < oldcolor) trend = 1;	/* It's getting better */
			else                          trend = 0;	/* Shouldn't happen ... */

			if (save_hostevents) {
				char hostlogfn[PATH_MAX];
				FILE *hostlogfd;

				MEMDEFINE(hostlogfn);

				sprintf(hostlogfn, "%s/%s", histdir, hostname);
				hostlogfd = fopen(hostlogfn, "a");
				if (hostlogfd) {
					fprintf(hostlogfd, "%s %d %d %d %s %s %d\n",
						testname, (int)tstamp, (int)lastchg, (int)(tstamp - lastchg),
						newcol2, oldcol2, trend);
					fclose(hostlogfd);
				}
				else {
					errprintf("Cannot open host logfile '%s' : %s\n", hostlogfn, strerror(errno));
				}

				MEMUNDEFINE(hostlogfn);
			}

			if (save_allevents) {
				fprintf(alleventsfd, "%s %s %d %d %d %s %s %d\n",
					hostname, testname, (int)tstamp, (int)lastchg, (int)(tstamp - lastchg),
					newcol2, oldcol2, trend);
				fflush(alleventsfd);
			}

			xfree(hostnamecommas);
		}
		else if ((metacount > 3) && ((strncmp(items[0], "@@drophost", 10) == 0))) {
			/* @@drophost|timestamp|sender|hostname */

			hostname = items[3];

			if (save_histlogs) {
				char *hostdash;
				char testdir[PATH_MAX];

				MEMDEFINE(testdir);

				/* Remove all directories below the host-specific histlog dir */
				p = hostdash = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = '_';
				sprintf(testdir, "%s/%s", histlogdir, hostdash);
				dropdirectory(testdir, 1);
				xfree(hostdash);

				MEMUNDEFINE(testdir);
			}

			if (save_hostevents) {
				char hostlogfn[PATH_MAX];
				struct stat st;

				MEMDEFINE(hostlogfn);

				sprintf(hostlogfn, "%s/%s", histdir, hostname);
				if ((stat(hostlogfn, &st) == 0) && S_ISREG(st.st_mode)) {
					unlink(hostlogfn);
				}

				MEMUNDEFINE(hostlogfn);
			}

			if (save_statusevents) {
				DIR *dirfd;
				struct dirent *de;
				char *hostlead;
				char statuslogfn[PATH_MAX];
				struct stat st;

				MEMDEFINE(statuslogfn);

				/* Remove bbvar/hist/host,name.* */
				p = hostnamecommas = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = ',';
				hostlead = malloc(strlen(hostname) + 2);
				strcpy(hostlead, hostnamecommas); strcat(hostlead, ".");

				dirfd = opendir(histdir);
				if (dirfd) {
					while ((de = readdir(dirfd)) != NULL) {
						if (strncmp(de->d_name, hostlead, strlen(hostlead)) == 0) {
							sprintf(statuslogfn, "%s/%s", histdir, de->d_name);
							if ((stat(statuslogfn, &st) == 0) && S_ISREG(st.st_mode)) {
								unlink(statuslogfn);
							}
						}
					}
					closedir(dirfd);
				}

				xfree(hostlead);
				xfree(hostnamecommas);

				MEMUNDEFINE(statuslogfn);
			}
		}
		else if ((metacount > 4) && ((strncmp(items[0], "@@droptest", 10) == 0))) {
			/* @@droptest|timestamp|sender|hostname|testname */

			hostname = items[3];
			testname = items[4];

			if (save_histlogs) {
				char *hostdash;
				char testdir[PATH_MAX];

				MEMDEFINE(testdir);

				p = hostdash = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = '_';
				sprintf(testdir, "%s/%s/%s", histlogdir, hostdash, testname);
				dropdirectory(testdir, 1);
				xfree(hostdash);

				MEMUNDEFINE(testdir);
			}

			if (save_statusevents) {
				char *hostnamecommas;
				char statuslogfn[PATH_MAX];
				struct stat st;

				MEMDEFINE(statuslogfn);

				p = hostnamecommas = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = ',';
				sprintf(statuslogfn, "%s/%s.%s", histdir, hostnamecommas, testname);
				if ((stat(statuslogfn, &st) == 0) && S_ISREG(st.st_mode)) unlink(statuslogfn);
				xfree(hostnamecommas);

				MEMUNDEFINE(statuslogfn);
			}
		}
		else if ((metacount > 4) && ((strncmp(items[0], "@@renamehost", 12) == 0))) {
			/* @@renamehost|timestamp|sender|hostname|newhostname */
			char *newhostname;

			hostname = items[3];
			newhostname = items[4];

			if (save_histlogs) {
				char *hostdash;
				char *newhostdash;
				char olddir[PATH_MAX];
				char newdir[PATH_MAX];

				MEMDEFINE(olddir); MEMDEFINE(newdir);

				p = hostdash = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = '_';
				p = newhostdash = strdup(newhostname); while ((p = strchr(p, '.')) != NULL) *p = '_';
				sprintf(olddir, "%s/%s", histlogdir, hostdash);
				sprintf(newdir, "%s/%s", histlogdir, newhostdash);
				rename(olddir, newdir);
				xfree(newhostdash);
				xfree(hostdash);

				MEMUNDEFINE(newdir); MEMUNDEFINE(olddir);
			}

			if (save_hostevents) {
				char hostlogfn[PATH_MAX];
				char newhostlogfn[PATH_MAX];

				MEMDEFINE(hostlogfn); MEMDEFINE(newhostlogfn);

				sprintf(hostlogfn, "%s/%s", histdir, hostname);
				sprintf(newhostlogfn, "%s/%s", histdir, newhostname);
				rename(hostlogfn, newhostlogfn);

				MEMUNDEFINE(hostlogfn); MEMUNDEFINE(newhostlogfn);
			}

			if (save_statusevents) {
				DIR *dirfd;
				struct dirent *de;
				char *hostlead;
				char *newhostnamecommas;
				char statuslogfn[PATH_MAX];
				char newlogfn[PATH_MAX];

				MEMDEFINE(statuslogfn); MEMDEFINE(newlogfn);

				p = hostnamecommas = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = ',';
				hostlead = malloc(strlen(hostname) + 2);
				strcpy(hostlead, hostnamecommas); strcat(hostlead, ".");

				p = newhostnamecommas = strdup(newhostname); while ((p = strchr(p, '.')) != NULL) *p = ',';


				dirfd = opendir(histdir);
				if (dirfd) {
					while ((de = readdir(dirfd)) != NULL) {
						if (strncmp(de->d_name, hostlead, strlen(hostlead)) == 0) {
							char *testname = strchr(de->d_name, '.');
							sprintf(statuslogfn, "%s/%s", histdir, de->d_name);
							sprintf(newlogfn, "%s/%s%s", histdir, newhostnamecommas, testname);
							rename(statuslogfn, newlogfn);
						}
					}
					closedir(dirfd);
				}

				xfree(newhostnamecommas);
				xfree(hostlead);
				xfree(hostnamecommas);

				MEMUNDEFINE(statuslogfn); MEMUNDEFINE(newlogfn);
			}
		}
		else if ((metacount > 5) && (strncmp(items[0], "@@renametest", 12) == 0)) {
			/* @@renametest|timestamp|sender|hostname|oldtestname|newtestname */
			char *newtestname;

			hostname = items[3];
			testname = items[4];
			newtestname = items[5];

			if (save_histlogs) {
				char *hostdash;
				char olddir[PATH_MAX];
				char newdir[PATH_MAX];

				MEMDEFINE(olddir); MEMDEFINE(newdir);

				p = hostdash = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = '_';
				sprintf(olddir, "%s/%s/%s", histlogdir, hostdash, testname);
				sprintf(newdir, "%s/%s/%s", histlogdir, hostdash, newtestname);
				rename(olddir, newdir);
				xfree(hostdash);

				MEMUNDEFINE(newdir); MEMUNDEFINE(olddir);
			}

			if (save_statusevents) {
				char *hostnamecommas;
				char statuslogfn[PATH_MAX];
				char newstatuslogfn[PATH_MAX];

				MEMDEFINE(statuslogfn); MEMDEFINE(newstatuslogfn);

				p = hostnamecommas = strdup(hostname); while ((p = strchr(p, '.')) != NULL) *p = ',';
				sprintf(statuslogfn, "%s/%s.%s", histdir, hostnamecommas, testname);
				sprintf(newstatuslogfn, "%s/%s.%s", histdir, hostnamecommas, newtestname);
				rename(statuslogfn, newstatuslogfn);
				xfree(hostnamecommas);

				MEMUNDEFINE(newstatuslogfn); MEMUNDEFINE(statuslogfn);
			}
		}
		else if (strncmp(items[0], "@@shutdown", 10) == 0) {
			running = 0;
		}
		else if (strncmp(items[0], "@@logrotate", 11) == 0) {
			char *fn = xgetenv("HOBBITCHANNEL_LOGFILENAME");
			if (fn && strlen(fn)) {
				freopen(fn, "a", stdout);
				freopen(fn, "a", stderr);
			}
			continue;
		}

		/* Pickup any finished child processes to avoid zombies */
		while (wait3(&childstat, WNOHANG, NULL) > 0) ;
	}

	MEMUNDEFINE(newcol2);
	MEMUNDEFINE(oldcol2);

	fclose(alleventsfd);
	return 0;
}

