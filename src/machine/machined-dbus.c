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
#include <unistd.h>
#include <pwd.h>

#include "sd-id128.h"
#include "sd-messages.h"

#include "strv.h"
#include "mkdir.h"
#include "path-util.h"
#include "special.h"
#include "fileio-label.h"
#include "label.h"
#include "utf8.h"
#include "unit-name.h"
#include "bus-util.h"
#include "time-util.h"
#include "machined.h"

static bool valid_machine_name(const char *p) {
        size_t l;

        if (!filename_is_safe(p))
                return false;

        if (!ascii_is_valid(p))
                return false;

        l = strlen(p);

        if (l < 1 || l> 64)
                return false;

        return true;
}

static int method_get_machine(sd_bus *bus, sd_bus_message *message, void *userdata) {
        _cleanup_free_ char *p = NULL;
        Manager *m = userdata;
        Machine *machine;
        const char *name;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        machine = hashmap_get(m->machines, name);
        if (!machine)
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NO_SUCH_MACHINE, "No machine '%s' known", name);

        p = machine_bus_path(machine);
        if (!p)
                return sd_bus_reply_method_errno(bus, message, -ENOMEM, NULL);

        return sd_bus_reply_method_return(bus, message, "o", p);
}

static int method_get_machine_by_pid(sd_bus *bus, sd_bus_message *message, void *userdata) {
        _cleanup_free_ char *p = NULL;
        Manager *m = userdata;
        Machine *machine = NULL;
        uint32_t pid;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "u", &pid);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        r = manager_get_machine_by_pid(m, pid, &machine);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);
        if (!machine)
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NO_MACHINE_FOR_PID, "PID %lu does not belong to any known machine", (unsigned long) pid);

        p = machine_bus_path(machine);
        if (!p)
                return sd_bus_reply_method_errno(bus, message, -ENOMEM, NULL);

        return sd_bus_reply_method_return(bus, message, "o", p);
}

static int method_list_machines(sd_bus *bus, sd_bus_message *message, void *userdata) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        Manager *m = userdata;
        Machine *machine;
        Iterator i;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_new_method_return(bus, message, &reply);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        r = sd_bus_message_open_container(reply, 'a', "(ssso)");
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        HASHMAP_FOREACH(machine, m->machines, i) {
                _cleanup_free_ char *p = NULL;

                p = machine_bus_path(machine);
                if (!p)
                        return sd_bus_reply_method_errno(bus, message, -ENOMEM, NULL);

                r = sd_bus_message_append(reply, "(ssso)",
                                          machine->name,
                                          strempty(machine_class_to_string(machine->class)),
                                          machine->service,
                                          p);
                if (r < 0)
                        return sd_bus_reply_method_errno(bus, message, r, NULL);
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_send(bus, reply, NULL);
}

static int method_create_machine(sd_bus *bus, sd_bus_message *message, void *userdata) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *name, *service, *class, *root_directory;
        Manager *manager = userdata;
        MachineClass c;
        uint32_t leader;
        sd_id128_t id;
        const void *v;
        Machine *m;
        size_t n;
        int r;

        assert(bus);
        assert(message);
        assert(manager);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);
        if (!valid_machine_name(name))
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid machine name");

        r = sd_bus_message_read_array(message, 'y', &v, &n);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);
        if (n == 0)
                id = SD_ID128_NULL;
        else if (n == 16)
                memcpy(&id, v, n);
        else
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid machine ID parameter");

        r = sd_bus_message_read(message, "ssus", &service, &class, &leader, &root_directory);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (isempty(class))
                c = _MACHINE_CLASS_INVALID;
        else {
                c = machine_class_from_string(class);
                if (c < 0)
                        return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid machine class parameter");
        }

        if (leader == 1)
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Invalid leader PID");

        if (!isempty(root_directory) && !path_is_absolute(root_directory))
                return sd_bus_reply_method_errorf(bus, message, SD_BUS_ERROR_INVALID_ARGS, "Root directory must be empty or an absolute path");

        r = sd_bus_message_enter_container(message, 'a', "(sv)");
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        if (leader == 0) {
                assert_cc(sizeof(uint32_t) == sizeof(pid_t));

                r = sd_bus_get_owner_pid(bus, sd_bus_message_get_sender(message), (pid_t*) &leader);
                if (r < 0)
                        return sd_bus_reply_method_errno(bus, message, r, NULL);
        }

        if (hashmap_get(manager->machines, name))
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_MACHINE_EXISTS, "Machine '%s' already exists", name);

        r = manager_add_machine(manager, name, &m);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        m->leader = leader;
        m->class = c;
        m->id = id;

        if (!isempty(service)) {
                m->service = strdup(service);
                if (!m->service) {
                        r = sd_bus_reply_method_errno(bus, message, -ENOMEM, NULL);
                        goto fail;
                }
        }

        if (!isempty(root_directory)) {
                m->root_directory = strdup(root_directory);
                if (!m->root_directory) {
                        r = sd_bus_reply_method_errno(bus, message, -ENOMEM, NULL);
                        goto fail;
                }
        }

        r = machine_start(m, message, &error);
        if (r < 0) {
                r = sd_bus_reply_method_errno(bus, message, r, &error);
                goto fail;
        }

        m->create_message = sd_bus_message_ref(message);

        return 1;

fail:
        machine_add_to_gc_queue(m);

        return r;
}

static int method_terminate_machine(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Manager *m = userdata;
        Machine *machine;
        const char *name;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        machine = hashmap_get(m->machines, name);
        if (!machine)
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NO_SUCH_MACHINE, "No machine '%s' known", name);

        r = machine_stop(machine);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

static int method_kill_machine(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Manager *m = userdata;
        Machine *machine;
        const char *name;
        const char *swho;
        int32_t signo;
        KillWho who;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "ssi", &name, &swho, &signo);
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

        machine = hashmap_get(m->machines, name);
        if (!machine)
                return sd_bus_reply_method_errorf(bus, message, BUS_ERROR_NO_SUCH_MACHINE, "No machine '%s' known", name);

        r = machine_kill(machine, who, signo);
        if (r < 0)
                return sd_bus_reply_method_errno(bus, message, r, NULL);

        return sd_bus_reply_method_return(bus, message, NULL);
}

const sd_bus_vtable manager_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("GetMachine", "s", "o", method_get_machine, 0),
        SD_BUS_METHOD("GetMachineByPID", "u", "o", method_get_machine_by_pid, 0),
        SD_BUS_METHOD("ListMachines", NULL, "a(ssso)", method_list_machines, 0),
        SD_BUS_METHOD("CreateMachine", "sayssusa(sv)", "o", method_create_machine, 0),
        SD_BUS_METHOD("KillMachine", "ssi", NULL, method_kill_machine, 0),
        SD_BUS_METHOD("TerminateMachine", "s", NULL, method_terminate_machine, 0),
        SD_BUS_SIGNAL("MachineNew", "so", 0),
        SD_BUS_SIGNAL("MachineRemoved", "so", 0),
        SD_BUS_VTABLE_END
};

int machine_node_enumerator(sd_bus *bus, const char *path, char ***nodes, void *userdata) {
        Machine *machine = NULL;
        Manager *m = userdata;
        char **l = NULL;
        Iterator i;
        int r;

        assert(bus);
        assert(path);
        assert(nodes);

        HASHMAP_FOREACH(machine, m->machines, i) {
                char *p;

                p = machine_bus_path(machine);
                if (!p)
                        return -ENOMEM;

                r = strv_push(&l, p);
                if (r < 0) {
                        free(p);
                        return r;
                }
        }

        *nodes = l;
        return 1;
}

int match_job_removed(sd_bus *bus, sd_bus_message *message, void *userdata) {
        const char *path, *result, *unit;
        Manager *m = userdata;
        Machine *machine;
        uint32_t id;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "uoss", &id, &path, &unit, &result);
        if (r < 0) {
                log_error("Failed to parse JobRemoved message: %s", strerror(-r));
                return 0;
        }

        machine = hashmap_get(m->machine_units, unit);
        if (!machine)
                return 0;

        if (streq_ptr(path, machine->scope_job)) {
                free(machine->scope_job);
                machine->scope_job = NULL;

                if (machine->started) {
                        if (streq(result, "done"))
                                machine_send_create_reply(machine, NULL);
                        else {
                                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;

                                sd_bus_error_setf(&error, BUS_ERROR_JOB_FAILED, "Start job for unit %s failed with '%s'", unit, result);

                                machine_send_create_reply(machine, &error);
                        }
                } else
                        machine_save(machine);
        }

        machine_add_to_gc_queue(machine);
        return 0;
}

int match_properties_changed(sd_bus *bus, sd_bus_message *message, void *userdata) {
        _cleanup_free_ char *unit = NULL;
        Manager *m = userdata;
        Machine *machine;
        const char *path;

        assert(bus);
        assert(message);
        assert(m);

        path = sd_bus_message_get_path(message);
        if (!path)
                return 0;

        unit_name_from_dbus_path(path, &unit);
        if (!unit)
                return 0;

        machine = hashmap_get(m->machine_units, unit);
        if (machine)
                machine_add_to_gc_queue(machine);

        return 0;
}

int match_unit_removed(sd_bus *bus, sd_bus_message *message, void *userdata) {
        const char *path, *unit;
        Manager *m = userdata;
        Machine *machine;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "so", &unit, &path);
        if (r < 0) {
                log_error("Failed to parse UnitRemoved message: %s", strerror(-r));
                return 0;
        }

        machine = hashmap_get(m->machine_units, unit);
        if (machine)
                machine_add_to_gc_queue(machine);

        return 0;
}

int match_reloading(sd_bus *bus, sd_bus_message *message, void *userdata) {
        Manager *m = userdata;
        int b, r;

        assert(bus);

        r = sd_bus_message_read(message, "b", &b);
        if (r < 0) {
                log_error("Failed to parse Reloading message: %s", strerror(-r));
                return 0;
        }

        /* systemd finished reloading, let's recheck all our machines */
        if (!b) {
                Machine *machine;
                Iterator i;

                log_debug("System manager has been reloaded, rechecking machines...");

                HASHMAP_FOREACH(machine, m->machines, i)
                        machine_add_to_gc_queue(machine);
        }

        return 0;
}

int manager_start_scope(
                Manager *manager,
                const char *scope,
                pid_t pid,
                const char *slice,
                const char *description,
                sd_bus_message *more_properties,
                sd_bus_error *error,
                char **job) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL, *reply = NULL;
        int r;

        assert(manager);
        assert(scope);
        assert(pid > 1);

        r = sd_bus_message_new_method_call(
                        manager->bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "StartTransientUnit",
                        &m);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "ss", scope, "fail");
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0)
                return r;

        if (!isempty(slice)) {
                r = sd_bus_message_append(m, "(sv)", "Slice", "s", slice);
                if (r < 0)
                        return r;
        }

        if (!isempty(description)) {
                r = sd_bus_message_append(m, "(sv)", "Description", "s", description);
                if (r < 0)
                        return r;
        }

        /* cgroup empty notification is not available in containers
         * currently. To make this less problematic, let's shorten the
         * stop timeout for machines, so that we don't wait
         * forever. */
        r = sd_bus_message_append(m, "(sv)", "TimeoutStopUSec", "t", 500 * USEC_PER_MSEC);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "(sv)", "PIDs", "au", 1, pid);
        if (r < 0)
                return r;

        if (more_properties) {
                r = sd_bus_message_copy(m, more_properties, true);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return r;

        r = sd_bus_send_with_reply_and_block(manager->bus, m, 0, error, &reply);
        if (r < 0)
                return r;

        if (job) {
                const char *j;
                char *copy;

                r = sd_bus_message_read(reply, "o", &j);
                if (r < 0)
                        return r;

                copy = strdup(j);
                if (!copy)
                        return -ENOMEM;

                *job = copy;
        }

        return 1;
}

int manager_stop_unit(Manager *manager, const char *unit, sd_bus_error *error, char **job) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        int r;

        assert(manager);
        assert(unit);

        r = sd_bus_call_method(
                        manager->bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "StopUnit",
                        error,
                        &reply,
                        "ss", unit, "fail");
        if (r < 0) {
                if (sd_bus_error_has_name(error, BUS_ERROR_NO_SUCH_UNIT) ||
                    sd_bus_error_has_name(error, BUS_ERROR_LOAD_FAILED)) {

                        if (job)
                                *job = NULL;

                        sd_bus_error_free(error);
                        return 0;
                }

                return r;
        }

        if (job) {
                const char *j;
                char *copy;

                r = sd_bus_message_read(reply, "o", &j);
                if (r < 0)
                        return r;

                copy = strdup(j);
                if (!copy)
                        return -ENOMEM;

                *job = copy;
        }

        return 1;
}

int manager_kill_unit(Manager *manager, const char *unit, KillWho who, int signo, sd_bus_error *error) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        int r;

        assert(manager);
        assert(unit);

        r = sd_bus_call_method(
                        manager->bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "KillUnit",
                        error,
                        &reply,
                        "ssi", unit, who == KILL_LEADER ? "main" : "all", signo);

        return r;
}

int manager_unit_is_active(Manager *manager, const char *unit) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_free_ char *path = NULL;
        const char *state;
        int r;

        assert(manager);
        assert(unit);

        path = unit_dbus_path_from_name(unit);
        if (!path)
                return -ENOMEM;

        r = sd_bus_get_property(
                        manager->bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Unit",
                        "ActiveState",
                        &error,
                        &reply,
                        "s");
        if (r < 0) {
                if (sd_bus_error_has_name(&error, SD_BUS_ERROR_NO_REPLY) ||
                    sd_bus_error_has_name(&error, SD_BUS_ERROR_DISCONNECTED))
                        return true;

                if (sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_UNIT) ||
                    sd_bus_error_has_name(&error, BUS_ERROR_LOAD_FAILED))
                        return false;

                return r;
        }

        r = sd_bus_message_read(reply, "s", &state);
        if (r < 0)
                return -EINVAL;

        return !streq(state, "inactive") && !streq(state, "failed");
}

int manager_job_is_active(Manager *manager, const char *path) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        int r;

        assert(manager);
        assert(path);

        r = sd_bus_get_property(
                        manager->bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Job",
                        "State",
                        &error,
                        &reply,
                        "s");
        if (r < 0) {
                if (sd_bus_error_has_name(&error, SD_BUS_ERROR_NO_REPLY) ||
                    sd_bus_error_has_name(&error, SD_BUS_ERROR_DISCONNECTED))
                        return true;

                if (sd_bus_error_has_name(&error, SD_BUS_ERROR_UNKNOWN_OBJECT))
                        return false;

                return r;
        }

        /* We don't actually care about the state really. The fact
         * that we could read the job state is enough for us */

        return true;
}
