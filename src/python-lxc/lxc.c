/*
 * python-lxc: Python bindings for LXC
 *
 * (C) Copyright Canonical Ltd. 2012-2013
 *
 * Authors:
 * Stéphane Graber <stgraber@ubuntu.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <Python.h>
#include "structmember.h"
#include <lxc/lxccontainer.h>
#include <lxc/utils.h>
#include <lxc/namespace.h>
#include <lxc/confile.h>
#include <stdio.h>
#include <sys/wait.h>

typedef struct {
    PyObject_HEAD
    struct lxc_container *container;
} Container;

char**
convert_tuple_to_char_pointer_array(PyObject *argv) {
    int argc;
    int i, j;

    /* not a list or tuple */
    if (!PyList_Check(argv) && !PyTuple_Check(argv)) {
        PyErr_SetString(PyExc_TypeError, "Expected list or tuple.");
        return NULL;
    }

    argc = PySequence_Fast_GET_SIZE(argv);

    char **result = (char**) calloc(argc + 1, sizeof(char*));

    if (result == NULL) {
        PyErr_SetNone(PyExc_MemoryError);
        return NULL;
    }

    for (i = 0; i < argc; i++) {
        PyObject *pyobj = PySequence_Fast_GET_ITEM(argv, i);
        assert(pyobj != NULL);

        char *str = NULL;
        PyObject *pystr = NULL;

        if (!PyUnicode_Check(pyobj)) {
            PyErr_SetString(PyExc_ValueError, "Expected a string");
            goto error;
        }

        pystr = PyUnicode_AsUTF8String(pyobj);
        if (!pystr) {
            /* Maybe it wasn't UTF-8 encoded.  An exception is already set. */
            goto error;
        }

        str = PyBytes_AsString(pystr);
        if (!str) {
            /* Maybe pystr wasn't a valid object. An exception is already set.
             */
            Py_DECREF(pystr);
            goto error;
        }

        /* We must make a copy of str, because it points into internal memory
         * which we do not own.  Assume it's NULL terminated, otherwise we'd
         * have to use PyUnicode_AsUTF8AndSize() and be explicit about copying
         * the memory.
         */
        result[i] = strdup(str);

        /* Do not decref pyobj since we stole a reference by using
         * PyTuple_GET_ITEM().
         */
        Py_DECREF(pystr);
        if (result[i] == NULL) {
            PyErr_SetNone(PyExc_MemoryError);
            goto error;
        }
    }

    result[argc] = NULL;
    return result;

error:
    /* We can only iterate up to but not including i because malloc() does not
     * initialize its memory.  Thus if we got here, i points to the index
     * after the last strdup'd entry in result.
     */
    for (j = 0; j < i; j++)
        free(result[j]);
    free(result);
    return NULL;
}

static void
Container_dealloc(Container* self)
{
    lxc_container_put(self->container);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
Container_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Container *self;

    self = (Container *)type->tp_alloc(type, 0);

    return (PyObject *)self;
}

static int
Container_init(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"name", "config_path", NULL};
    char *name = NULL;
    PyObject *fs_config_path = NULL;
    char *config_path = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|O&", kwlist,
                                      &name,
                                      PyUnicode_FSConverter, &fs_config_path))
        return -1;

    if (fs_config_path != NULL) {
        config_path = PyBytes_AS_STRING(fs_config_path);
        assert(config_path != NULL);
    }

    self->container = lxc_container_new(name, config_path);
    if (!self->container) {
        Py_XDECREF(fs_config_path);
        fprintf(stderr, "%d: error creating container %s\n", __LINE__, name);
        return -1;
    }

    Py_XDECREF(fs_config_path);
    return 0;
}

static PyObject *
LXC_get_default_config_path(PyObject *self, PyObject *args)
{
    return PyUnicode_FromString(lxc_get_default_config_path());
}

static PyObject *
LXC_get_version(PyObject *self, PyObject *args)
{
    return PyUnicode_FromString(lxc_get_version());
}

// Container properties
static PyObject *
Container_config_file_name(Container *self, void *closure)
{
    return PyUnicode_FromString(
                self->container->config_file_name(self->container));
}

static PyObject *
Container_defined(Container *self, void *closure)
{
    if (self->container->is_defined(self->container)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_init_pid(Container *self, void *closure)
{
    return PyLong_FromLong(self->container->init_pid(self->container));
}

static PyObject *
Container_name(Container *self, void *closure)
{
    return PyUnicode_FromString(self->container->name);
}

static PyObject *
Container_running(Container *self, void *closure)
{
    if (self->container->is_running(self->container)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_state(Container *self, void *closure)
{
    return PyUnicode_FromString(self->container->state(self->container));
}

// Container Functions
static PyObject *
Container_clear_config_item(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"key", NULL};
    char *key = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist,
                                      &key))
        return NULL;

    if (self->container->clear_config_item(self->container, key)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_create(Container *self, PyObject *args, PyObject *kwds)
{
    char* template_name = NULL;
    char** create_args = {NULL};
    PyObject *retval = NULL, *vargs = NULL;
    int i = 0;
    static char *kwlist[] = {"template", "args", NULL};

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s|O", kwlist,
                                      &template_name, &vargs))
        return NULL;

    if (vargs) {
        if (PyTuple_Check(vargs)) {
            create_args = convert_tuple_to_char_pointer_array(vargs);
            if (!create_args) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_ValueError, "args needs to be a tuple");
            return NULL;
        }
    }

    if (self->container->create(self->container, template_name, NULL, NULL, 0, create_args))
        retval = Py_True;
    else
        retval = Py_False;

    if (vargs) {
        /* We cannot have gotten here unless vargs was given and create_args
         * was successfully allocated.
         */
        for (i = 0; i < PyTuple_GET_SIZE(vargs); i++)
            free(create_args[i]);
        free(create_args);
    }

    Py_INCREF(retval);
    return retval;
}

static PyObject *
Container_destroy(Container *self, PyObject *args, PyObject *kwds)
{
    if (self->container->destroy(self->container)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_freeze(Container *self, PyObject *args, PyObject *kwds)
{
    if (self->container->freeze(self->container)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_get_cgroup_item(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"key", NULL};
    char* key = NULL;
    int len = 0;
    PyObject *ret = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist,
                                      &key))
        return NULL;

    len = self->container->get_cgroup_item(self->container, key, NULL, 0);

    if (len < 0) {
        PyErr_SetString(PyExc_KeyError, "Invalid cgroup entry");
        return NULL;
    }

    char* value = (char*) malloc(sizeof(char)*len + 1);
    if (value == NULL)
        return PyErr_NoMemory();

    if (self->container->get_cgroup_item(self->container,
                                            key, value, len + 1) != len) {
        PyErr_SetString(PyExc_ValueError, "Unable to read config value");
        free(value);
        return NULL;
    }

    ret = PyUnicode_FromString(value);
    free(value);
    return ret;
}

static PyObject *
Container_get_config_item(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"key", NULL};
    char* key = NULL;
    int len = 0;
    PyObject *ret = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s|", kwlist,
                                      &key))
        return NULL;

    len = self->container->get_config_item(self->container, key, NULL, 0);

    if (len < 0) {
        PyErr_SetString(PyExc_KeyError, "Invalid configuration key");
        return NULL;
    }

    char* value = (char*) malloc(sizeof(char)*len + 1);
    if (value == NULL)
        return PyErr_NoMemory();

    if (self->container->get_config_item(self->container,
                                            key, value, len + 1) != len) {
        PyErr_SetString(PyExc_ValueError, "Unable to read config value");
        free(value);
        return NULL;
    }

    ret = PyUnicode_FromString(value);
    free(value);
    return ret;
}

static PyObject *
Container_get_config_path(Container *self, PyObject *args, PyObject *kwds)
{
    return PyUnicode_FromString(
                self->container->get_config_path(self->container));
}

static PyObject *
Container_get_keys(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"key", NULL};
    char* key = NULL;
    int len = 0;
    PyObject *ret = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|s", kwlist,
                                      &key))
        return NULL;

    len = self->container->get_keys(self->container, key, NULL, 0);

    if (len < 0) {
        PyErr_SetString(PyExc_KeyError, "Invalid configuration key");
        return NULL;
    }

    char* value = (char*) malloc(sizeof(char)*len + 1);
    if (value == NULL)
        return PyErr_NoMemory();

    if (self->container->get_keys(self->container,
                                    key, value, len + 1) != len) {
        PyErr_SetString(PyExc_ValueError, "Unable to read config keys");
        free(value);
        return NULL;
    }

    ret = PyUnicode_FromString(value);
    free(value);
    return ret;
}

static PyObject *
Container_get_interfaces(Container *self)
{
    int i = 0;
    char** interfaces = NULL;

    PyObject* ret;

    /* Get the interfaces */
    interfaces = self->container->get_interfaces(self->container);
    if (!interfaces)
        return PyTuple_New(0);

    /* Count the entries */
    while (interfaces[i])
        i++;

    /* Create the new tuple */
    ret = PyTuple_New(i);
    if (!ret)
        return NULL;

    /* Add the entries to the tuple and free the memory */
    i = 0;
    while (interfaces[i]) {
        PyObject *unicode = PyUnicode_FromString(interfaces[i]);
        if (!unicode) {
            Py_DECREF(ret);
            ret = NULL;
            break;
        }
        PyTuple_SET_ITEM(ret, i, unicode);
        i++;
    }

    /* Free the list of IPs */
    i = 0;
    while (interfaces[i]) {
        free(interfaces[i]);
        i++;
    }
    free(interfaces);

    return ret;
}

static PyObject *
Container_get_ips(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"interface", "family", "scope", NULL};
    char* interface = NULL;
    char* family = NULL;
    int scope = 0;

    int i = 0;
    char** ips = NULL;

    PyObject* ret;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|ssi", kwlist,
                                      &interface, &family, &scope))
        return NULL;

    /* Get the IPs */
    ips = self->container->get_ips(self->container, interface, family, scope);
    if (!ips)
        return PyTuple_New(0);

    /* Count the entries */
    while (ips[i])
        i++;

    /* Create the new tuple */
    ret = PyTuple_New(i);
    if (!ret)
        return NULL;

    /* Add the entries to the tuple and free the memory */
    i = 0;
    while (ips[i]) {
        PyObject *unicode = PyUnicode_FromString(ips[i]);
        if (!unicode) {
            Py_DECREF(ret);
            ret = NULL;
            break;
        }
        PyTuple_SET_ITEM(ret, i, unicode);
        i++;
    }

    /* Free the list of IPs */
    i = 0;
    while (ips[i]) {
        free(ips[i]);
        i++;
    }
    free(ips);

    return ret;
}

static PyObject *
Container_load_config(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"path", NULL};
    PyObject *fs_path = NULL;
    char* path = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|O&", kwlist,
                                      PyUnicode_FSConverter, &fs_path))
        return NULL;

    if (fs_path != NULL) {
        path = PyBytes_AS_STRING(fs_path);
        assert(path != NULL);
    }

    if (self->container->load_config(self->container, path)) {
        Py_XDECREF(fs_path);
        Py_RETURN_TRUE;
    }

    Py_XDECREF(fs_path);
    Py_RETURN_FALSE;
}

static PyObject *
Container_save_config(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"path", NULL};
    PyObject *fs_path = NULL;
    char* path = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|O&", kwlist,
                                      PyUnicode_FSConverter, &fs_path))
        return NULL;

    if (fs_path != NULL) {
        path = PyBytes_AS_STRING(fs_path);
        assert(path != NULL);
    }

    if (self->container->save_config(self->container, path)) {
        Py_XDECREF(fs_path);
        Py_RETURN_TRUE;
    }

    Py_XDECREF(fs_path);
    Py_RETURN_FALSE;
}

static PyObject *
Container_set_cgroup_item(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"key", "value", NULL};
    char *key = NULL;
    char *value = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "ss", kwlist,
                                      &key, &value))
        return NULL;

    if (self->container->set_cgroup_item(self->container, key, value)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_set_config_item(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"key", "value", NULL};
    char *key = NULL;
    char *value = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "ss", kwlist,
                                      &key, &value))
        return NULL;

    if (self->container->set_config_item(self->container, key, value)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_set_config_path(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"path", NULL};
    char *path = NULL;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist,
                                      &path))
        return NULL;

    if (self->container->set_config_path(self->container, path)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_shutdown(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"timeout", NULL};
    int timeout = -1;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwlist,
                                      &timeout))
        return NULL;

    if (self->container->shutdown(self->container, timeout)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_start(Container *self, PyObject *args, PyObject *kwds)
{
    char** init_args = {NULL};
    PyObject *useinit = NULL, *retval = NULL, *vargs = NULL;
    int init_useinit = 0, i = 0;
    static char *kwlist[] = {"useinit", "cmd", NULL};

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|OO", kwlist,
                                      &useinit, &vargs))
        return NULL;

    if (useinit && useinit == Py_True) {
        init_useinit = 1;
    }

    if (vargs && PyTuple_Check(vargs)) {
        init_args = convert_tuple_to_char_pointer_array(vargs);
        if (!init_args) {
            return NULL;
        }
    }

    self->container->want_daemonize(self->container);

    if (self->container->start(self->container, init_useinit, init_args))
        retval = Py_True;
    else
        retval = Py_False;

    if (vargs) {
        /* We cannot have gotten here unless vargs was given and create_args
         * was successfully allocated.
         */
        for (i = 0; i < PyTuple_GET_SIZE(vargs); i++)
            free(init_args[i]);
        free(init_args);
    }

    Py_INCREF(retval);
    return retval;
}

static PyObject *
Container_stop(Container *self, PyObject *args, PyObject *kwds)
{
    if (self->container->stop(self->container)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_unfreeze(Container *self, PyObject *args, PyObject *kwds)
{
    if (self->container->unfreeze(self->container)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject *
Container_console(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"ttynum", "stdinfd", "stdoutfd", "stderrfd", "escape", NULL};
    int ttynum = -1, stdinfd = 0, stdoutfd = 1, stderrfd = 2, escape = 1;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|iiiii", kwlist,
                                      &ttynum, &stdinfd, &stdoutfd, &stderrfd,
                                      &escape))
        return NULL;

    if (self->container->console(self->container, ttynum,
				 stdinfd, stdoutfd, stderrfd, escape) == 0) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *
Container_console_getfd(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"ttynum", NULL};
    int ttynum = -1, masterfd;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwlist, &ttynum))
        return NULL;

    if (self->container->console_getfd(self->container, &ttynum, &masterfd) < 0) {
        PyErr_SetString(PyExc_ValueError, "Unable to allocate tty");
        return NULL;
    }
    return PyLong_FromLong(masterfd);
}

static PyObject *
Container_wait(Container *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"state", "timeout", NULL};
    char *state = NULL;
    int timeout = -1;

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "s|i", kwlist,
                                      &state, &timeout))
        return NULL;

    if (self->container->wait(self->container, state, timeout)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

struct lxc_attach_python_payload {
    PyObject *fn;
    PyObject *arg;
};

static int lxc_attach_python_exec(void* _payload)
{
    struct lxc_attach_python_payload *payload = (struct lxc_attach_python_payload *)_payload;
    PyObject *result = PyObject_CallFunctionObjArgs(payload->fn, payload->arg, NULL);

    if (!result) {
        PyErr_Print();
        return -1;
    }
    if (PyLong_Check(result))
        return (int)PyLong_AsLong(result);
    else
        return -1;
}

static void lxc_attach_free_options(lxc_attach_options_t *options);

static lxc_attach_options_t *lxc_attach_parse_options(PyObject *kwds)
{
    static char *kwlist[] = {"attach_flags", "namespaces", "personality", "initial_cwd", "uid", "gid", "env_policy", "extra_env_vars", "extra_keep_env", "stdin", "stdout", "stderr", NULL};
    long temp_uid, temp_gid;
    int temp_env_policy;
    PyObject *extra_env_vars_obj = NULL;
    PyObject *extra_keep_env_obj = NULL;
    PyObject *stdin_obj = NULL;
    PyObject *stdout_obj = NULL;
    PyObject *stderr_obj = NULL;
    PyObject *initial_cwd_obj = NULL;
    PyObject *dummy;
    bool parse_result;

    lxc_attach_options_t default_options = LXC_ATTACH_OPTIONS_DEFAULT;
    lxc_attach_options_t *options = malloc(sizeof(*options));

    if (!options) {
        PyErr_SetNone(PyExc_MemoryError);
        return NULL;
    }
    memcpy(options, &default_options, sizeof(*options));

    /* we need some dummy variables because we can't be sure
     * the data types match completely */
    temp_uid = -1;
    temp_gid = -1;
    temp_env_policy = options->env_policy;

    /* we need a dummy tuple */
    dummy = PyTuple_New(0);

    parse_result = PyArg_ParseTupleAndKeywords(dummy, kwds, "|iilO&lliOOOOO", kwlist,
                                               &options->attach_flags, &options->namespaces, &options->personality,
                                               PyUnicode_FSConverter, &initial_cwd_obj, &temp_uid, &temp_gid,
                                               &temp_env_policy, &extra_env_vars_obj, &extra_keep_env_obj,
                                               &stdin_obj, &stdout_obj, &stderr_obj);

    /* immediately get rid of the dummy tuple */
    Py_DECREF(dummy);

    if (!parse_result) {
        lxc_attach_free_options(options);
        return NULL;
    }

    /* duplicate the string, so we don't depend on some random Python object */
    if (initial_cwd_obj != NULL) {
        options->initial_cwd = strndup(PyBytes_AsString(initial_cwd_obj), PyBytes_Size(initial_cwd_obj));
        Py_DECREF(initial_cwd_obj);
    }

    /* do the type conversion from the types that match the parse string */
    if (temp_uid != -1) options->uid = (uid_t)temp_uid;
    if (temp_gid != -1) options->gid = (gid_t)temp_gid;
    options->env_policy = (lxc_attach_env_policy_t)temp_env_policy;

    if (extra_env_vars_obj)
        options->extra_env_vars = convert_tuple_to_char_pointer_array(extra_env_vars_obj);
    if (extra_keep_env_obj)
        options->extra_keep_env = convert_tuple_to_char_pointer_array(extra_keep_env_obj);
    if (stdin_obj) {
        options->stdin_fd = PyObject_AsFileDescriptor(stdin_obj);
        if (options->stdin_fd < 0) {
            lxc_attach_free_options(options);
            return NULL;
        }
    }
    if (stdout_obj) {
        options->stdout_fd = PyObject_AsFileDescriptor(stdout_obj);
        if (options->stdout_fd < 0) {
            lxc_attach_free_options(options);
            return NULL;
        }
    }
    if (stderr_obj) {
        options->stderr_fd = PyObject_AsFileDescriptor(stderr_obj);
        if (options->stderr_fd < 0) {
            lxc_attach_free_options(options);
            return NULL;
        }
    }

    return options;
}

void lxc_attach_free_options(lxc_attach_options_t *options)
{
    int i;
    if (!options)
        return;
    if (options->initial_cwd)
        free(options->initial_cwd);
    if (options->extra_env_vars) {
        for (i = 0; options->extra_env_vars[i]; i++)
            free(options->extra_env_vars[i]);
        free(options->extra_env_vars);
    }
    if (options->extra_keep_env) {
        for (i = 0; options->extra_keep_env[i]; i++)
            free(options->extra_keep_env[i]);
        free(options->extra_keep_env);
    }
    free(options);
}

static PyObject *
Container_attach_and_possibly_wait(Container *self, PyObject *args, PyObject *kwds, int wait)
{
    struct lxc_attach_python_payload payload = { NULL, NULL };
    lxc_attach_options_t *options = NULL;
    long ret;
    pid_t pid;

    if (!PyArg_ParseTuple(args, "O|O", &payload.fn, &payload.arg))
        return NULL;
    if (!PyCallable_Check(payload.fn)) {
        PyErr_Format(PyExc_TypeError, "attach: object not callable");
        return NULL;
    }

    options = lxc_attach_parse_options(kwds);
    if (!options)
        return NULL;

    ret = self->container->attach(self->container, lxc_attach_python_exec, &payload, options, &pid);
    if (ret < 0)
        goto out;

    if (wait) {
        ret = lxc_wait_for_pid_status(pid);
        /* handle case where attach fails */
        if (WIFEXITED(ret) && WEXITSTATUS(ret) == 255)
            ret = -1;
    } else {
        ret = (long)pid;
    }

out:
    lxc_attach_free_options(options);
    return PyLong_FromLong(ret);
}

static PyObject *
Container_attach(Container *self, PyObject *args, PyObject *kwds)
{
    return Container_attach_and_possibly_wait(self, args, kwds, 0);
}

static PyObject *
Container_attach_wait(Container *self, PyObject *args, PyObject *kwds)
{
    return Container_attach_and_possibly_wait(self, args, kwds, 1);
}

static PyObject *
LXC_attach_run_shell(PyObject *self, PyObject *arg)
{
    int rv;

    rv = lxc_attach_run_shell(NULL);

    return PyLong_FromLong(rv);
}

static PyObject *
LXC_arch_to_personality(PyObject *self, PyObject *arg)
{
    long rv = -1;
    PyObject *pystr;
    char *str;

    if (!PyUnicode_Check(arg)) {
        PyErr_SetString(PyExc_ValueError, "Expected a string");
        return NULL;
    }

    pystr = PyUnicode_AsUTF8String(arg);
    if (!pystr)
        return NULL;

    str = PyBytes_AsString(pystr);
    if (!str)
        goto out;

    rv = lxc_config_parse_arch(str);
    if (rv == -1)
        PyErr_SetString(PyExc_KeyError, "Failed to lookup architecture.");

out:
    Py_DECREF(pystr);
    return rv == -1 ? NULL : PyLong_FromLong(rv);
}

static PyObject *
LXC_attach_run_command(PyObject *self, PyObject *arg)
{
    PyObject *args_obj = NULL;
    int i, rv;
    lxc_attach_command_t cmd = {
        NULL,         /* program */
        NULL          /* argv[] */
    };

    if (!PyArg_ParseTuple(arg, "sO", (const char**)&cmd.program, &args_obj))
        return NULL;
    if (args_obj && PyList_Check(args_obj)) {
        cmd.argv = convert_tuple_to_char_pointer_array(args_obj);
    } else {
        PyErr_Format(PyExc_TypeError, "Second part of tuple passed to attach_run_command must be a list.");
        return NULL;
    }

    if (!cmd.argv)
        return NULL;

    rv = lxc_attach_run_command(&cmd);

    for (i = 0; cmd.argv[i]; i++)
        free(cmd.argv[i]);
    free(cmd.argv);

    return PyLong_FromLong(rv);
}

static PyGetSetDef Container_getseters[] = {
    {"config_file_name",
     (getter)Container_config_file_name, NULL,
     "Path to the container configuration",
     NULL},
    {"defined",
     (getter)Container_defined, NULL,
     "Boolean indicating whether the container configuration exists",
     NULL},
    {"init_pid",
     (getter)Container_init_pid, NULL,
     "PID of the container's init process in the host's PID namespace",
     NULL},
    {"name",
     (getter)Container_name, NULL,
     "Container name",
     NULL},
    {"running",
     (getter)Container_running, NULL,
     "Boolean indicating whether the container is running or not",
     NULL},
    {"state",
     (getter)Container_state, NULL,
     "Container state",
     NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef Container_methods[] = {
    {"clear_config_item", (PyCFunction)Container_clear_config_item,
     METH_VARARGS|METH_KEYWORDS,
     "clear_config_item(key) -> boolean\n"
     "\n"
     "Clear the current value of a config key."
    },
    {"create", (PyCFunction)Container_create,
     METH_VARARGS|METH_KEYWORDS,
     "create(template, args = (,)) -> boolean\n"
     "\n"
     "Create a new rootfs for the container, using the given template "
     "and passing some optional arguments to it."
    },
    {"destroy", (PyCFunction)Container_destroy,
     METH_NOARGS,
     "destroy() -> boolean\n"
     "\n"
     "Destroys the container."
    },
    {"freeze", (PyCFunction)Container_freeze,
     METH_NOARGS,
     "freeze() -> boolean\n"
     "\n"
     "Freezes the container and returns its return code."
    },
    {"get_cgroup_item", (PyCFunction)Container_get_cgroup_item,
     METH_VARARGS|METH_KEYWORDS,
     "get_cgroup_item(key) -> string\n"
     "\n"
     "Get the current value of a cgroup entry."
    },
    {"get_config_item", (PyCFunction)Container_get_config_item,
     METH_VARARGS|METH_KEYWORDS,
     "get_config_item(key) -> string\n"
     "\n"
     "Get the current value of a config key."
    },
    {"get_config_path", (PyCFunction)Container_get_config_path,
     METH_NOARGS,
     "get_config_path() -> string\n"
     "\n"
     "Return the LXC config path (where the containers are stored)."
    },
    {"get_keys", (PyCFunction)Container_get_keys,
     METH_VARARGS|METH_KEYWORDS,
     "get_keys(key) -> string\n"
     "\n"
     "Get a list of valid sub-keys for a key."
    },
    {"get_interfaces", (PyCFunction)Container_get_interfaces,
     METH_NOARGS,
     "get_interface() -> tuple\n"
     "\n"
     "Get a tuple of interfaces for the container."
    },
    {"get_ips", (PyCFunction)Container_get_ips,
     METH_VARARGS|METH_KEYWORDS,
     "get_ips(interface, family, scope) -> tuple\n"
     "\n"
     "Get a tuple of IPs for the container."
    },
    {"load_config", (PyCFunction)Container_load_config,
     METH_VARARGS|METH_KEYWORDS,
     "load_config(path = DEFAULT) -> boolean\n"
     "\n"
     "Read the container configuration from its default "
     "location or from an alternative location if provided."
    },
    {"save_config", (PyCFunction)Container_save_config,
     METH_VARARGS|METH_KEYWORDS,
     "save_config(path = DEFAULT) -> boolean\n"
     "\n"
     "Save the container configuration to its default "
     "location or to an alternative location if provided."
    },
    {"set_cgroup_item", (PyCFunction)Container_set_cgroup_item,
     METH_VARARGS|METH_KEYWORDS,
     "set_cgroup_item(key, value) -> boolean\n"
     "\n"
     "Set a cgroup entry to the provided value."
    },
    {"set_config_item", (PyCFunction)Container_set_config_item,
     METH_VARARGS|METH_KEYWORDS,
     "set_config_item(key, value) -> boolean\n"
     "\n"
     "Set a config key to the provided value."
    },
    {"set_config_path", (PyCFunction)Container_set_config_path,
     METH_VARARGS|METH_KEYWORDS,
     "set_config_path(path) -> boolean\n"
     "\n"
     "Set the LXC config path (where the containers are stored)."
    },
    {"shutdown", (PyCFunction)Container_shutdown,
     METH_VARARGS|METH_KEYWORDS,
     "shutdown(timeout = -1) -> boolean\n"
     "\n"
     "Sends SIGPWR to the container and wait for it to shutdown "
     "unless timeout is set to a positive value, in which case "
     "the container will be killed when the timeout is reached."
    },
    {"start", (PyCFunction)Container_start,
     METH_VARARGS|METH_KEYWORDS,
     "start(useinit = False, cmd = (,)) -> boolean\n"
     "\n"
     "Start the container, optionally using lxc-init and "
     "an alternate init command, then returns its return code."
    },
    {"stop", (PyCFunction)Container_stop,
     METH_NOARGS,
     "stop() -> boolean\n"
     "\n"
     "Stop the container and returns its return code."
    },
    {"unfreeze", (PyCFunction)Container_unfreeze,
     METH_NOARGS,
     "unfreeze() -> boolean\n"
     "\n"
     "Unfreezes the container and returns its return code."
    },
    {"wait", (PyCFunction)Container_wait,
     METH_VARARGS|METH_KEYWORDS,
     "wait(state, timeout = -1) -> boolean\n"
     "\n"
     "Wait for the container to reach a given state or timeout."
    },
    {"console", (PyCFunction)Container_console,
     METH_VARARGS|METH_KEYWORDS,
     "console(ttynum = -1, stdinfd = 0, stdoutfd = 1, stderrfd = 2, escape = 0) -> boolean\n"
     "\n"
     "Attach to container's console."
    },
    {"console_getfd", (PyCFunction)Container_console_getfd,
     METH_VARARGS|METH_KEYWORDS,
     "console(ttynum = -1) -> boolean\n"
     "\n"
     "Attach to container's console."
    },
    {"attach", (PyCFunction)Container_attach,
     METH_VARARGS|METH_KEYWORDS,
     "attach(run, payload) -> int\n"
     "\n"
     "Attach to the container. Returns the pid of the attached process."
    },
    {"attach_wait", (PyCFunction)Container_attach_wait,
     METH_VARARGS|METH_KEYWORDS,
     "attach(run, payload) -> int\n"
     "\n"
     "Attach to the container. Returns the exit code of the process."
    },
    {NULL, NULL, 0, NULL}
};

static PyTypeObject _lxc_ContainerType = {
PyVarObject_HEAD_INIT(NULL, 0)
    "lxc.Container",                /* tp_name */
    sizeof(Container),              /* tp_basicsize */
    0,                              /* tp_itemsize */
    (destructor)Container_dealloc,  /* tp_dealloc */
    0,                              /* tp_print */
    0,                              /* tp_getattr */
    0,                              /* tp_setattr */
    0,                              /* tp_reserved */
    0,                              /* tp_repr */
    0,                              /* tp_as_number */
    0,                              /* tp_as_sequence */
    0,                              /* tp_as_mapping */
    0,                              /* tp_hash  */
    0,                              /* tp_call */
    0,                              /* tp_str */
    0,                              /* tp_getattro */
    0,                              /* tp_setattro */
    0,                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT |
        Py_TPFLAGS_BASETYPE,        /* tp_flags */
    "Container objects",            /* tp_doc */
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    0,                              /* tp_iter */
    0,                              /* tp_iternext */
    Container_methods,              /* tp_methods */
    0,                              /* tp_members */
    Container_getseters,            /* tp_getset */
    0,                              /* tp_base */
    0,                              /* tp_dict */
    0,                              /* tp_descr_get */
    0,                              /* tp_descr_set */
    0,                              /* tp_dictoffset */
    (initproc)Container_init,       /* tp_init */
    0,                              /* tp_alloc */
    Container_new,                  /* tp_new */
};

static PyMethodDef LXC_methods[] = {
    {"attach_run_shell", (PyCFunction)LXC_attach_run_shell, METH_O,
     "Starts up a shell when attaching, to use as the run parameter for attach or attach_wait"},
    {"attach_run_command", (PyCFunction)LXC_attach_run_command, METH_O,
     "Runs a command when attaching, to use as the run parameter for attach or attach_wait"},
    {"arch_to_personality", (PyCFunction)LXC_arch_to_personality, METH_O,
     "Returns the process personality of the corresponding architecture"},
    {"get_default_config_path", (PyCFunction)LXC_get_default_config_path, METH_NOARGS,
     "Returns the current LXC config path"},
    {"get_version", (PyCFunction)LXC_get_version, METH_NOARGS,
     "Returns the current LXC library version"},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef _lxcmodule = {
    PyModuleDef_HEAD_INIT,
    "_lxc",
    "Binding for liblxc in python",
    -1,
    LXC_methods
};

PyMODINIT_FUNC
PyInit__lxc(void)
{
    PyObject* m;
    PyObject* d;

    if (PyType_Ready(&_lxc_ContainerType) < 0)
        return NULL;

    m = PyModule_Create(&_lxcmodule);
    if (m == NULL)
        return NULL;

    Py_INCREF(&_lxc_ContainerType);
    PyModule_AddObject(m, "Container", (PyObject *)&_lxc_ContainerType);

    /* add constants */
    d = PyModule_GetDict(m);

    #define PYLXC_EXPORT_CONST(c) PyDict_SetItemString(d, #c, PyLong_FromLong(c))

    /* environment variable handling */
    PYLXC_EXPORT_CONST(LXC_ATTACH_KEEP_ENV);
    PYLXC_EXPORT_CONST(LXC_ATTACH_CLEAR_ENV);

    /* attach options */
    PYLXC_EXPORT_CONST(LXC_ATTACH_MOVE_TO_CGROUP);
    PYLXC_EXPORT_CONST(LXC_ATTACH_DROP_CAPABILITIES);
    PYLXC_EXPORT_CONST(LXC_ATTACH_SET_PERSONALITY);
    PYLXC_EXPORT_CONST(LXC_ATTACH_LSM_NOW);
    PYLXC_EXPORT_CONST(LXC_ATTACH_LSM_EXEC);
    PYLXC_EXPORT_CONST(LXC_ATTACH_REMOUNT_PROC_SYS);
    PYLXC_EXPORT_CONST(LXC_ATTACH_DEFAULT);

    /* namespace flags (no other python lib exports this) */
    PYLXC_EXPORT_CONST(CLONE_NEWUTS);
    PYLXC_EXPORT_CONST(CLONE_NEWIPC);
    PYLXC_EXPORT_CONST(CLONE_NEWUSER);
    PYLXC_EXPORT_CONST(CLONE_NEWPID);
    PYLXC_EXPORT_CONST(CLONE_NEWNET);
    PYLXC_EXPORT_CONST(CLONE_NEWNS);

    #undef PYLXC_EXPORT_CONST

    return m;
}

/*
 * kate: space-indent on; indent-width 4; mixedindent off; indent-mode cstyle;
 */
