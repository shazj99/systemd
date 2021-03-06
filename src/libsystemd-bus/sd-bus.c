/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <endian.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/poll.h>
#include <byteswap.h>
#include <sys/mman.h>
#include <pthread.h>

#include "util.h"
#include "macro.h"
#include "strv.h"
#include "set.h"
#include "missing.h"

#include "sd-bus.h"
#include "bus-internal.h"
#include "bus-message.h"
#include "bus-type.h"
#include "bus-socket.h"
#include "bus-kernel.h"
#include "bus-control.h"
#include "bus-introspect.h"
#include "bus-signature.h"
#include "bus-objects.h"
#include "bus-util.h"
#include "bus-container.h"

static int bus_poll(sd_bus *bus, bool need_more, uint64_t timeout_usec);

static void bus_close_fds(sd_bus *b) {
        assert(b);

        if (b->input_fd >= 0)
                close_nointr_nofail(b->input_fd);

        if (b->output_fd >= 0 && b->output_fd != b->input_fd)
                close_nointr_nofail(b->output_fd);

        b->input_fd = b->output_fd = -1;
}

static void bus_node_destroy(sd_bus *b, struct node *n) {
        struct node_callback *c;
        struct node_vtable *v;
        struct node_enumerator *e;

        assert(b);

        if (!n)
                return;

        while (n->child)
                bus_node_destroy(b, n->child);

        while ((c = n->callbacks)) {
                LIST_REMOVE(callbacks, n->callbacks, c);
                free(c);
        }

        while ((v = n->vtables)) {
                LIST_REMOVE(vtables, n->vtables, v);
                free(v->interface);
                free(v);
        }

        while ((e = n->enumerators)) {
                LIST_REMOVE(enumerators, n->enumerators, e);
                free(e);
        }

        if (n->parent)
                LIST_REMOVE(siblings, n->parent->child, n);

        assert_se(hashmap_remove(b->nodes, n->path) == n);
        free(n->path);
        free(n);
}

static void bus_free(sd_bus *b) {
        struct filter_callback *f;
        struct node *n;
        unsigned i;

        assert(b);

        sd_bus_detach_event(b);

        bus_close_fds(b);

        if (b->kdbus_buffer)
                munmap(b->kdbus_buffer, KDBUS_POOL_SIZE);

        free(b->rbuffer);
        free(b->unique_name);
        free(b->auth_buffer);
        free(b->address);
        free(b->kernel);
        free(b->machine);

        free(b->exec_path);
        strv_free(b->exec_argv);

        close_many(b->fds, b->n_fds);
        free(b->fds);

        for (i = 0; i < b->rqueue_size; i++)
                sd_bus_message_unref(b->rqueue[i]);
        free(b->rqueue);

        for (i = 0; i < b->wqueue_size; i++)
                sd_bus_message_unref(b->wqueue[i]);
        free(b->wqueue);

        hashmap_free_free(b->reply_callbacks);
        prioq_free(b->reply_callbacks_prioq);

        while ((f = b->filter_callbacks)) {
                LIST_REMOVE(callbacks, b->filter_callbacks, f);
                free(f);
        }

        bus_match_free(&b->match_callbacks);

        hashmap_free_free(b->vtable_methods);
        hashmap_free_free(b->vtable_properties);

        while ((n = hashmap_first(b->nodes)))
                bus_node_destroy(b, n);

        hashmap_free(b->nodes);

        bus_kernel_flush_memfd(b);

        assert_se(pthread_mutex_destroy(&b->memfd_cache_mutex) == 0);

        free(b);
}

int sd_bus_new(sd_bus **ret) {
        sd_bus *r;

        assert_return(ret, -EINVAL);

        r = new0(sd_bus, 1);
        if (!r)
                return -ENOMEM;

        r->n_ref = REFCNT_INIT;
        r->input_fd = r->output_fd = -1;
        r->message_version = 1;
        r->hello_flags |= KDBUS_HELLO_ACCEPT_FD;
        r->original_pid = getpid();

        assert_se(pthread_mutex_init(&r->memfd_cache_mutex, NULL) == 0);

        /* We guarantee that wqueue always has space for at least one
         * entry */
        r->wqueue = new(sd_bus_message*, 1);
        if (!r->wqueue) {
                free(r);
                return -ENOMEM;
        }

        *ret = r;
        return 0;
}

int sd_bus_set_address(sd_bus *bus, const char *address) {
        char *a;

        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(address, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        a = strdup(address);
        if (!a)
                return -ENOMEM;

        free(bus->address);
        bus->address = a;

        return 0;
}

int sd_bus_set_fd(sd_bus *bus, int input_fd, int output_fd) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(input_fd >= 0, -EINVAL);
        assert_return(output_fd >= 0, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        bus->input_fd = input_fd;
        bus->output_fd = output_fd;
        return 0;
}

int sd_bus_set_exec(sd_bus *bus, const char *path, char *const argv[]) {
        char *p, **a;

        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(path, -EINVAL);
        assert_return(!strv_isempty(argv), -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        p = strdup(path);
        if (!p)
                return -ENOMEM;

        a = strv_copy(argv);
        if (!a) {
                free(p);
                return -ENOMEM;
        }

        free(bus->exec_path);
        strv_free(bus->exec_argv);

        bus->exec_path = p;
        bus->exec_argv = a;

        return 0;
}

int sd_bus_set_bus_client(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        bus->bus_client = !!b;
        return 0;
}

int sd_bus_negotiate_fds(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ACCEPT_FD, b);
        return 0;
}

int sd_bus_negotiate_attach_comm(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_COMM, b);
        return 0;
}

int sd_bus_negotiate_attach_exe(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_EXE, b);
        return 0;
}

int sd_bus_negotiate_attach_cmdline(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_CMDLINE, b);
        return 0;
}

int sd_bus_negotiate_attach_cgroup(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_CGROUP, b);
        return 0;
}

int sd_bus_negotiate_attach_caps(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_CAPS, b);
        return 0;
}

int sd_bus_negotiate_attach_selinux_context(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_SECLABEL, b);
        return 0;
}

int sd_bus_negotiate_attach_audit(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_AUDIT, b);
        return 0;
}

int sd_bus_set_server(sd_bus *bus, int b, sd_id128_t server_id) {
        assert_return(bus, -EINVAL);
        assert_return(b || sd_id128_equal(server_id, SD_ID128_NULL), -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        bus->is_server = !!b;
        bus->server_id = server_id;
        return 0;
}

int sd_bus_set_anonymous(sd_bus *bus, int b) {
        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        bus->anonymous_auth = !!b;
        return 0;
}

static int hello_callback(sd_bus *bus, sd_bus_message *reply, void *userdata) {
        const char *s;
        int r;

        assert(bus);
        assert(bus->state == BUS_HELLO);
        assert(reply);

        r = sd_bus_message_get_errno(reply);
        if (r < 0)
                return r;
        if (r > 0)
                return -r;

        r = sd_bus_message_read(reply, "s", &s);
        if (r < 0)
                return r;

        if (!service_name_is_valid(s) || s[0] != ':')
                return -EBADMSG;

        bus->unique_name = strdup(s);
        if (!bus->unique_name)
                return -ENOMEM;

        bus->state = BUS_RUNNING;

        return 1;
}

static int bus_send_hello(sd_bus *bus) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert(bus);

        if (!bus->bus_client || bus->is_kernel)
                return 0;

        r = sd_bus_message_new_method_call(
                        bus,
                        "org.freedesktop.DBus",
                        "/",
                        "org.freedesktop.DBus",
                        "Hello",
                        &m);
        if (r < 0)
                return r;

        return sd_bus_send_with_reply(bus, m, hello_callback, NULL, 0, &bus->hello_serial);
}

int bus_start_running(sd_bus *bus) {
        assert(bus);

        if (bus->bus_client && !bus->is_kernel) {
                bus->state = BUS_HELLO;
                return 1;
        }

        bus->state = BUS_RUNNING;
        return 1;
}

static int parse_address_key(const char **p, const char *key, char **value) {
        size_t l, n = 0;
        const char *a;
        char *r = NULL;

        assert(p);
        assert(*p);
        assert(value);

        if (key) {
                l = strlen(key);
                if (strncmp(*p, key, l) != 0)
                        return 0;

                if ((*p)[l] != '=')
                        return 0;

                if (*value)
                        return -EINVAL;

                a = *p + l + 1;
        } else
                a = *p;

        while (*a != ';' && *a != ',' && *a != 0) {
                char c, *t;

                if (*a == '%') {
                        int x, y;

                        x = unhexchar(a[1]);
                        if (x < 0) {
                                free(r);
                                return x;
                        }

                        y = unhexchar(a[2]);
                        if (y < 0) {
                                free(r);
                                return y;
                        }

                        c = (char) ((x << 4) | y);
                        a += 3;
                } else {
                        c = *a;
                        a++;
                }

                t = realloc(r, n + 2);
                if (!t) {
                        free(r);
                        return -ENOMEM;
                }

                r = t;
                r[n++] = c;
        }

        if (!r) {
                r = strdup("");
                if (!r)
                        return -ENOMEM;
        } else
                r[n] = 0;

        if (*a == ',')
                a++;

        *p = a;

        free(*value);
        *value = r;

        return 1;
}

static void skip_address_key(const char **p) {
        assert(p);
        assert(*p);

        *p += strcspn(*p, ",");

        if (**p == ',')
                (*p) ++;
}

static int parse_unix_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *path = NULL, *abstract = NULL;
        size_t l;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "path", &path);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "abstract", &abstract);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!path && !abstract)
                return -EINVAL;

        if (path && abstract)
                return -EINVAL;

        if (path) {
                l = strlen(path);
                if (l > sizeof(b->sockaddr.un.sun_path))
                        return -E2BIG;

                b->sockaddr.un.sun_family = AF_UNIX;
                strncpy(b->sockaddr.un.sun_path, path, sizeof(b->sockaddr.un.sun_path));
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + l;
        } else if (abstract) {
                l = strlen(abstract);
                if (l > sizeof(b->sockaddr.un.sun_path) - 1)
                        return -E2BIG;

                b->sockaddr.un.sun_family = AF_UNIX;
                b->sockaddr.un.sun_path[0] = 0;
                strncpy(b->sockaddr.un.sun_path+1, abstract, sizeof(b->sockaddr.un.sun_path)-1);
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + 1 + l;
        }

        return 0;
}

static int parse_tcp_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *host = NULL, *port = NULL, *family = NULL;
        int r;
        struct addrinfo *result, hints = {
                .ai_socktype = SOCK_STREAM,
                .ai_flags = AI_ADDRCONFIG,
        };

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "host", &host);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "port", &port);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "family", &family);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!host || !port)
                return -EINVAL;

        if (family) {
                if (streq(family, "ipv4"))
                        hints.ai_family = AF_INET;
                else if (streq(family, "ipv6"))
                        hints.ai_family = AF_INET6;
                else
                        return -EINVAL;
        }

        r = getaddrinfo(host, port, &hints, &result);
        if (r == EAI_SYSTEM)
                return -errno;
        else if (r != 0)
                return -EADDRNOTAVAIL;

        memcpy(&b->sockaddr, result->ai_addr, result->ai_addrlen);
        b->sockaddr_size = result->ai_addrlen;

        freeaddrinfo(result);

        return 0;
}

static int parse_exec_address(sd_bus *b, const char **p, char **guid) {
        char *path = NULL;
        unsigned n_argv = 0, j;
        char **argv = NULL;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        goto fail;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "path", &path);
                if (r < 0)
                        goto fail;
                else if (r > 0)
                        continue;

                if (startswith(*p, "argv")) {
                        unsigned ul;

                        errno = 0;
                        ul = strtoul(*p + 4, (char**) p, 10);
                        if (errno > 0 || **p != '=' || ul > 256) {
                                r = -EINVAL;
                                goto fail;
                        }

                        (*p) ++;

                        if (ul >= n_argv) {
                                char **x;

                                x = realloc(argv, sizeof(char*) * (ul + 2));
                                if (!x) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                memset(x + n_argv, 0, sizeof(char*) * (ul - n_argv + 2));

                                argv = x;
                                n_argv = ul + 1;
                        }

                        r = parse_address_key(p, NULL, argv + ul);
                        if (r < 0)
                                goto fail;

                        continue;
                }

                skip_address_key(p);
        }

        if (!path) {
                r = -EINVAL;
                goto fail;
        }

        /* Make sure there are no holes in the array, with the
         * exception of argv[0] */
        for (j = 1; j < n_argv; j++)
                if (!argv[j]) {
                        r = -EINVAL;
                        goto fail;
                }

        if (argv && argv[0] == NULL) {
                argv[0] = strdup(path);
                if (!argv[0]) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        b->exec_path = path;
        b->exec_argv = argv;
        return 0;

fail:
        for (j = 0; j < n_argv; j++)
                free(argv[j]);

        free(argv);
        free(path);
        return r;
}

static int parse_kernel_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *path = NULL;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "path", &path);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!path)
                return -EINVAL;

        free(b->kernel);
        b->kernel = path;
        path = NULL;

        return 0;
}

static int parse_container_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *machine = NULL;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "machine", &machine);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!machine)
                return -EINVAL;

        free(b->machine);
        b->machine = machine;
        machine = NULL;

        b->sockaddr.un.sun_family = AF_UNIX;
        strncpy(b->sockaddr.un.sun_path, "/var/run/dbus/system_bus_socket", sizeof(b->sockaddr.un.sun_path));
        b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + sizeof("/var/run/dbus/system_bus_socket") - 1;

        return 0;
}

static void bus_reset_parsed_address(sd_bus *b) {
        assert(b);

        zero(b->sockaddr);
        b->sockaddr_size = 0;
        strv_free(b->exec_argv);
        free(b->exec_path);
        b->exec_path = NULL;
        b->exec_argv = NULL;
        b->server_id = SD_ID128_NULL;
        free(b->kernel);
        b->kernel = NULL;
        free(b->machine);
        b->machine = NULL;
}

static int bus_parse_next_address(sd_bus *b) {
        _cleanup_free_ char *guid = NULL;
        const char *a;
        int r;

        assert(b);

        if (!b->address)
                return 0;
        if (b->address[b->address_index] == 0)
                return 0;

        bus_reset_parsed_address(b);

        a = b->address + b->address_index;

        while (*a != 0) {

                if (*a == ';') {
                        a++;
                        continue;
                }

                if (startswith(a, "unix:")) {
                        a += 5;

                        r = parse_unix_address(b, &a, &guid);
                        if (r < 0)
                                return r;
                        break;

                } else if (startswith(a, "tcp:")) {

                        a += 4;
                        r = parse_tcp_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;

                } else if (startswith(a, "unixexec:")) {

                        a += 9;
                        r = parse_exec_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;

                } else if (startswith(a, "kernel:")) {

                        a += 7;
                        r = parse_kernel_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;
                } else if (startswith(a, "x-container:")) {

                        a += 12;
                        r = parse_container_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;
                }

                a = strchr(a, ';');
                if (!a)
                        return 0;
        }

        if (guid) {
                r = sd_id128_from_string(guid, &b->server_id);
                if (r < 0)
                        return r;
        }

        b->address_index = a - b->address;
        return 1;
}

static int bus_start_address(sd_bus *b) {
        int r;

        assert(b);

        for (;;) {
                sd_bus_close(b);

                if (b->exec_path) {

                        r = bus_socket_exec(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;
                } else if (b->kernel) {

                        r = bus_kernel_connect(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;

                } else if (b->machine) {

                        r = bus_container_connect(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;

                } else if (b->sockaddr.sa.sa_family != AF_UNSPEC) {

                        r = bus_socket_connect(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;
                }

                r = bus_parse_next_address(b);
                if (r < 0)
                        return r;
                if (r == 0)
                        return b->last_connect_error ? -b->last_connect_error : -ECONNREFUSED;
        }
}

int bus_next_address(sd_bus *b) {
        assert(b);

        bus_reset_parsed_address(b);
        return bus_start_address(b);
}

static int bus_start_fd(sd_bus *b) {
        struct stat st;
        int r;

        assert(b);
        assert(b->input_fd >= 0);
        assert(b->output_fd >= 0);

        r = fd_nonblock(b->input_fd, true);
        if (r < 0)
                return r;

        r = fd_cloexec(b->input_fd, true);
        if (r < 0)
                return r;

        if (b->input_fd != b->output_fd) {
                r = fd_nonblock(b->output_fd, true);
                if (r < 0)
                        return r;

                r = fd_cloexec(b->output_fd, true);
                if (r < 0)
                        return r;
        }

        if (fstat(b->input_fd, &st) < 0)
                return -errno;

        if (S_ISCHR(b->input_fd))
                return bus_kernel_take_fd(b);
        else
                return bus_socket_take_fd(b);
}

int sd_bus_start(sd_bus *bus) {
        int r;

        assert_return(bus, -EINVAL);
        assert_return(bus->state == BUS_UNSET, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        bus->state = BUS_OPENING;

        if (bus->is_server && bus->bus_client)
                return -EINVAL;

        if (bus->input_fd >= 0)
                r = bus_start_fd(bus);
        else if (bus->address || bus->sockaddr.sa.sa_family != AF_UNSPEC || bus->exec_path || bus->kernel || bus->machine)
                r = bus_start_address(bus);
        else
                return -EINVAL;

        if (r < 0)
                return r;

        return bus_send_hello(bus);
}

int sd_bus_open_system(sd_bus **ret) {
        const char *e;
        sd_bus *b;
        int r;

        assert_return(ret, -EINVAL);

        r = sd_bus_new(&b);
        if (r < 0)
                return r;

        e = secure_getenv("DBUS_SYSTEM_BUS_ADDRESS");
        if (e) {
                r = sd_bus_set_address(b, e);
                if (r < 0)
                        goto fail;
        } else {
                b->sockaddr.un.sun_family = AF_UNIX;
                strncpy(b->sockaddr.un.sun_path, "/run/dbus/system_bus_socket", sizeof(b->sockaddr.un.sun_path));
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + sizeof("/run/dbus/system_bus_socket") - 1;
        }

        b->bus_client = true;

        r = sd_bus_start(b);
        if (r < 0)
                goto fail;

        *ret = b;
        return 0;

fail:
        bus_free(b);
        return r;
}

int sd_bus_open_user(sd_bus **ret) {
        const char *e;
        sd_bus *b;
        size_t l;
        int r;

        assert_return(ret, -EINVAL);

        r = sd_bus_new(&b);
        if (r < 0)
                return r;

        e = secure_getenv("DBUS_SESSION_BUS_ADDRESS");
        if (e) {
                r = sd_bus_set_address(b, e);
                if (r < 0)
                        goto fail;
        } else {
                e = secure_getenv("XDG_RUNTIME_DIR");
                if (!e) {
                        r = -ENOENT;
                        goto fail;
                }

                l = strlen(e);
                if (l + 4 > sizeof(b->sockaddr.un.sun_path)) {
                        r = -E2BIG;
                        goto fail;
                }

                b->sockaddr.un.sun_family = AF_UNIX;
                memcpy(mempcpy(b->sockaddr.un.sun_path, e, l), "/bus", 4);
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + l + 4;
        }

        b->bus_client = true;

        r = sd_bus_start(b);
        if (r < 0)
                goto fail;

        *ret = b;
        return 0;

fail:
        bus_free(b);
        return r;
}

int sd_bus_open_system_remote(const char *host, sd_bus **ret) {
        _cleanup_free_ char *e = NULL;
        char *p = NULL;
        sd_bus *bus;
        int r;

        assert_return(host, -EINVAL);
        assert_return(ret, -EINVAL);

        e = bus_address_escape(host);
        if (!e)
                return -ENOMEM;

        p = strjoin("unixexec:path=ssh,argv1=-xT,argv2=", e, ",argv3=systemd-stdio-bridge", NULL);
        if (!p)
                return -ENOMEM;

        r = sd_bus_new(&bus);
        if (r < 0) {
                free(p);
                return r;
        }

        bus->address = p;
        bus->bus_client = true;

        r = sd_bus_start(bus);
        if (r < 0) {
                bus_free(bus);
                return r;
        }

        *ret = bus;
        return 0;
}

int sd_bus_open_system_container(const char *machine, sd_bus **ret) {
        _cleanup_free_ char *e = NULL;
        sd_bus *bus;
        char *p;
        int r;

        assert_return(machine, -EINVAL);
        assert_return(ret, -EINVAL);

        e = bus_address_escape(machine);
        if (!e)
                return -ENOMEM;

        p = strjoin("x-container:machine=", e, NULL);
        if (!p)
                return -ENOMEM;

        r = sd_bus_new(&bus);
        if (r < 0) {
                free(p);
                return r;
        }

        bus->address = p;
        bus->bus_client = true;

        r = sd_bus_start(bus);
        if (r < 0) {
                bus_free(bus);
                return r;
        }

        *ret = bus;
        return 0;
}

void sd_bus_close(sd_bus *bus) {
        if (!bus)
                return;
        if (bus->state == BUS_CLOSED)
                return;
        if (bus_pid_changed(bus))
                return;

        bus->state = BUS_CLOSED;

        sd_bus_detach_event(bus);

        if (!bus->is_kernel)
                bus_close_fds(bus);

        /* We'll leave the fd open in case this is a kernel bus, since
         * there might still be memblocks around that reference this
         * bus, and they might need to invoke the
         * KDBUS_CMD_MSG_RELEASE ioctl on the fd when they are
         * freed. */
}

sd_bus *sd_bus_ref(sd_bus *bus) {
        if (!bus)
                return NULL;

        assert_se(REFCNT_INC(bus->n_ref) >= 2);

        return bus;
}

sd_bus *sd_bus_unref(sd_bus *bus) {
        if (!bus)
                return NULL;

        if (REFCNT_DEC(bus->n_ref) <= 0)
                bus_free(bus);

        return NULL;
}

int sd_bus_is_open(sd_bus *bus) {

        assert_return(bus, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        return BUS_IS_OPEN(bus->state);
}

int sd_bus_can_send(sd_bus *bus, char type) {
        int r;

        assert_return(bus, -EINVAL);
        assert_return(bus->state != BUS_UNSET, -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (type == SD_BUS_TYPE_UNIX_FD) {
                if (!(bus->hello_flags & KDBUS_HELLO_ACCEPT_FD))
                        return 0;

                r = bus_ensure_running(bus);
                if (r < 0)
                        return r;

                return bus->can_fds;
        }

        return bus_type_is_valid(type);
}

int sd_bus_get_server_id(sd_bus *bus, sd_id128_t *server_id) {
        int r;

        assert_return(bus, -EINVAL);
        assert_return(server_id, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        r = bus_ensure_running(bus);
        if (r < 0)
                return r;

        *server_id = bus->server_id;
        return 0;
}

static int bus_seal_message(sd_bus *b, sd_bus_message *m) {
        assert(m);

        if (m->header->version > b->message_version)
                return -EPERM;

        if (m->sealed)
                return 0;

        return bus_message_seal(m, ++b->serial);
}

static int dispatch_wqueue(sd_bus *bus) {
        int r, ret = 0;

        assert(bus);
        assert(bus->state == BUS_RUNNING || bus->state == BUS_HELLO);

        while (bus->wqueue_size > 0) {

                if (bus->is_kernel)
                        r = bus_kernel_write_message(bus, bus->wqueue[0]);
                else
                        r = bus_socket_write_message(bus, bus->wqueue[0], &bus->windex);

                if (r < 0) {
                        sd_bus_close(bus);
                        return r;
                } else if (r == 0)
                        /* Didn't do anything this time */
                        return ret;
                else if (bus->is_kernel || bus->windex >= BUS_MESSAGE_SIZE(bus->wqueue[0])) {
                        /* Fully written. Let's drop the entry from
                         * the queue.
                         *
                         * This isn't particularly optimized, but
                         * well, this is supposed to be our worst-case
                         * buffer only, and the socket buffer is
                         * supposed to be our primary buffer, and if
                         * it got full, then all bets are off
                         * anyway. */

                        sd_bus_message_unref(bus->wqueue[0]);
                        bus->wqueue_size --;
                        memmove(bus->wqueue, bus->wqueue + 1, sizeof(sd_bus_message*) * bus->wqueue_size);
                        bus->windex = 0;

                        ret = 1;
                }
        }

        return ret;
}

static int dispatch_rqueue(sd_bus *bus, sd_bus_message **m) {
        sd_bus_message *z = NULL;
        int r, ret = 0;

        assert(bus);
        assert(m);
        assert(bus->state == BUS_RUNNING || bus->state == BUS_HELLO);

        if (bus->rqueue_size > 0) {
                /* Dispatch a queued message */

                *m = bus->rqueue[0];
                bus->rqueue_size --;
                memmove(bus->rqueue, bus->rqueue + 1, sizeof(sd_bus_message*) * bus->rqueue_size);
                return 1;
        }

        /* Try to read a new message */
        do {
                if (bus->is_kernel)
                        r = bus_kernel_read_message(bus, &z);
                else
                        r = bus_socket_read_message(bus, &z);

                if (r < 0) {
                        sd_bus_close(bus);
                        return r;
                }
                if (r == 0)
                        return ret;

                ret = 1;
        } while (!z);

        *m = z;
        return ret;
}

int sd_bus_send(sd_bus *bus, sd_bus_message *m, uint64_t *serial) {
        int r;

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(m, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (m->n_fds > 0) {
                r = sd_bus_can_send(bus, SD_BUS_TYPE_UNIX_FD);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -ENOTSUP;
        }

        /* If the serial number isn't kept, then we know that no reply
         * is expected */
        if (!serial && !m->sealed)
                m->header->flags |= SD_BUS_MESSAGE_NO_REPLY_EXPECTED;

        r = bus_seal_message(bus, m);
        if (r < 0)
                return r;

        /* If this is a reply and no reply was requested, then let's
         * suppress this, if we can */
        if (m->dont_send && !serial)
                return 1;

        if ((bus->state == BUS_RUNNING || bus->state == BUS_HELLO) && bus->wqueue_size <= 0) {
                size_t idx = 0;

                if (bus->is_kernel)
                        r = bus_kernel_write_message(bus, m);
                else
                        r = bus_socket_write_message(bus, m, &idx);

                if (r < 0) {
                        sd_bus_close(bus);
                        return r;
                } else if (!bus->is_kernel && idx < BUS_MESSAGE_SIZE(m))  {
                        /* Wasn't fully written. So let's remember how
                         * much was written. Note that the first entry
                         * of the wqueue array is always allocated so
                         * that we always can remember how much was
                         * written. */
                        bus->wqueue[0] = sd_bus_message_ref(m);
                        bus->wqueue_size = 1;
                        bus->windex = idx;
                }
        } else {
                sd_bus_message **q;

                /* Just append it to the queue. */

                if (bus->wqueue_size >= BUS_WQUEUE_MAX)
                        return -ENOBUFS;

                q = realloc(bus->wqueue, sizeof(sd_bus_message*) * (bus->wqueue_size + 1));
                if (!q)
                        return -ENOMEM;

                bus->wqueue = q;
                q[bus->wqueue_size ++] = sd_bus_message_ref(m);
        }

        if (serial)
                *serial = BUS_MESSAGE_SERIAL(m);

        return 1;
}

static usec_t calc_elapse(uint64_t usec) {
        if (usec == (uint64_t) -1)
                return 0;

        if (usec == 0)
                usec = BUS_DEFAULT_TIMEOUT;

        return now(CLOCK_MONOTONIC) + usec;
}

static int timeout_compare(const void *a, const void *b) {
        const struct reply_callback *x = a, *y = b;

        if (x->timeout != 0 && y->timeout == 0)
                return -1;

        if (x->timeout == 0 && y->timeout != 0)
                return 1;

        if (x->timeout < y->timeout)
                return -1;

        if (x->timeout > y->timeout)
                return 1;

        return 0;
}

int sd_bus_send_with_reply(
                sd_bus *bus,
                sd_bus_message *m,
                sd_bus_message_handler_t callback,
                void *userdata,
                uint64_t usec,
                uint64_t *serial) {

        struct reply_callback *c;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(m, -EINVAL);
        assert_return(m->header->type == SD_BUS_MESSAGE_METHOD_CALL, -EINVAL);
        assert_return(!(m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED), -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        r = hashmap_ensure_allocated(&bus->reply_callbacks, uint64_hash_func, uint64_compare_func);
        if (r < 0)
                return r;

        if (usec != (uint64_t) -1) {
                r = prioq_ensure_allocated(&bus->reply_callbacks_prioq, timeout_compare);
                if (r < 0)
                        return r;
        }

        r = bus_seal_message(bus, m);
        if (r < 0)
                return r;

        c = new0(struct reply_callback, 1);
        if (!c)
                return -ENOMEM;

        c->callback = callback;
        c->userdata = userdata;
        c->serial = BUS_MESSAGE_SERIAL(m);
        c->timeout = calc_elapse(usec);

        r = hashmap_put(bus->reply_callbacks, &c->serial, c);
        if (r < 0) {
                free(c);
                return r;
        }

        if (c->timeout != 0) {
                r = prioq_put(bus->reply_callbacks_prioq, c, &c->prioq_idx);
                if (r < 0) {
                        c->timeout = 0;
                        sd_bus_send_with_reply_cancel(bus, c->serial);
                        return r;
                }
        }

        r = sd_bus_send(bus, m, serial);
        if (r < 0) {
                sd_bus_send_with_reply_cancel(bus, c->serial);
                return r;
        }

        return r;
}

int sd_bus_send_with_reply_cancel(sd_bus *bus, uint64_t serial) {
        struct reply_callback *c;

        assert_return(bus, -EINVAL);
        assert_return(serial != 0, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        c = hashmap_remove(bus->reply_callbacks, &serial);
        if (!c)
                return 0;

        if (c->timeout != 0)
                prioq_remove(bus->reply_callbacks_prioq, c, &c->prioq_idx);

        free(c);
        return 1;
}

int bus_ensure_running(sd_bus *bus) {
        int r;

        assert(bus);

        if (bus->state == BUS_UNSET || bus->state == BUS_CLOSED)
                return -ENOTCONN;
        if (bus->state == BUS_RUNNING)
                return 1;

        for (;;) {
                r = sd_bus_process(bus, NULL);
                if (r < 0)
                        return r;
                if (bus->state == BUS_RUNNING)
                        return 1;
                if (r > 0)
                        continue;

                r = sd_bus_wait(bus, (uint64_t) -1);
                if (r < 0)
                        return r;
        }
}

int sd_bus_send_with_reply_and_block(
                sd_bus *bus,
                sd_bus_message *m,
                uint64_t usec,
                sd_bus_error *error,
                sd_bus_message **reply) {

        int r;
        usec_t timeout;
        uint64_t serial;
        bool room = false;

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(m, -EINVAL);
        assert_return(m->header->type == SD_BUS_MESSAGE_METHOD_CALL, -EINVAL);
        assert_return(!(m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED), -EINVAL);
        assert_return(!bus_error_is_dirty(error), -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        r = bus_ensure_running(bus);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, m, &serial);
        if (r < 0)
                return r;

        timeout = calc_elapse(usec);

        for (;;) {
                usec_t left;
                sd_bus_message *incoming = NULL;

                if (!room) {
                        sd_bus_message **q;

                        if (bus->rqueue_size >= BUS_RQUEUE_MAX)
                                return -ENOBUFS;

                        /* Make sure there's room for queuing this
                         * locally, before we read the message */

                        q = realloc(bus->rqueue, (bus->rqueue_size + 1) * sizeof(sd_bus_message*));
                        if (!q)
                                return -ENOMEM;

                        bus->rqueue = q;
                        room = true;
                }

                if (bus->is_kernel)
                        r = bus_kernel_read_message(bus, &incoming);
                else
                        r = bus_socket_read_message(bus, &incoming);
                if (r < 0)
                        return r;
                if (incoming) {

                        if (incoming->reply_serial == serial) {
                                /* Found a match! */

                                if (incoming->header->type == SD_BUS_MESSAGE_METHOD_RETURN) {

                                        if (reply)
                                                *reply = incoming;
                                        else
                                                sd_bus_message_unref(incoming);

                                        return 1;
                                }

                                if (incoming->header->type == SD_BUS_MESSAGE_METHOD_ERROR) {
                                        int k;

                                        r = sd_bus_error_copy(error, &incoming->error);
                                        if (r < 0) {
                                                sd_bus_message_unref(incoming);
                                                return r;
                                        }

                                        k = sd_bus_error_get_errno(&incoming->error);
                                        sd_bus_message_unref(incoming);
                                        return -k;
                                }

                                sd_bus_message_unref(incoming);
                                return -EIO;
                        }

                        /* There's already guaranteed to be room for
                         * this, so need to resize things here */
                        bus->rqueue[bus->rqueue_size ++] = incoming;
                        room = false;

                        /* Try to read more, right-away */
                        continue;
                }
                if (r != 0)
                        continue;

                if (timeout > 0) {
                        usec_t n;

                        n = now(CLOCK_MONOTONIC);
                        if (n >= timeout)
                                return -ETIMEDOUT;

                        left = timeout - n;
                } else
                        left = (uint64_t) -1;

                r = bus_poll(bus, true, left);
                if (r < 0)
                        return r;

                r = dispatch_wqueue(bus);
                if (r < 0)
                        return r;
        }
}

int sd_bus_get_fd(sd_bus *bus) {

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(bus->input_fd == bus->output_fd, -EPERM);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        return bus->input_fd;
}

int sd_bus_get_events(sd_bus *bus) {
        int flags = 0;

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (bus->state == BUS_OPENING)
                flags |= POLLOUT;
        else if (bus->state == BUS_AUTHENTICATING) {

                if (bus_socket_auth_needs_write(bus))
                        flags |= POLLOUT;

                flags |= POLLIN;

        } else if (bus->state == BUS_RUNNING || bus->state == BUS_HELLO) {
                if (bus->rqueue_size <= 0)
                        flags |= POLLIN;
                if (bus->wqueue_size > 0)
                        flags |= POLLOUT;
        }

        return flags;
}

int sd_bus_get_timeout(sd_bus *bus, uint64_t *timeout_usec) {
        struct reply_callback *c;

        assert_return(bus, -EINVAL);
        assert_return(timeout_usec, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (bus->state == BUS_AUTHENTICATING) {
                *timeout_usec = bus->auth_timeout;
                return 1;
        }

        if (bus->state != BUS_RUNNING && bus->state != BUS_HELLO) {
                *timeout_usec = (uint64_t) -1;
                return 0;
        }

        if (bus->rqueue_size > 0) {
                *timeout_usec = 0;
                return 1;
        }

        c = prioq_peek(bus->reply_callbacks_prioq);
        if (!c) {
                *timeout_usec = (uint64_t) -1;
                return 0;
        }

        *timeout_usec = c->timeout;
        return 1;
}

static int process_timeout(sd_bus *bus) {
        _cleanup_bus_message_unref_ sd_bus_message* m = NULL;
        struct reply_callback *c;
        usec_t n;
        int r;

        assert(bus);

        c = prioq_peek(bus->reply_callbacks_prioq);
        if (!c)
                return 0;

        n = now(CLOCK_MONOTONIC);
        if (c->timeout > n)
                return 0;

        r = bus_message_new_synthetic_error(
                        bus,
                        c->serial,
                        &SD_BUS_ERROR_MAKE(SD_BUS_ERROR_NO_REPLY, "Method call timed out"),
                        &m);
        if (r < 0)
                return r;

        assert_se(prioq_pop(bus->reply_callbacks_prioq) == c);
        hashmap_remove(bus->reply_callbacks, &c->serial);

        r = c->callback(bus, m, c->userdata);
        free(c);

        return r < 0 ? r : 1;
}

static int process_hello(sd_bus *bus, sd_bus_message *m) {
        assert(bus);
        assert(m);

        if (bus->state != BUS_HELLO)
                return 0;

        /* Let's make sure the first message on the bus is the HELLO
         * reply. But note that we don't actually parse the message
         * here (we leave that to the usual handling), we just verify
         * we don't let any earlier msg through. */

        if (m->header->type != SD_BUS_MESSAGE_METHOD_RETURN &&
            m->header->type != SD_BUS_MESSAGE_METHOD_ERROR)
                return -EIO;

        if (m->reply_serial != bus->hello_serial)
                return -EIO;

        return 0;
}

static int process_reply(sd_bus *bus, sd_bus_message *m) {
        struct reply_callback *c;
        int r;

        assert(bus);
        assert(m);

        if (m->header->type != SD_BUS_MESSAGE_METHOD_RETURN &&
            m->header->type != SD_BUS_MESSAGE_METHOD_ERROR)
                return 0;

        c = hashmap_remove(bus->reply_callbacks, &m->reply_serial);
        if (!c)
                return 0;

        if (c->timeout != 0)
                prioq_remove(bus->reply_callbacks_prioq, c, &c->prioq_idx);

        r = sd_bus_message_rewind(m, true);
        if (r < 0)
                return r;

        r = c->callback(bus, m, c->userdata);
        free(c);

        return r;
}

static int process_filter(sd_bus *bus, sd_bus_message *m) {
        struct filter_callback *l;
        int r;

        assert(bus);
        assert(m);

        do {
                bus->filter_callbacks_modified = false;

                LIST_FOREACH(callbacks, l, bus->filter_callbacks) {

                        if (bus->filter_callbacks_modified)
                                break;

                        /* Don't run this more than once per iteration */
                        if (l->last_iteration == bus->iteration_counter)
                                continue;

                        l->last_iteration = bus->iteration_counter;

                        r = sd_bus_message_rewind(m, true);
                        if (r < 0)
                                return r;

                        r = l->callback(bus, m, l->userdata);
                        if (r != 0)
                                return r;

                }

        } while (bus->filter_callbacks_modified);

        return 0;
}

static int process_match(sd_bus *bus, sd_bus_message *m) {
        int r;

        assert(bus);
        assert(m);

        do {
                bus->match_callbacks_modified = false;

                r = bus_match_run(bus, &bus->match_callbacks, m);
                if (r != 0)
                        return r;

        } while (bus->match_callbacks_modified);

        return 0;
}

static int process_builtin(sd_bus *bus, sd_bus_message *m) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        int r;

        assert(bus);
        assert(m);

        if (m->header->type != SD_BUS_MESSAGE_METHOD_CALL)
                return 0;

        if (!streq_ptr(m->interface, "org.freedesktop.DBus.Peer"))
                return 0;

        if (m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED)
                return 1;

        if (streq_ptr(m->member, "Ping"))
                r = sd_bus_message_new_method_return(bus, m, &reply);
        else if (streq_ptr(m->member, "GetMachineId")) {
                sd_id128_t id;
                char sid[33];

                r = sd_id128_get_machine(&id);
                if (r < 0)
                        return r;

                r = sd_bus_message_new_method_return(bus, m, &reply);
                if (r < 0)
                        return r;

                r = sd_bus_message_append(reply, "s", sd_id128_to_string(id, sid));
        } else {
                r = sd_bus_message_new_method_errorf(
                                bus, m, &reply,
                                SD_BUS_ERROR_UNKNOWN_METHOD,
                                 "Unknown method '%s' on interface '%s'.", m->member, m->interface);
        }

        if (r < 0)
                return r;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int process_message(sd_bus *bus, sd_bus_message *m) {
        int r;

        assert(bus);
        assert(m);

        bus->iteration_counter++;

        log_debug("Got message sender=%s object=%s interface=%s member=%s",
                  strna(sd_bus_message_get_sender(m)),
                  strna(sd_bus_message_get_path(m)),
                  strna(sd_bus_message_get_interface(m)),
                  strna(sd_bus_message_get_member(m)));

        r = process_hello(bus, m);
        if (r != 0)
                return r;

        r = process_reply(bus, m);
        if (r != 0)
                return r;

        r = process_filter(bus, m);
        if (r != 0)
                return r;

        r = process_match(bus, m);
        if (r != 0)
                return r;

        r = process_builtin(bus, m);
        if (r != 0)
                return r;

        return bus_process_object(bus, m);
}

static int process_running(sd_bus *bus, sd_bus_message **ret) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert(bus);
        assert(bus->state == BUS_RUNNING || bus->state == BUS_HELLO);

        r = process_timeout(bus);
        if (r != 0)
                goto null_message;

        r = dispatch_wqueue(bus);
        if (r != 0)
                goto null_message;

        r = dispatch_rqueue(bus, &m);
        if (r < 0)
                return r;
        if (!m)
                goto null_message;

        r = process_message(bus, m);
        if (r != 0)
                goto null_message;

        if (ret) {
                r = sd_bus_message_rewind(m, true);
                if (r < 0)
                        return r;

                *ret = m;
                m = NULL;
                return 1;
        }

        if (m->header->type == SD_BUS_MESSAGE_METHOD_CALL) {

                r = sd_bus_reply_method_errorf(
                                bus, m,
                                SD_BUS_ERROR_UNKNOWN_OBJECT,
                                "Unknown object '%s'.", m->path);
                if (r < 0)
                        return r;
        }

        return 1;

null_message:
        if (r >= 0 && ret)
                *ret = NULL;

        return r;
}

int sd_bus_process(sd_bus *bus, sd_bus_message **ret) {
        BUS_DONT_DESTROY(bus);
        int r;

        /* Returns 0 when we didn't do anything. This should cause the
         * caller to invoke sd_bus_wait() before returning the next
         * time. Returns > 0 when we did something, which possibly
         * means *ret is filled in with an unprocessed message. */

        assert_return(bus, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        /* We don't allow recursively invoking sd_bus_process(). */
        assert_return(!bus->processing, -EBUSY);

        switch (bus->state) {

        case BUS_UNSET:
        case BUS_CLOSED:
                return -ENOTCONN;

        case BUS_OPENING:
                r = bus_socket_process_opening(bus);
                if (r < 0)
                        return r;
                if (ret)
                        *ret = NULL;
                return r;

        case BUS_AUTHENTICATING:

                r = bus_socket_process_authenticating(bus);
                if (r < 0)
                        return r;
                if (ret)
                        *ret = NULL;
                return r;

        case BUS_RUNNING:
        case BUS_HELLO:

                bus->processing = true;
                r = process_running(bus, ret);
                bus->processing = false;

                return r;
        }

        assert_not_reached("Unknown state");
}

static int bus_poll(sd_bus *bus, bool need_more, uint64_t timeout_usec) {
        struct pollfd p[2] = {};
        int r, e, n;
        struct timespec ts;
        usec_t m = (usec_t) -1;

        assert(bus);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);

        e = sd_bus_get_events(bus);
        if (e < 0)
                return e;

        if (need_more)
                /* The caller really needs some more data, he doesn't
                 * care about what's already read, or any timeouts
                 * except its own.*/
                e |= POLLIN;
        else {
                usec_t until;
                /* The caller wants to process if there's something to
                 * process, but doesn't care otherwise */

                r = sd_bus_get_timeout(bus, &until);
                if (r < 0)
                        return r;
                if (r > 0) {
                        usec_t nw;
                        nw = now(CLOCK_MONOTONIC);
                        m = until > nw ? until - nw : 0;
                }
        }

        if (timeout_usec != (uint64_t) -1 && (m == (uint64_t) -1 || timeout_usec < m))
                m = timeout_usec;

        p[0].fd = bus->input_fd;
        if (bus->output_fd == bus->input_fd) {
                p[0].events = e;
                n = 1;
        } else {
                p[0].events = e & POLLIN;
                p[1].fd = bus->output_fd;
                p[1].events = e & POLLOUT;
                n = 2;
        }

        r = ppoll(p, n, m == (uint64_t) -1 ? NULL : timespec_store(&ts, m), NULL);
        if (r < 0)
                return -errno;

        return r > 0 ? 1 : 0;
}

int sd_bus_wait(sd_bus *bus, uint64_t timeout_usec) {

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (bus->rqueue_size > 0)
                return 0;

        return bus_poll(bus, false, timeout_usec);
}

int sd_bus_flush(sd_bus *bus) {
        int r;

        assert_return(bus, -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        r = bus_ensure_running(bus);
        if (r < 0)
                return r;

        if (bus->wqueue_size <= 0)
                return 0;

        for (;;) {
                r = dispatch_wqueue(bus);
                if (r < 0)
                        return r;

                if (bus->wqueue_size <= 0)
                        return 0;

                r = bus_poll(bus, false, (uint64_t) -1);
                if (r < 0)
                        return r;
        }
}

int sd_bus_add_filter(sd_bus *bus, sd_bus_message_handler_t callback, void *userdata) {
        struct filter_callback *f;

        assert_return(bus, -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        f = new0(struct filter_callback, 1);
        if (!f)
                return -ENOMEM;
        f->callback = callback;
        f->userdata = userdata;

        bus->filter_callbacks_modified = true;
        LIST_PREPEND(callbacks, bus->filter_callbacks, f);
        return 0;
}

int sd_bus_remove_filter(sd_bus *bus, sd_bus_message_handler_t callback, void *userdata) {
        struct filter_callback *f;

        assert_return(bus, -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        LIST_FOREACH(callbacks, f, bus->filter_callbacks) {
                if (f->callback == callback && f->userdata == userdata) {
                        bus->filter_callbacks_modified = true;
                        LIST_REMOVE(callbacks, bus->filter_callbacks, f);
                        free(f);
                        return 1;
                }
        }

        return 0;
}

int sd_bus_add_match(sd_bus *bus, const char *match, sd_bus_message_handler_t callback, void *userdata) {
        struct bus_match_component *components = NULL;
        unsigned n_components = 0;
        uint64_t cookie = 0;
        int r = 0;

        assert_return(bus, -EINVAL);
        assert_return(match, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        r = bus_match_parse(match, &components, &n_components);
        if (r < 0)
                goto finish;

        if (bus->bus_client) {
                cookie = ++bus->match_cookie;

                r = bus_add_match_internal(bus, match, components, n_components, cookie);
                if (r < 0)
                        goto finish;
        }

        bus->match_callbacks_modified = true;
        r = bus_match_add(&bus->match_callbacks, components, n_components, callback, userdata, cookie, NULL);
        if (r < 0) {
                if (bus->bus_client)
                        bus_remove_match_internal(bus, match, cookie);
        }

finish:
        bus_match_parse_free(components, n_components);
        return r;
}

int sd_bus_remove_match(sd_bus *bus, const char *match, sd_bus_message_handler_t callback, void *userdata) {
        struct bus_match_component *components = NULL;
        unsigned n_components = 0;
        int r = 0, q = 0;
        uint64_t cookie = 0;

        assert_return(bus, -EINVAL);
        assert_return(match, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        r = bus_match_parse(match, &components, &n_components);
        if (r < 0)
                return r;

        bus->match_callbacks_modified = true;
        r = bus_match_remove(&bus->match_callbacks, components, n_components, callback, userdata, &cookie);

        if (bus->bus_client)
                q = bus_remove_match_internal(bus, match, cookie);

        bus_match_parse_free(components, n_components);

        return r < 0 ? r : q;
}

bool bus_pid_changed(sd_bus *bus) {
        assert(bus);

        /* We don't support people creating a bus connection and
         * keeping it around over a fork(). Let's complain. */

        return bus->original_pid != getpid();
}

static int io_callback(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        void *bus = userdata;
        int r;

        assert(bus);

        r = sd_bus_process(bus, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int time_callback(sd_event_source *s, uint64_t usec, void *userdata) {
        void *bus = userdata;
        int r;

        assert(bus);

        r = sd_bus_process(bus, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int prepare_callback(sd_event_source *s, void *userdata) {
        sd_bus *bus = userdata;
        int r, e;
        usec_t until;

        assert(s);
        assert(bus);

        e = sd_bus_get_events(bus);
        if (e < 0)
                return e;

        if (bus->output_fd != bus->input_fd) {

                r = sd_event_source_set_io_events(bus->input_io_event_source, e & POLLIN);
                if (r < 0)
                        return r;

                r = sd_event_source_set_io_events(bus->output_io_event_source, e & POLLOUT);
                if (r < 0)
                        return r;
        } else {
                r = sd_event_source_set_io_events(bus->input_io_event_source, e);
                if (r < 0)
                        return r;
        }

        r = sd_bus_get_timeout(bus, &until);
        if (r < 0)
                return r;
        if (r > 0) {
                int j;

                j = sd_event_source_set_time(bus->time_event_source, until);
                if (j < 0)
                        return j;
        }

        r = sd_event_source_set_enabled(bus->time_event_source, r > 0);
        if (r < 0)
                return r;

        return 1;
}

static int quit_callback(sd_event_source *event, void *userdata) {
        sd_bus *bus = userdata;

        assert(event);

        sd_bus_flush(bus);

        return 1;
}

int sd_bus_attach_event(sd_bus *bus, sd_event *event, int priority) {
        int r;

        assert_return(bus, -EINVAL);
        assert_return(event, -EINVAL);
        assert_return(!bus->event, -EBUSY);

        assert(!bus->input_io_event_source);
        assert(!bus->output_io_event_source);
        assert(!bus->time_event_source);

        bus->event = sd_event_ref(event);

        r = sd_event_add_io(event, bus->input_fd, 0, io_callback, bus, &bus->input_io_event_source);
        if (r < 0)
                goto fail;

        r = sd_event_source_set_priority(bus->input_io_event_source, priority);
        if (r < 0)
                goto fail;

        if (bus->output_fd != bus->input_fd) {
                r = sd_event_add_io(event, bus->output_fd, 0, io_callback, bus, &bus->output_io_event_source);
                if (r < 0)
                        goto fail;

                r = sd_event_source_set_priority(bus->output_io_event_source, priority);
                if (r < 0)
                        goto fail;
        }

        r = sd_event_source_set_prepare(bus->input_io_event_source, prepare_callback);
        if (r < 0)
                goto fail;

        r = sd_event_add_monotonic(event, 0, 0, time_callback, bus, &bus->time_event_source);
        if (r < 0)
                goto fail;

        r = sd_event_source_set_priority(bus->time_event_source, priority);
        if (r < 0)
                goto fail;

        r = sd_event_add_quit(event, quit_callback, bus, &bus->quit_event_source);
        if (r < 0)
                goto fail;

        return 0;

fail:
        sd_bus_detach_event(bus);
        return r;
}

int sd_bus_detach_event(sd_bus *bus) {
        assert_return(bus, -EINVAL);
        assert_return(bus->event, -ENXIO);

        if (bus->input_io_event_source)
                bus->input_io_event_source = sd_event_source_unref(bus->input_io_event_source);

        if (bus->output_io_event_source)
                bus->output_io_event_source = sd_event_source_unref(bus->output_io_event_source);

        if (bus->time_event_source)
                bus->time_event_source = sd_event_source_unref(bus->time_event_source);

        if (bus->quit_event_source)
                bus->quit_event_source = sd_event_source_unref(bus->quit_event_source);

        if (bus->event)
                bus->event = sd_event_unref(bus->event);

        return 0;
}
