/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

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

#include <errno.h>
#include <string.h>

#include "bus-util.h"
#include "machine.h"

static int property_get_id(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Machine *m = userdata;
        int r;

        assert(bus);
        assert(reply);
        assert(m);

        r = sd_bus_message_append_array(reply, 'y', &m->id, 16);
        if (r < 0)
                return r;

        return 1;
}

static int property_get_state(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                sd_bus_error *error,
                void *userdata) {

        Machine *m = userdata;
        const char *state;
        int r;

        assert(bus);
        assert(reply);
        assert(m);

        state = machine_state_to_string(machine_get_state(m));

        r = sd_bus_message_append_basic(reply, 's', state);
        if (r < 0)
                return r;

        return 1;
}

static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_class, machine_class, MachineClass);

static int method_terminate(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Machine *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = machine_stop(m);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_kill(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Machine *m = userdata;
        const char *swho;
        int32_t signo;
        KillWho who;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "si", &swho, &signo);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (isempty(swho))
                who = KILL_ALL;
        else {
                who = kill_who_from_string(swho);
                if (who < 0)
                        return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid kill parameter '%s'", swho);
        }

        if (signo <= 0 || signo >= _NSIG)
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid signal %i", signo);

        r = machine_kill(m, who, signo);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

const sd_bus_vtable machine_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Name", "s", NULL, offsetof(Machine, name), 0),
        SD_BUS_PROPERTY("Id", "ay", property_get_id, 0, 0),
        SD_BUS_PROPERTY("Timestamp", "t", NULL, offsetof(Machine, timestamp.realtime), 0),
        SD_BUS_PROPERTY("TimestampMonotonic", "t", NULL, offsetof(Machine, timestamp.monotonic), 0),
        SD_BUS_PROPERTY("Service", "s", NULL, offsetof(Machine, service), 0),
        SD_BUS_PROPERTY("Scope", "s", NULL, offsetof(Machine, scope), 0),
        SD_BUS_PROPERTY("Leader", "u", NULL, offsetof(Machine, leader), 0),
        SD_BUS_PROPERTY("Class", "s", property_get_class, offsetof(Machine, class), 0),
        SD_BUS_PROPERTY("State", "s", property_get_state, 0, 0),
        SD_BUS_PROPERTY("RootDirectory", "s", NULL, offsetof(Machine, root_directory), 0),
        SD_BUS_METHOD("Terminate", NULL, NULL, method_terminate, 0),
        SD_BUS_METHOD("Kill", "si", NULL, method_kill, 0),
        SD_BUS_VTABLE_END
};

int machine_object_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        _cleanup_free_ char *e = NULL;
        Manager *m = userdata;
        Machine *machine;
        const char *p;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        p = startswith(path, "/org/freedesktop/machine1/machine/");
        if (!p)
                return 0;

        e = bus_path_unescape(p);
        if (!e)
                return -ENOMEM;

        machine = hashmap_get(m->machines, e);
        if (!machine)
                return 0;

        *found = machine;
        return 1;
}

char *machine_bus_path(Machine *m) {
        _cleanup_free_ char *e = NULL;

        assert(m);

        e = bus_path_escape(m->name);
        if (!e)
                return NULL;

        return strappend("/org/freedesktop/machine1/machine/", e);
}

int machine_send_signal(Machine *m, bool new_machine) {
        _cleanup_free_ char *p = NULL;

        assert(m);

        p = machine_bus_path(m);
        if (!p)
                return -ENOMEM;

        return sd_bus_emit_signal(
                        m->manager->bus,
                        "/org/freedesktop/machine1",
                        "org.freedesktop.machine1.Manager",
                        new_machine ? "MachineNew" : "MachineRemoved",
                        "so", m->name, p);
}

int machine_send_create_reply(Machine *m, sd_bus_error *error) {
        _cleanup_bus_message_unref_ sd_bus_message *c = NULL;
        _cleanup_free_ char *p = NULL;

        assert(m);

        if (!m->create_message)
                return 0;

        c = m->create_message;
        m->create_message = NULL;

        /* Update the machine state file before we notify the client
         * about the result. */
        machine_save(m);

        if (error)
                return sd_bus_reply_method_error(m->manager->bus, c, error);

        p = machine_bus_path(m);
        if (!p)
                return -ENOMEM;

        return sd_bus_reply_method_return(m->manager->bus, c, "o", p);
}
