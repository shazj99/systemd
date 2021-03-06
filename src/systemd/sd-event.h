/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef foosdeventhfoo
#define foosdeventhfoo

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

#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <inttypes.h>
#include <signal.h>

/*
  Why is this better than pure epoll?

  - Supports event source priorisation
  - Scales better with a large number of time events, since it doesn't require one timerfd each
  - Automatically tries to coalesce timer events system-wide
  - Handles signals and child PIDs
*/

typedef struct sd_event sd_event;
typedef struct sd_event_source sd_event_source;

enum {
        SD_EVENT_OFF = 0,
        SD_EVENT_ON = 1,
        SD_EVENT_ONESHOT = -1
};

enum {
        SD_EVENT_PASSIVE,
        SD_EVENT_RUNNING,
        SD_EVENT_QUITTING,
        SD_EVENT_FINISHED
};

typedef int (*sd_io_handler_t)(sd_event_source *s, int fd, uint32_t revents, void *userdata);
typedef int (*sd_time_handler_t)(sd_event_source *s, uint64_t usec, void *userdata);
typedef int (*sd_signal_handler_t)(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata);
typedef int (*sd_child_handler_t)(sd_event_source *s, const siginfo_t *si, void *userdata);
typedef int (*sd_defer_handler_t)(sd_event_source *s, void *userdata);
typedef int (*sd_prepare_handler_t)(sd_event_source *s, void *userdata);
typedef int (*sd_quit_handler_t)(sd_event_source *s, void *userdata);

int sd_event_new(sd_event **e);
sd_event* sd_event_ref(sd_event *e);
sd_event* sd_event_unref(sd_event *e);

int sd_event_add_io(sd_event *e, int fd, uint32_t events, sd_io_handler_t callback, void *userdata, sd_event_source **s);
int sd_event_add_monotonic(sd_event *e, uint64_t usec, uint64_t accuracy, sd_time_handler_t callback, void *userdata, sd_event_source **s);
int sd_event_add_realtime(sd_event *e, uint64_t usec, uint64_t accuracy, sd_time_handler_t callback, void *userdata, sd_event_source **s);
int sd_event_add_signal(sd_event *e, int sig, sd_signal_handler_t callback, void *userdata, sd_event_source **s);
int sd_event_add_child(sd_event *e, pid_t pid, int options, sd_child_handler_t callback, void *userdata, sd_event_source **s);
int sd_event_add_defer(sd_event *e, sd_defer_handler_t callback, void *userdata, sd_event_source **s);
int sd_event_add_quit(sd_event *e, sd_quit_handler_t callback, void *userdata, sd_event_source **s);

int sd_event_run(sd_event *e, uint64_t timeout);
int sd_event_loop(sd_event *e);

int sd_event_get_state(sd_event *e);
int sd_event_get_quit(sd_event *e);
int sd_event_request_quit(sd_event *e);
int sd_event_get_now_realtime(sd_event *e, uint64_t *usec);
int sd_event_get_now_monotonic(sd_event *e, uint64_t *usec);
sd_event *sd_event_get(sd_event_source *s);

sd_event_source* sd_event_source_ref(sd_event_source *s);
sd_event_source* sd_event_source_unref(sd_event_source *s);

int sd_event_source_set_prepare(sd_event_source *s, sd_prepare_handler_t callback);
int sd_event_source_get_pending(sd_event_source *s);
int sd_event_source_get_priority(sd_event_source *s, int *priority);
int sd_event_source_set_priority(sd_event_source *s, int priority);
int sd_event_source_get_enabled(sd_event_source *s, int *enabled);
int sd_event_source_set_enabled(sd_event_source *s, int enabled);
void* sd_event_source_get_userdata(sd_event_source *s);
int sd_event_source_get_io_fd(sd_event_source *s);
int sd_event_source_get_io_events(sd_event_source *s, uint32_t* events);
int sd_event_source_set_io_events(sd_event_source *s, uint32_t events);
int sd_event_source_get_io_revents(sd_event_source *s, uint32_t* revents);
int sd_event_source_get_time(sd_event_source *s, uint64_t *usec);
int sd_event_source_set_time(sd_event_source *s, uint64_t usec);
int sd_event_source_set_time_accuracy(sd_event_source *s, uint64_t usec);
int sd_event_source_get_time_accuracy(sd_event_source *s, uint64_t *usec);
int sd_event_source_get_signal(sd_event_source *s);
int sd_event_source_get_child_pid(sd_event_source *s, pid_t *pid);

#endif
