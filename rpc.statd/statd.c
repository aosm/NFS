/*
 * Copyright (c) 2002-2008 Apple Inc.  All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1995
 *	A.R. Gordon (andrew.gordon@net-tel.co.uk).  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed for the FreeBSD project
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ANDREW GORDON AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#ifndef lint
static const char rcsid[] = "$FreeBSD$";
#endif				/* not lint */

/* main() function for status monitor daemon.  Some of the code in this	 */
/* file was generated by running rpcgen /usr/include/rpcsvc/sm_inter.x	 */
/* The actual program logic is in the file procs.c			 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/param.h>
#include <libutil.h>
#include <spawn.h>
#include <launch.h>
#include "statd.h"

int bindresvport_sa(int sd, struct sockaddr * sa);

int statd_server = 0;		/* are we the statd server (not notify, list...) */
int notify_only = 0;		/* just send SM_NOTIFY messages */
int list_only = 0;		/* just list status database entries */

int udpport, tcpport;

struct pidfh *pfh = NULL;

const struct nfs_conf_statd config_defaults =
{
	0,			/* port */
	0,			/* simu_crash_allowed */
	0,			/* verbose */
};
struct nfs_conf_statd config;

int log_to_stderr = 0;

extern void sm_prog_1(struct svc_req * rqstp, SVCXPRT * transp);
static void handle_sigchld(int sig);
static void cleanup(int sig);
static void usage(void);
static int config_read(struct nfs_conf_statd * conf);

int
main(int argc, char **argv)
{
	SVCXPRT *transp;
	struct sigaction sa;
	int c, sockfd;
	pid_t pid;
	struct sockaddr_in inetaddr;
	socklen_t socklen;
	char *unnotify_host = NULL;	/* host to "unnotify" */
	int rv, need_notify;

	config = config_defaults;
	config_read(&config);

	while ((c = getopt(argc, argv, "dnlLN:")) != EOF)
		switch (c) {
		case 'd':
			config.verbose = INT_MAX;
			break;
		case 'n':
			if (list_only || unnotify_host)
				usage();
			notify_only = 1;
			break;
		case 'l':
			if (notify_only || unnotify_host || (list_only == LIST_MODE_WATCH))
				usage();
			list_only = LIST_MODE_ONCE;
			break;
		case 'L':
			if (notify_only || unnotify_host || (list_only == LIST_MODE_ONCE))
				usage();
			list_only = LIST_MODE_WATCH;
			break;
		case 'N':
			if (notify_only || unnotify_host || list_only)
				usage();
			unnotify_host = optarg;
			break;
		default:
			usage();
		}

	if (list_only || unnotify_host)
		log_to_stderr = 1;

	if (list_only)
		exit(list_hosts(list_only));

	if (getuid()) {
		log(LOG_ERR, "Sorry, rpc.statd must be run as root");
		exit(0);
	}
	if (unnotify_host)
		exit(do_unnotify_host(unnotify_host));

	/* Install signal handler to do cleanup */
	signal(SIGINT, cleanup);
	signal(SIGTERM, cleanup);
	signal(SIGHUP, cleanup);
	signal(SIGQUIT, cleanup);

	openlog("rpc.statd", LOG_PID | LOG_CONS, LOG_DAEMON);
	setlogmask(LOG_UPTO(LOG_LEVEL));

	if (notify_only) {
		rv = notify_hosts();
		if (rv)
			log(LOG_NOTICE, "statd.notify exiting %d", rv);
		exit(rv);
	}
	statd_server = 1;
	log(LOG_INFO, "statd starting");

	/* claim PID file */
	pfh = pidfile_open(_PATH_STATD_PID, 0644, &pid);
	if (pfh == NULL) {
		log(LOG_ERR, "can't open statd pidfile: %s (%d)", strerror(errno), errno);
		if (errno == EEXIST) {
			log(LOG_ERR, "statd already running, pid: %d", pid);
			exit(0);
		}
		exit(2);
	}
	if (pidfile_write(pfh) == -1)
		log(LOG_WARNING, "can't write to statd pidfile: %s (%d)", strerror(errno), errno);

	need_notify = init_file(_PATH_STATD_DATABASE);
	if (need_notify && !get_statd_notify_pid()) {
		/*
	         * It looks like there are notifications that need to be made, but that the
	         * statd.notify service isn't running.  Let's try to start it up.
	         */
		log(LOG_INFO, "need to start statd notify");
		if (statd_notify_is_loaded())
			statd_notify_start();
		else
			statd_notify_load();
	}
	pmap_unset(SM_PROG, SM_VERS);

	if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		log(LOG_ERR, "can't create UDP socket: %s (%d)", strerror(errno), errno);
		exit(1);
	}
	inetaddr.sin_family = AF_INET;
	inetaddr.sin_addr.s_addr = INADDR_ANY;
	inetaddr.sin_port = htons(config.port);
	inetaddr.sin_len = sizeof(inetaddr);
	if (bindresvport_sa(sockfd, (struct sockaddr *) & inetaddr) < 0) {
		log(LOG_ERR, "can't bind UDP addr: %s (%d)", strerror(errno), errno);
		exit(1);
	}
	socklen = sizeof(inetaddr);
	if (getsockname(sockfd, (struct sockaddr *) & inetaddr, &socklen))
		log(LOG_ERR, "can't getsockname on UDP socket: %s (%d)", strerror(errno), errno);
	else
		udpport = ntohs(inetaddr.sin_port);
	transp = svcudp_create(sockfd);
	if (transp == NULL)
		errx(1, "cannot create UDP service");
	if (!svc_register(transp, SM_PROG, SM_VERS, sm_prog_1, IPPROTO_UDP))
		errx(1, "unable to register (SM_PROG, SM_VERS, UDP)");

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		log(LOG_ERR, "can't create TCP socket: %s (%d)", strerror(errno), errno);
		exit(1);
	}
	inetaddr.sin_family = AF_INET;
	inetaddr.sin_addr.s_addr = INADDR_ANY;
	inetaddr.sin_port = htons(config.port);
	inetaddr.sin_len = sizeof(inetaddr);
	if (bindresvport_sa(sockfd, (struct sockaddr *) & inetaddr) < 0) {
		log(LOG_ERR, "can't bind TCP addr: %s (%d)", strerror(errno), errno);
		exit(1);
	}
	socklen = sizeof(inetaddr);
	if (getsockname(sockfd, (struct sockaddr *) & inetaddr, &socklen))
		log(LOG_ERR, "can't getsockname on TCP socket: %s (%d)", strerror(errno), errno);
	else
		tcpport = ntohs(inetaddr.sin_port);
	transp = svctcp_create(sockfd, 0, 0);
	if (transp == NULL)
		errx(1, "cannot create TCP service");
	if (!svc_register(transp, SM_PROG, SM_VERS, sm_prog_1, IPPROTO_TCP))
		errx(1, "unable to register (SM_PROG, SM_VERS, TCP)");

	/* Install signal handler to collect exit status of child processes	 */
	sa.sa_handler = handle_sigchld;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGCHLD);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	svc_run();		/* Should never return */
	exit(1);
}

static void
usage(void)
{
	fprintf(stderr, "usage: rpc.statd [-d] [ -n | -l | -L | -N hostname ]\n");
	exit(1);
}

/*
 * get the PID of the running statd
 */
pid_t
get_statd_pid(void)
{
	char pidbuf[128], *pidend;
	int fd, len, rv;
	pid_t pid;
	struct flock lock;

	if ((fd = open(_PATH_STATD_PID, O_RDONLY)) < 0) {
		DEBUG(9, "%s: %s (%d)", _PATH_STATD_PID, strerror(errno), errno);
		return (0);
	}
	len = sizeof(pidbuf) - 1;
	if ((len = read(fd, pidbuf, len)) < 0) {
		DEBUG(9, "%s: %s (%d)", _PATH_STATD_PID, strerror(errno), errno);
		return (0);
	}
	/* parse PID */
	pidbuf[len] = '\0';
	pid = strtol(pidbuf, &pidend, 10);
	if (!len || (pid < 1)) {
		DEBUG(1, "%s: bogus pid: %s", _PATH_STATD_PID, pidbuf);
		return (0);
	}
	/* check for lock on file by PID */
	lock.l_type = F_RDLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	rv = fcntl(fd, F_GETLK, &lock);
	close(fd);
	if (rv != 0) {
		DEBUG(1, "%s: fcntl: %s (%d)", _PATH_STATD_PID, strerror(errno), errno);
		return (0);
	} else if (lock.l_type == F_UNLCK) {
		DEBUG(8, "%s: not locked\n", _PATH_STATD_PID);
		return (0);
	}
	return (pid);
}

/*
 * get the PID of the running statd.notify
 */
pid_t
get_statd_notify_pid(void)
{
	char pidbuf[128], *pidend;
	int fd, len, rv;
	pid_t pid;
	struct flock lock;

	if ((fd = open(_PATH_STATD_NOTIFY_PID, O_RDONLY)) < 0) {
		DEBUG(9, "%s: %s (%d)", _PATH_STATD_NOTIFY_PID, strerror(errno), errno);
		return (0);
	}
	len = sizeof(pidbuf) - 1;
	if ((len = read(fd, pidbuf, len)) < 0) {
		DEBUG(9, "%s: %s (%d)", _PATH_STATD_NOTIFY_PID, strerror(errno), errno);
		return (0);
	}
	/* parse PID */
	pidbuf[len] = '\0';
	pid = strtol(pidbuf, &pidend, 10);
	if (!len || (pid < 1)) {
		DEBUG(1, "%s: bogus pid: %s", _PATH_STATD_NOTIFY_PID, pidbuf);
		return (0);
	}
	/* check for lock on file by PID */
	lock.l_type = F_RDLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	rv = fcntl(fd, F_GETLK, &lock);
	close(fd);
	if (rv != 0) {
		DEBUG(1, "%s: fcntl: %s (%d)", _PATH_STATD_NOTIFY_PID, strerror(errno), errno);
		return (0);
	} else if (lock.l_type == F_UNLCK) {
		DEBUG(8, "%s: not locked\n", _PATH_STATD_NOTIFY_PID);
		return (0);
	}
	return (pid);
}


/* handle_sigchld ---------------------------------------------------------- */
/*
   Purpose:	Catch SIGCHLD and collect process status
   Returns:	Nothing.
   Notes:	No special action required, other than to collect the
		process status and hence allow the child to die:
		we only use child processes for asynchronous transmission
		of SM_NOTIFY to other systems, so it is normal for the
		children to exit when they have done their work.
*/

static void 
handle_sigchld(int sig __unused)
{
	int pid, status;
	pid = wait4(-1, &status, WNOHANG, (struct rusage *) 0);
	if (!pid)
		log(LOG_ERR, "Phantom SIGCHLD??");
	else if (status == 0)
		DEBUG(2, "Child %d exited OK", pid);
	else
		log(LOG_ERR, "Child %d failed with status %d", pid, WEXITSTATUS(status));
}

/* cleanup ------------------------------------------------------ */
/*
   Purpose:	call pid file cleanup function on signal
   Returns:	Nothing
*/

static void
cleanup(int sig)
{
	if (statd_server) {
		/* update state to "down" on our way out */
		status_info->fh_state = htonl(ntohl(status_info->fh_state) + 1);
		sync_file();
		/* unregister statd service */
		alarm(1); /* XXX 5028243 in case pmap_unset() gets hung up during shutdown */
		pmap_unset(SM_PROG, SM_VERS);
	}
	pidfile_remove(pfh);
	exit((sig == SIGTERM) ? 0 : 1);
}


/*
 * read the statd values from nfs.conf
 */
static int
config_read(struct nfs_conf_statd * conf)
{
	FILE *f;
	size_t len, linenum = 0;
	char *line, *p, *key, *value;
	long val;

	if (!(f = fopen(_PATH_NFS_CONF, "r"))) {
		if (errno != ENOENT)
			log(LOG_WARNING, "%s", _PATH_NFS_CONF);
		return (1);
	}
	for (; (line = fparseln(f, &len, &linenum, NULL, 0)); free(line)) {
		if (len <= 0)
			continue;
		/* trim trailing whitespace */
		p = line + len - 1;
		while ((p > line) && isspace(*p))
			*p-- = '\0';
		/* find key start */
		key = line;
		while (isspace(*key))
			key++;
		/* find equals/value */
		value = p = strchr(line, '=');
		if (p)		/* trim trailing whitespace on key */
			do { *p-- = '\0'; } while ((p > line) && isspace(*p));
		/* find value start */
		if (value)
			do { value++; } while (isspace(*value));

		/* all statd keys start with "nfs.statd." */
		if (strncmp(key, "nfs.statd.", 10)) {
			DEBUG(4, "%4ld %s=%s\n", linenum, key, value ? value : "");
			continue;
		}
		val = !value ? 1 : strtol(value, NULL, 0);
		DEBUG(1, "%4ld %s=%s (%d)\n", linenum, key, value ? value : "", val);

		if (!strcmp(key, "nfs.statd.port")) {
			conf->port = val;
		} else if (!strcmp(key, "nfs.statd.simu_crash_allowed")) {
			conf->simu_crash_allowed = val;
		} else if (!strcmp(key, "nfs.statd.verbose")) {
			conf->verbose = val;
		} else {
			DEBUG(2, "ignoring unknown config value: %4ld %s=%s\n", linenum, key, value ? value : "");
		}

	}

	fclose(f);
	return (0);
}

/*
 * run an external program
 */
static int
safe_exec(char *const argv[], int silent)
{
	posix_spawn_file_actions_t psfileact, *psfileactp = NULL;
	pid_t pid;
	int status;
	extern char **environ;

	if (silent) {
		psfileactp = &psfileact;
		if (posix_spawn_file_actions_init(psfileactp)) {
			log(LOG_ERR, "spawn init of %s failed: %s (%d)", argv[0], strerror(errno), errno);
			return (1);
		}
		posix_spawn_file_actions_addopen(psfileactp, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		posix_spawn_file_actions_addopen(psfileactp, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(psfileactp, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	}
	status = posix_spawn(&pid, argv[0], psfileactp, NULL, argv, environ);
	if (psfileactp)
		posix_spawn_file_actions_destroy(psfileactp);
	if (status) {
		log(LOG_ERR, "spawn of %s failed: %s (%d)", argv[0], strerror(errno), errno);
		return (1);
	}
	while ((waitpid(pid, &status, 0) == -1) && (errno == EINTR))
		usleep(1000);
	if (WIFSIGNALED(status)) {
		log(LOG_ERR, "%s aborted by signal %d", argv[0], WTERMSIG(status));
		return (1);
	} else if (WIFSTOPPED(status)) {
		log(LOG_ERR, "%s stopped by signal %d ?", argv[0], WSTOPSIG(status));
		return (1);
	} else if (WEXITSTATUS(status) && !silent) {
		log(LOG_ERR, "%s exited with status %d", argv[0], WEXITSTATUS(status));
	}
	return (WEXITSTATUS(status));
}

int
statd_notify_load(void)
{
	/*
         * XXX Sorry, but it's just so much simpler to exec launchctl than to
         * try to read the plist file, convert it to launchd data, and submit
         * it ourselves.
         */
	const char *const args[] = {_PATH_LAUNCHCTL, "load", _PATH_STATD_NOTIFY_PLIST, NULL};
	return safe_exec((char *const *) args, 1);
}

int
statd_notify_is_loaded(void)
{
	launch_data_t msg, resp;
	int rv = 0;

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	if (!msg)
		return (0);
	launch_data_dict_insert(msg, launch_data_new_string(_STATD_NOTIFY_SERVICE_LABEL), LAUNCH_KEY_GETJOB);

	resp = launch_msg(msg);
	if (resp) {
		if (launch_data_get_type(resp) == LAUNCH_DATA_DICTIONARY)
			rv = 1;
		launch_data_free(resp);
	} else {
		log(LOG_ERR, "launch_msg(): %m");
	}

	launch_data_free(msg);
	return (rv);
}

int
statd_notify_start(void)
{
	launch_data_t msg, resp;
	int rv = 1;

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	if (!msg)
		return (1);
	launch_data_dict_insert(msg, launch_data_new_string(_STATD_NOTIFY_SERVICE_LABEL), LAUNCH_KEY_STARTJOB);

	resp = launch_msg(msg);
	if (!resp) {
		rv = errno;
	} else {
		if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO)
			rv = launch_data_get_errno(resp);
		launch_data_free(resp);
	}

	launch_data_free(msg);
	return (rv);
}

/*
 * our own little logging function...
 */
void
SYSLOG(int pri, const char *fmt,...)
{
	va_list ap;

	if (pri > LOG_LEVEL)
		return;

	va_start(ap, fmt);
	if (log_to_stderr) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		fflush(stderr);
	} else {
		vsyslog(pri, fmt, ap);
	}
	va_end(ap);
}
