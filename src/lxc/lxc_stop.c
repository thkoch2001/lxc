/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <stdio.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/types.h>

#include <lxc/lxc.h>
#include <lxc/log.h>

#include <lxc/lxccontainer.h>
#include "arguments.h"
#include "commands.h"
#include "utils.h"

static int my_parser(struct lxc_arguments* args, int c, char* arg)
{
	switch (c) {
	case 'r': args->reboot = 1; break;
	case 'W': args->nowait = 1; break;
	case 't': args->timeout = atoi(arg); break;
	case 'k': args->hardstop = 1; break;
	case 's': args->shutdown = 1; break;
	}
	return 0;
}

static const struct option my_longopts[] = {
	{"reboot", no_argument, 0, 'r'},
	{"nowait", no_argument, 0, 'W'},
	{"timeout", required_argument, 0, 't'},
	{"kill", no_argument, 0, 'k'},
	{"shutdown", no_argument, 0, 's'},
	LXC_COMMON_OPTIONS
};

static struct lxc_arguments my_args = {
	.progname = "lxc-stop",
	.help     = "\
--name=NAME\n\
\n\
lxc-stop stops a container with the identifier NAME\n\
\n\
Options :\n\
  -n, --name=NAME   NAME for name of the container\n\
  -r, --reboot      reboot the container\n\
  -W, --nowait      don't wait for shutdown or reboot to complete\n\
  -t, --timeout=T   wait T seconds before hard-stopping\n\
  -k, --kill        kill container rather than request clean shutdown\n\
  -s, --shutdown    Only request clean shutdown, don't later force kill\n",
	.options  = my_longopts,
	.parser   = my_parser,
	.checker  = NULL,
	.timeout = 60,
};

/* returns -1 on failure, 0 on success */
int do_reboot_and_check(struct lxc_arguments *a, struct lxc_container *c)
{
	int ret;
	pid_t pid;
	pid_t newpid;
	int timeout = a->timeout;

	pid = c->init_pid(c);
	if (pid == -1)
		return -1;
	if (!c->reboot(c))
		return -1;
	if (a->nowait)
		return 0;
	if (timeout <= 0)
		goto out;

	for (;;) {
		/* can we use c-> wait for this, assuming it will
		 * re-enter RUNNING?  For now just sleep */
		int elapsed_time, curtime = 0;
		struct timeval tv;

		newpid = c->init_pid(c);
		if (newpid != -1 && newpid != pid)
			return 0;

		ret = gettimeofday(&tv, NULL);
		if (ret)
			break;
		curtime = tv.tv_sec;

		sleep(1);
		ret = gettimeofday(&tv, NULL);
		if (ret)
			break;
		elapsed_time = tv.tv_sec - curtime;
		if (timeout - elapsed_time <= 0)
			break;
		timeout -= elapsed_time;
	}

out:
	newpid = c->init_pid(c);
	if (newpid == -1 || newpid == pid) {
		printf("Reboot did not complete before timeout\n");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct lxc_container *c;
	bool s;
	int ret = 1;

	if (lxc_arguments_parse(&my_args, argc, argv))
		return 1;

	if (lxc_log_init(my_args.name, my_args.log_file, my_args.log_priority,
			 my_args.progname, my_args.quiet, my_args.lxcpath[0]))
		return 1;

	c = lxc_container_new(my_args.name, my_args.lxcpath[0]);
	if (!c) {
		fprintf(stderr, "Error opening container\n");
		goto out;
	}

	if (!c->is_running(c)) {
		fprintf(stderr, "%s is not running\n", c->name);
		ret = 2;
		goto out;
	}

	if (my_args.hardstop) {
		ret = c->stop(c) ? 0 : 1;
		goto out;
	}
	if (my_args.reboot) {
		ret = do_reboot_and_check(&my_args, c);
		goto out;
	}

	s = c->shutdown(c, my_args.timeout);
	if (!s) {
		if (!my_args.shutdown)
			ret = c->wait(c, "STOPPED", -1) ? 0 : 1;
		else
			ret = 1; // fail
	} else
		ret = 0;

out:
	lxc_container_put(c);
	return ret;
}
