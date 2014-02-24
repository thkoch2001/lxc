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
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <net/if.h>

#include "error.h"
#include "commands.h"
#include "list.h"
#include "conf.h"
#include "utils.h"
#include "bdev.h"
#include "log.h"
#include "cgroup.h"
#include "start.h"
#include "state.h"

#ifdef HAVE_CGMANAGER
lxc_log_define(lxc_cgmanager, lxc);

static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

static void lock_mutex(pthread_mutex_t *l)
{
	int ret;

	if ((ret = pthread_mutex_lock(l)) != 0) {
		fprintf(stderr, "pthread_mutex_lock returned:%d %s\n", ret, strerror(ret));
		exit(1);
	}
}

static void unlock_mutex(pthread_mutex_t *l)
{
	int ret;

	if ((ret = pthread_mutex_unlock(l)) != 0) {
		fprintf(stderr, "pthread_mutex_unlock returned:%d %s\n", ret, strerror(ret));
		exit(1);
	}
}

#include <nih-dbus/dbus_connection.h>
#include <cgmanager/cgmanager-client.h>
#include <nih/alloc.h>
#include <nih/error.h>
#include <nih/string.h>

struct cgm_data {
	char *name;
	char *cgroup_path;
	const char *cgroup_pattern;
};

static NihDBusProxy *cgroup_manager = NULL;
static struct cgroup_ops cgmanager_ops;
static int nr_subsystems;
static char **subsystems;

#define CGMANAGER_DBUS_SOCK "unix:path=/sys/fs/cgroup/cgmanager/sock"
static void cgm_dbus_disconnected(DBusConnection *connection);
static bool cgm_dbus_connect(void)
{
	DBusError dbus_error;
	DBusConnection *connection;
	dbus_error_init(&dbus_error);

	lock_mutex(&thread_mutex);
	connection = nih_dbus_connect(CGMANAGER_DBUS_SOCK, cgm_dbus_disconnected);
	if (!connection) {
		NihError *nerr;
		nerr = nih_error_get();
		DEBUG("Unable to open cgmanager connection at %s: %s", CGMANAGER_DBUS_SOCK,
			nerr->message);
		nih_free(nerr);
		dbus_error_free(&dbus_error);
		unlock_mutex(&thread_mutex);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	dbus_error_free(&dbus_error);
	cgroup_manager = nih_dbus_proxy_new(NULL, connection,
				NULL /* p2p */,
				"/org/linuxcontainers/cgmanager", NULL, NULL);
	dbus_connection_unref(connection);
	if (!cgroup_manager) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("Error opening cgmanager proxy: %s", nerr->message);
		nih_free(nerr);
		unlock_mutex(&thread_mutex);
		return false;
	}

	// force fd passing negotiation
	if (cgmanager_ping_sync(NULL, cgroup_manager, 0) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("Error pinging cgroup manager: %s", nerr->message);
		nih_free(nerr);
	}
	unlock_mutex(&thread_mutex);
	return true;
}

static void cgm_dbus_disconnect(void)
{
	lock_mutex(&thread_mutex);
	if (cgroup_manager)
		nih_free(cgroup_manager);
	cgroup_manager = NULL;
	unlock_mutex(&thread_mutex);
}

static void cgm_dbus_disconnected(DBusConnection *connection)
{
	WARN("Cgroup manager connection was terminated");
	cgroup_manager = NULL;
	if (cgm_dbus_connect()) {
		INFO("New cgroup manager connection was opened");
	} else {
		WARN("Cgroup manager unable to re-open connection");
	}
}

static int send_creds(int sock, int rpid, int ruid, int rgid)
{
	struct msghdr msg = { 0 };
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct ucred cred = {
		.pid = rpid,
		.uid = ruid,
		.gid = rgid,
	};
	char cmsgbuf[CMSG_SPACE(sizeof(cred))];
	char buf[1];
	buf[0] = 'p';

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;
	memcpy(CMSG_DATA(cmsg), &cred, sizeof(cred));

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(sock, &msg, 0) < 0)
		return -1;
	return 0;
}

static bool lxc_cgmanager_create(const char *controller, const char *cgroup_path, int32_t *existed)
{
	lock_mutex(&thread_mutex);
	if ( cgmanager_create_sync(NULL, cgroup_manager, controller,
				       cgroup_path, existed) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_create_sync failed: %s", nerr->message);
		nih_free(nerr);
		ERROR("Failed to create %s:%s", controller, cgroup_path);
		unlock_mutex(&thread_mutex);
		return false;
	}

	unlock_mutex(&thread_mutex);
	return true;
}

static bool lxc_cgmanager_escape(void)
{
	pid_t me = getpid();
	int i;
	lock_mutex(&thread_mutex);
	for (i = 0; i < nr_subsystems; i++) {
		if (cgmanager_move_pid_abs_sync(NULL, cgroup_manager,
					subsystems[i], "/", me) != 0) {
			NihError *nerr;
			nerr = nih_error_get();
			ERROR("call to cgmanager_move_pid_abs_sync(%s) failed: %s",
					subsystems[i], nerr->message);
			nih_free(nerr);
			unlock_mutex(&thread_mutex);
			return false;
		}
	}

	unlock_mutex(&thread_mutex);
	return true;
}

struct chown_data {
	const char *controller;
	const char *cgroup_path;
	uid_t origuid;
};

static int do_chown_cgroup(const char *controller, const char *cgroup_path,
		uid_t origuid)
{
	int sv[2] = {-1, -1}, optval = 1, ret = -1;
	char buf[1];

	uid_t caller_nsuid = get_ns_uid(origuid);

	lock_mutex(&thread_mutex);
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		SYSERROR("Error creating socketpair");
		goto out;
	}
	if (setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		SYSERROR("setsockopt failed");
		goto out;
	}
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		SYSERROR("setsockopt failed");
		goto out;
	}
	if ( cgmanager_chown_scm_sync(NULL, cgroup_manager, controller,
				       cgroup_path, sv[1]) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_chown_scm_sync failed: %s", nerr->message);
		nih_free(nerr);
		goto out;
	}
	/* now send credentials */

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(sv[0], &rfds);
	if (select(sv[0]+1, &rfds, NULL, NULL, NULL) < 0) {
		ERROR("Error getting go-ahead from server: %s", strerror(errno));
		goto out;
	}
	if (read(sv[0], &buf, 1) != 1) {
		ERROR("Error getting reply from server over socketpair");
		goto out;
	}
	if (send_creds(sv[0], getpid(), getuid(), getgid())) {
		SYSERROR("%s: Error sending pid over SCM_CREDENTIAL", __func__);
		goto out;
	}
	FD_ZERO(&rfds);
	FD_SET(sv[0], &rfds);
	if (select(sv[0]+1, &rfds, NULL, NULL, NULL) < 0) {
		ERROR("Error getting go-ahead from server: %s", strerror(errno));
		goto out;
	}
	if (read(sv[0], &buf, 1) != 1) {
		ERROR("Error getting reply from server over socketpair");
		goto out;
	}
	if (send_creds(sv[0], getpid(), caller_nsuid, 0)) {
		SYSERROR("%s: Error sending pid over SCM_CREDENTIAL", __func__);
		goto out;
	}
	FD_ZERO(&rfds);
	FD_SET(sv[0], &rfds);
	if (select(sv[0]+1, &rfds, NULL, NULL, NULL) < 0) {
		ERROR("Error getting go-ahead from server: %s", strerror(errno));
		goto out;
	}
	ret = read(sv[0], buf, 1);
out:
	close(sv[0]);
	close(sv[1]);
	unlock_mutex(&thread_mutex);
	if (ret == 1 && *buf == '1')
		return 0;
	return -1;
}

static int chown_cgroup_wrapper(void *data)
{
	struct chown_data *arg = data;

	if (setresgid(0,0,0) < 0)
		SYSERROR("Failed to setgid to 0");
	if (setresuid(0,0,0) < 0)
		SYSERROR("Failed to setuid to 0");
	if (setgroups(0, NULL) < 0)
		SYSERROR("Failed to clear groups");
	return do_chown_cgroup(arg->controller, arg->cgroup_path, arg->origuid);
}

static bool lxc_cgmanager_chmod(const char *controller,
		const char *cgroup_path, const char *file, int mode)
{
	lock_mutex(&thread_mutex);
	if (cgmanager_chmod_sync(NULL, cgroup_manager, controller,
			cgroup_path, file, mode) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_chmod_sync failed: %s", nerr->message);
		nih_free(nerr);
		unlock_mutex(&thread_mutex);
		return false;
	}
	unlock_mutex(&thread_mutex);
	return true;
}

static bool chown_cgroup(const char *controller, const char *cgroup_path,
			struct lxc_conf *conf)
{
	struct chown_data data;

	if (lxc_list_empty(&conf->id_map))
		/* If there's no mapping then we don't need to chown */
		return true;

	data.controller = controller;
	data.cgroup_path = cgroup_path;
	data.origuid = geteuid();

	if (userns_exec_1(conf, chown_cgroup_wrapper, &data) < 0) {
		ERROR("Error requesting cgroup chown in new namespace");
		return false;
	}

	/* now chmod 775 the directory else the container cannot create cgroups */
	if (!lxc_cgmanager_chmod(controller, cgroup_path, "", 0775))
		return false;
	if (!lxc_cgmanager_chmod(controller, cgroup_path, "tasks", 0775))
		return false;
	if (!lxc_cgmanager_chmod(controller, cgroup_path, "cgroup.procs", 0775))
		return false;
	return true;
}

#define CG_REMOVE_RECURSIVE 1
static void cgm_remove_cgroup(const char *controller, const char *path)
{
	int existed;
	lock_mutex(&thread_mutex);
	if ( cgmanager_remove_sync(NULL, cgroup_manager, controller,
				   path, CG_REMOVE_RECURSIVE, &existed) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_remove_sync failed: %s", nerr->message);
		nih_free(nerr);
		unlock_mutex(&thread_mutex);
		ERROR("Error removing %s:%s", controller, path);
	}
	unlock_mutex(&thread_mutex);
	if (existed == -1)
		INFO("cgroup removal attempt: %s:%s did not exist", controller, path);
}

static void *cgm_init(const char *name)
{
	struct cgm_data *d;

	d = malloc(sizeof(*d));
	if (!d)
		return NULL;

	memset(d, 0, sizeof(*d));
	d->name = strdup(name);
	if (!d->name)
		goto err1;

	/* if we are running as root, use system cgroup pattern, otherwise
	 * just create a cgroup under the current one. But also fall back to
	 * that if for some reason reading the configuration fails and no
	 * default value is available
	 */
	if (geteuid() == 0)
		d->cgroup_pattern = lxc_global_config_value("lxc.cgroup.pattern");
	if (!d->cgroup_pattern)
		d->cgroup_pattern = "%n";
	return d;

err1:
	free(d);
	return NULL;
}

static void cgm_destroy(void *hdata)
{
	struct cgm_data *d = hdata;
	int i;

	if (!d)
		return;
	for (i = 0; i < nr_subsystems; i++)
		cgm_remove_cgroup(subsystems[i], d->cgroup_path);

	free(d->name);
	if (d->cgroup_path)
		free(d->cgroup_path);
	free(d);
}

/*
 * remove all the cgroups created
 */
static inline void cleanup_cgroups(char *path)
{
	int i;
	for (i = 0; i < nr_subsystems; i++)
		cgm_remove_cgroup(subsystems[i], path);
}

static inline bool cgm_create(void *hdata)
{
	struct cgm_data *d = hdata;
	int i, index=0, baselen, ret;
	int32_t existed;
	char result[MAXPATHLEN], *tmp, *cgroup_path;

	if (!d)
		return false;
// XXX we should send a hint to the cgmanager that when these
// cgroups become empty they should be deleted.  Requires a cgmanager
// extension

	memset(result, 0, MAXPATHLEN);
	tmp = lxc_string_replace("%n", d->name, d->cgroup_pattern);
	if (!tmp)
		return false;
	if (strlen(tmp) >= MAXPATHLEN) {
		free(tmp);
		return false;
	}
	strcpy(result, tmp);
	baselen = strlen(result);
	free(tmp);
	tmp = result;
	while (*tmp == '/')
		tmp++;
again:
	if (index == 100) { // turn this into a warn later
		ERROR("cgroup error?  100 cgroups with this name already running");
		return false;
	}
	if (index) {
		ret = snprintf(result+baselen, MAXPATHLEN-baselen, "-%d", index);
		if (ret < 0 || ret >= MAXPATHLEN-baselen)
			return false;
	}
	existed = 0;
	for (i = 0; i < nr_subsystems; i++) {
		if (!lxc_cgmanager_create(subsystems[i], tmp, &existed)) {
			ERROR("Error creating cgroup %s:%s", subsystems[i], result);
			cleanup_cgroups(tmp);
			return false;
		}
		if (existed == 1)
			goto next;
	}
	// success
	cgroup_path = strdup(tmp);
	if (!cgroup_path) {
		cleanup_cgroups(tmp);
		return false;
	}
	d->cgroup_path = cgroup_path;
	return true;
next:
	cleanup_cgroups(tmp);
	index++;
	goto again;
}

/*
 * Use the cgmanager to move a task into a cgroup for a particular
 * hierarchy.
 * All the subsystems in this hierarchy are co-mounted, so we only
 * need to transition the task into one of the cgroups
 */
static bool lxc_cgmanager_enter(pid_t pid, const char *controller,
		const char *cgroup_path)
{
	lock_mutex(&thread_mutex);
	if (cgmanager_move_pid_sync(NULL, cgroup_manager, controller,
			cgroup_path, pid) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_move_pid_sync failed: %s", nerr->message);
		nih_free(nerr);
		unlock_mutex(&thread_mutex);
		return false;
	}
	unlock_mutex(&thread_mutex);
	return true;
}

static bool do_cgm_enter(pid_t pid, const char *cgroup_path)
{
	int i;

	for (i = 0; i < nr_subsystems; i++) {
		if (!lxc_cgmanager_enter(pid, subsystems[i], cgroup_path))
			return false;
	}
	return true;
}

static inline bool cgm_enter(void *hdata, pid_t pid)
{
	struct cgm_data *d = hdata;

	if (!d || !d->cgroup_path)
		return false;
	return do_cgm_enter(pid, d->cgroup_path);
}

static const char *cgm_get_cgroup(void *hdata, const char *subsystem)
{
	struct cgm_data *d = hdata;

	if (!d || !d->cgroup_path)
		return NULL;
	return d->cgroup_path;
}

static int cgm_get_nrtasks(void *hdata)
{
	struct cgm_data *d = hdata;
	int32_t *pids;
	size_t pids_len;

	if (!d || !d->cgroup_path)
		return false;

	lock_mutex(&thread_mutex);
	if (cgmanager_get_tasks_sync(NULL, cgroup_manager, subsystems[0],
				     d->cgroup_path, &pids, &pids_len) != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_get_tasks_sync failed: %s", nerr->message);
		nih_free(nerr);
		unlock_mutex(&thread_mutex);
		return -1;
	}
	unlock_mutex(&thread_mutex);
	nih_free(pids);
	return pids_len;
}

static int cgm_get(const char *filename, char *value, size_t len, const char *name, const char *lxcpath)
{
	char *result, *controller, *key, *cgroup;
	size_t newlen;

	controller = alloca(strlen(filename)+1);
	strcpy(controller, filename);
	key = strchr(controller, '.');
	if (!key)
		return -1;
	*key = '\0';

	/* use the command interface to look for the cgroup */
	cgroup = lxc_cmd_get_cgroup_path(name, lxcpath, controller);
	if (!cgroup)
		return -1;
	lock_mutex(&thread_mutex);
	if (cgmanager_get_value_sync(NULL, cgroup_manager, controller, cgroup, filename, &result) != 0) {
		/*
		 * must consume the nih error
		 * However don't print out an error as the key may simply not exist
		 * on the host
		 */
		NihError *nerr;
		nerr = nih_error_get();
		nih_free(nerr);
		free(cgroup);
		unlock_mutex(&thread_mutex);
		return -1;
	}
	unlock_mutex(&thread_mutex);
	free(cgroup);
	newlen = strlen(result);
	if (!value) {
		// user queries the size
		nih_free(result);
		return newlen+1;
	}

	strncpy(value, result, len);
	if (newlen >= len) {
		value[len-1] = '\0';
		newlen = len-1;
	} else if (newlen+1 < len) {
		// cgmanager doesn't add eol to last entry
		value[newlen++] = '\n';
		value[newlen] = '\0';
	}
	nih_free(result);
	return newlen;
}

static int cgm_do_set(const char *controller, const char *file,
			 const char *cgroup, const char *value)
{
	int ret;
	lock_mutex(&thread_mutex);
	ret = cgmanager_set_value_sync(NULL, cgroup_manager, controller,
				 cgroup, file, value);
	if (ret != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_remove_sync failed: %s", nerr->message);
		nih_free(nerr);
		ERROR("Error setting cgroup %s limit %s", file, cgroup);
	}
	unlock_mutex(&thread_mutex);
	return ret;
}

static int cgm_set(const char *filename, const char *value, const char *name, const char *lxcpath)
{
	char *controller, *key, *cgroup;
	int ret;

	controller = alloca(strlen(filename)+1);
	strcpy(controller, filename);
	key = strchr(controller, '.');
	if (!key)
		return -1;
	*key = '\0';

	/* use the command interface to look for the cgroup */
	cgroup = lxc_cmd_get_cgroup_path(name, lxcpath, controller);
	if (!cgroup) {
		ERROR("Failed to get cgroup for controller %s for %s:%s",
			controller, lxcpath, name);
		return -1;
	}
	ret = cgm_do_set(controller, filename, cgroup, value);
	free(cgroup);
	return ret;
}

static void free_subsystems(void)
{
	int i;

	lock_mutex(&thread_mutex);
	for (i = 0; i < nr_subsystems; i++)
		free(subsystems[i]);
	free(subsystems);
	subsystems = NULL;
	nr_subsystems = 0;
	unlock_mutex(&thread_mutex);
}

static bool collect_subsytems(void)
{
	char *line = NULL, *tab1;
	size_t sz = 0;
	FILE *f;

	if (subsystems) // already initialized
		return true;

	lock_mutex(&thread_mutex);
	f = fopen_cloexec("/proc/cgroups", "r");
	if (!f) {
		unlock_mutex(&thread_mutex);
		return false;
	}
	while (getline(&line, &sz, f) != -1) {
		char **tmp;
		if (line[0] == '#')
			continue;
		if (!line[0])
			continue;
		tab1 = strchr(line, '\t');
		if (!tab1)
			continue;
		*tab1 = '\0';
		tmp = realloc(subsystems, (nr_subsystems+1)*sizeof(char *));
		if (!tmp)
			goto out_free;

		subsystems = tmp;
		tmp[nr_subsystems] = strdup(line);
		if (!tmp[nr_subsystems])
			goto out_free;
		nr_subsystems++;
	}
	fclose(f);

	unlock_mutex(&thread_mutex);

	if (!nr_subsystems) {
		ERROR("No cgroup subsystems found");
		return false;
	}

	return true;

out_free:
	fclose(f);
	unlock_mutex(&thread_mutex);
	free_subsystems();
	return false;
}

struct cgroup_ops *cgm_ops_init(void)
{
	if (!collect_subsytems())
		return NULL;
	if (!cgm_dbus_connect())
		goto err1;

	// root;  try to escape to root cgroup
	if (geteuid() == 0 && !lxc_cgmanager_escape())
		goto err2;

	return &cgmanager_ops;

err2:
	cgm_dbus_disconnect();
err1:
	free_subsystems();
	return NULL;
}

static bool cgm_unfreeze(void *hdata)
{
	struct cgm_data *d = hdata;

	if (!d || !d->cgroup_path)
		return false;

	lock_mutex(&thread_mutex);
	if (cgmanager_set_value_sync(NULL, cgroup_manager, "freezer", d->cgroup_path,
			"freezer.state", "THAWED") != 0) {
		NihError *nerr;
		nerr = nih_error_get();
		ERROR("call to cgmanager_set_value_sync failed: %s", nerr->message);
		nih_free(nerr);
		ERROR("Error unfreezing %s", d->cgroup_path);
		unlock_mutex(&thread_mutex);
		return false;
	}
	unlock_mutex(&thread_mutex);
	return true;
}

static bool cgm_setup_limits(void *hdata, struct lxc_list *cgroup_settings, bool do_devices)
{
	struct cgm_data *d = hdata;
	struct lxc_list *iterator;
	struct lxc_cgroup *cg;
	bool ret = false;

	if (lxc_list_empty(cgroup_settings))
		return true;

	if (!d || !d->cgroup_path)
		return false;

	lxc_list_for_each(iterator, cgroup_settings) {
		char controller[100], *p;
		cg = iterator->elem;
		if (do_devices != !strncmp("devices", cg->subsystem, 7))
			continue;
		if (strlen(cg->subsystem) > 100) // i smell a rat
			goto out;
		strcpy(controller, cg->subsystem);
		p = strchr(controller, '.');
		if (p)
			*p = '\0';
		if (cgm_do_set(controller, cg->subsystem, d->cgroup_path
				, cg->value) < 0) {
			ERROR("Error setting %s to %s for %s",
			      cg->subsystem, cg->value, d->name);
			goto out;
		}

		DEBUG("cgroup '%s' set to '%s'", cg->subsystem, cg->value);
	}

	ret = true;
	INFO("cgroup limits have been setup");
out:
	return ret;
}

static bool cgm_chown(void *hdata, struct lxc_conf *conf)
{
	struct cgm_data *d = hdata;
	int i;

	if (!d || !d->cgroup_path)
		return false;
	for (i = 0; i < nr_subsystems; i++) {
		if (!chown_cgroup(subsystems[i], d->cgroup_path, conf))
			WARN("Failed to chown %s:%s to container root",
				subsystems[i], d->cgroup_path);
	}
	return true;
}

/*
 * TODO: this should be re-written to use the get_config_item("lxc.id_map")
 * cmd api instead of getting the idmap from c->lxc_conf.  The reason is
 * that the id_maps may be different if the container was started with a
 * -f or -s argument.
 * The reason I'm punting on that is because we'll need to parse the
 * idmap results.
 */
static bool cgm_attach(const char *name, const char *lxcpath, pid_t pid)
{
	bool pass = false;
	char *cgroup = NULL;
	struct lxc_container *c;

	c = lxc_container_new(name, lxcpath);
	if (!c) {
		ERROR("Could not load container %s:%s", lxcpath, name);
		return false;
	}
	if (!collect_subsytems()) {
		ERROR("Error collecting cgroup subsystems");
		goto out;
	}
	// cgm_create makes sure that we have the same cgroup name for all
	// subsystems, so since this is a slow command over the cmd socket,
	// just get the cgroup name for the first one.
	cgroup = lxc_cmd_get_cgroup_path(name, lxcpath, subsystems[0]);
	if (!cgroup) {
		ERROR("Failed to get cgroup for controller %s", subsystems[0]);
		goto out;
	}

	if (!(pass = do_cgm_enter(pid, cgroup)))
		ERROR("Failed to enter group %s", cgroup);

out:
	free(cgroup);
	lxc_container_put(c);
	return pass;
}

static bool cgm_bind_dir(const char *root, const char *dirname)
{
	nih_local char *cgpath = NULL;

	/* /sys should have been mounted by now */
	cgpath = NIH_MUST( nih_strdup(NULL, root) );
	NIH_MUST( nih_strcat(&cgpath, NULL, "/sys/fs/cgroup") );

	if (!dir_exists(cgpath)) {
		ERROR("%s does not exist", cgpath);
		return false;
	}

	/* mount a tmpfs there so we can create subdirs */
	if (mount("cgroup", cgpath, "tmpfs", 0, "size=10000,mode=755")) {
		SYSERROR("Failed to mount tmpfs at %s", cgpath);
		return false;
	}
	NIH_MUST( nih_strcat(&cgpath, NULL, "/cgmanager") );

	if (mkdir(cgpath, 0755) < 0) {
		SYSERROR("Failed to create %s", cgpath);
		return false;
	}

	if (mount(dirname, cgpath, "none", MS_BIND, 0)) {
		SYSERROR("Failed to bind mount %s to %s", dirname, cgpath);
		return false;
	}

	return true;
}

/*
 * cgm_mount_cgroup:
 * If /sys/fs/cgroup/cgmanager.lower/ exists, bind mount that to
 * /sys/fs/cgroup/cgmanager/ in the container.
 * Otherwise, if /sys/fs/cgroup/cgmanager exists, bind mount that.
 * Else do nothing
 */
#define CGMANAGER_LOWER_SOCK "/sys/fs/cgroup/cgmanager.lower"
#define CGMANAGER_UPPER_SOCK "/sys/fs/cgroup/cgmanager"
static bool cgm_mount_cgroup(void *hdata, const char *root, int type)
{
	if (dir_exists(CGMANAGER_LOWER_SOCK))
		return cgm_bind_dir(root, CGMANAGER_LOWER_SOCK);
	if (dir_exists(CGMANAGER_UPPER_SOCK))
		return cgm_bind_dir(root, CGMANAGER_UPPER_SOCK);
	// Host doesn't have cgmanager running?  Then how did we get here?
	return false;
}

static struct cgroup_ops cgmanager_ops = {
	.init = cgm_init,
	.destroy = cgm_destroy,
	.create = cgm_create,
	.enter = cgm_enter,
	.create_legacy = NULL,
	.get_cgroup = cgm_get_cgroup,
	.get = cgm_get,
	.set = cgm_set,
	.unfreeze = cgm_unfreeze,
	.setup_limits = cgm_setup_limits,
	.name = "cgmanager",
	.chown = cgm_chown,
	.attach = cgm_attach,
	.mount_cgroup = cgm_mount_cgroup,
	.nrtasks = cgm_get_nrtasks,
	.disconnect = cgm_dbus_disconnect,
};
#endif
