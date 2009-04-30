/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/domain.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/parseaddr.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/native-common.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/dynarray.h>
#include <pulsecore/modargs.h>
#include <pulsecore/avahi-wrap.h>
#include <pulsecore/endianmacros.h>
#include <pulsecore/protocol-native.h>

#include "module-zeroconf-publish-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("mDNS/DNS-SD Service Publisher");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

#define SERVICE_TYPE_SINK "_pulse-sink._tcp"
#define SERVICE_TYPE_SOURCE "_pulse-source._tcp"
#define SERVICE_TYPE_SERVER "_pulse-server._tcp"
#define SERVICE_SUBTYPE_SINK_HARDWARE "_hardware._sub."SERVICE_TYPE_SINK
#define SERVICE_SUBTYPE_SINK_VIRTUAL "_virtual._sub."SERVICE_TYPE_SINK
#define SERVICE_SUBTYPE_SOURCE_HARDWARE "_hardware._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_VIRTUAL "_virtual._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_MONITOR "_monitor._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_NON_MONITOR "_non-monitor._sub."SERVICE_TYPE_SOURCE

static const char* const valid_modargs[] = {
    NULL
};

enum service_subtype {
    SUBTYPE_HARDWARE,
    SUBTYPE_VIRTUAL,
    SUBTYPE_MONITOR
};

struct service {
    struct userdata *userdata;
    AvahiEntryGroup *entry_group;
    char *service_name;
    pa_object *device;
    enum service_subtype subtype;
};

struct userdata {
    pa_core *core;
    pa_module *module;

    AvahiPoll *avahi_poll;
    AvahiClient *client;

    pa_hashmap *services;
    char *service_name;

    AvahiEntryGroup *main_entry_group;

    pa_hook_slot *sink_new_slot, *source_new_slot, *sink_unlink_slot, *source_unlink_slot, *sink_changed_slot, *source_changed_slot;

    pa_native_protocol *native;
};

static void get_service_data(struct service *s, pa_sample_spec *ret_ss, pa_channel_map *ret_map, const char **ret_name, const char **ret_description, enum service_subtype *ret_subtype) {
    pa_assert(s);
    pa_assert(ret_ss);
    pa_assert(ret_description);
    pa_assert(ret_subtype);

    if (pa_sink_isinstance(s->device)) {
        pa_sink *sink = PA_SINK(s->device);

        *ret_ss = sink->sample_spec;
        *ret_map = sink->channel_map;
        *ret_name = sink->name;
        *ret_description = pa_strnull(pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION));
        *ret_subtype = sink->flags & PA_SINK_HARDWARE ? SUBTYPE_HARDWARE : SUBTYPE_VIRTUAL;

    } else if (pa_source_isinstance(s->device)) {
        pa_source *source = PA_SOURCE(s->device);

        *ret_ss = source->sample_spec;
        *ret_map = source->channel_map;
        *ret_name = source->name;
        *ret_description = pa_strnull(pa_proplist_gets(source->proplist, PA_PROP_DEVICE_DESCRIPTION));
        *ret_subtype = source->monitor_of ? SUBTYPE_MONITOR : (source->flags & PA_SOURCE_HARDWARE ? SUBTYPE_HARDWARE : SUBTYPE_VIRTUAL);

    } else
        pa_assert_not_reached();
}

static AvahiStringList* txt_record_server_data(pa_core *c, AvahiStringList *l) {
    char s[128];

    pa_assert(c);

    l = avahi_string_list_add_pair(l, "server-version", PACKAGE_NAME" "PACKAGE_VERSION);
    l = avahi_string_list_add_pair(l, "user-name", pa_get_user_name(s, sizeof(s)));
    l = avahi_string_list_add_pair(l, "fqdn", pa_get_fqdn(s, sizeof(s)));
    l = avahi_string_list_add_printf(l, "cookie=0x%08x", c->cookie);

    return l;
}

static int publish_service(struct service *s);

static void service_entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    struct service *s = userdata;

    pa_assert(s);

    switch (state) {

        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            pa_log_info("Successfully established service %s.", s->service_name);
            break;

        case AVAHI_ENTRY_GROUP_COLLISION: {
            char *t;

            t = avahi_alternative_service_name(s->service_name);
            pa_log_info("Name collision, renaming %s to %s.", s->service_name, t);
            pa_xfree(s->service_name);
            s->service_name = t;

            publish_service(s);
            break;
        }

        case AVAHI_ENTRY_GROUP_FAILURE: {
            pa_log("Failed to register service: %s", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));

            avahi_entry_group_free(g);
            s->entry_group = NULL;

            break;
        }

        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            ;
    }
}

static void service_free(struct service *s);

static uint16_t compute_port(struct userdata *u) {
    pa_strlist *i;

    pa_assert(u);

    for (i = pa_native_protocol_servers(u->native); i; i = pa_strlist_next(i)) {
        pa_parsed_address a;

        if (pa_parse_address(pa_strlist_data(i), &a) >= 0 &&
            (a.type == PA_PARSED_ADDRESS_TCP4 ||
             a.type == PA_PARSED_ADDRESS_TCP6 ||
             a.type == PA_PARSED_ADDRESS_TCP_AUTO) &&
            a.port > 0) {

            pa_xfree(a.path_or_host);
            return a.port;
        }

        pa_xfree(a.path_or_host);
    }

    return PA_NATIVE_DEFAULT_PORT;
}

static int publish_service(struct service *s) {
    int r = -1;
    AvahiStringList *txt = NULL;
    const char *description = NULL, *name = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    enum service_subtype subtype;

    const char * const subtype_text[] = {
        [SUBTYPE_HARDWARE] = "hardware",
        [SUBTYPE_VIRTUAL] = "virtual",
        [SUBTYPE_MONITOR] = "monitor"
    };

    pa_assert(s);

    if (!s->userdata->client || avahi_client_get_state(s->userdata->client) != AVAHI_CLIENT_S_RUNNING)
        return 0;

    if (!s->entry_group) {
        if (!(s->entry_group = avahi_entry_group_new(s->userdata->client, service_entry_group_callback, s))) {
            pa_log("avahi_entry_group_new(): %s", avahi_strerror(avahi_client_errno(s->userdata->client)));
            goto finish;
        }
    } else
        avahi_entry_group_reset(s->entry_group);

    txt = txt_record_server_data(s->userdata->core, txt);

    get_service_data(s, &ss, &map, &name, &description, &subtype);
    txt = avahi_string_list_add_pair(txt, "device", name);
    txt = avahi_string_list_add_printf(txt, "rate=%u", ss.rate);
    txt = avahi_string_list_add_printf(txt, "channels=%u", ss.channels);
    txt = avahi_string_list_add_pair(txt, "format", pa_sample_format_to_string(ss.format));
    txt = avahi_string_list_add_pair(txt, "channel_map", pa_channel_map_snprint(cm, sizeof(cm), &map));
    txt = avahi_string_list_add_pair(txt, "subtype", subtype_text[subtype]);

    if (avahi_entry_group_add_service_strlst(
                s->entry_group,
                AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                0,
                s->service_name,
                pa_sink_isinstance(s->device) ? SERVICE_TYPE_SINK : SERVICE_TYPE_SOURCE,
                NULL,
                NULL,
                compute_port(s->userdata),
                txt) < 0) {

        pa_log("avahi_entry_group_add_service_strlst(): %s", avahi_strerror(avahi_client_errno(s->userdata->client)));
        goto finish;
    }

    if (avahi_entry_group_add_service_subtype(
                s->entry_group,
                AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                0,
                s->service_name,
                pa_sink_isinstance(s->device) ? SERVICE_TYPE_SINK : SERVICE_TYPE_SOURCE,
                NULL,
                pa_sink_isinstance(s->device) ? (subtype == SUBTYPE_HARDWARE ? SERVICE_SUBTYPE_SINK_HARDWARE : SERVICE_SUBTYPE_SINK_VIRTUAL) :
                (subtype == SUBTYPE_HARDWARE ? SERVICE_SUBTYPE_SOURCE_HARDWARE : (subtype == SUBTYPE_VIRTUAL ? SERVICE_SUBTYPE_SOURCE_VIRTUAL : SERVICE_SUBTYPE_SOURCE_MONITOR))) < 0) {

        pa_log("avahi_entry_group_add_service_subtype(): %s", avahi_strerror(avahi_client_errno(s->userdata->client)));
        goto finish;
    }

    if (pa_source_isinstance(s->device) && subtype != SUBTYPE_MONITOR) {
        if (avahi_entry_group_add_service_subtype(
                    s->entry_group,
                    AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                    0,
                    s->service_name,
                    SERVICE_TYPE_SOURCE,
                    NULL,
                    SERVICE_SUBTYPE_SOURCE_NON_MONITOR) < 0) {

            pa_log("avahi_entry_group_add_service_subtype(): %s", avahi_strerror(avahi_client_errno(s->userdata->client)));
            goto finish;
        }
    }

    if (avahi_entry_group_commit(s->entry_group) < 0) {
        pa_log("avahi_entry_group_commit(): %s", avahi_strerror(avahi_client_errno(s->userdata->client)));
        goto finish;
    }

    r = 0;
    pa_log_debug("Successfully created entry group for %s.", s->service_name);

finish:

    /* Remove this service */
    if (r < 0)
        service_free(s);

    avahi_string_list_free(txt);

    return r;
}

static struct service *get_service(struct userdata *u, pa_object *device) {
    struct service *s;
    char hn[64], un[64];
    const char *n;

    pa_assert(u);
    pa_object_assert_ref(device);

    if ((s = pa_hashmap_get(u->services, device)))
        return s;

    s = pa_xnew(struct service, 1);
    s->userdata = u;
    s->entry_group = NULL;
    s->device = device;

    if (pa_sink_isinstance(device)) {
        if (!(n = pa_proplist_gets(PA_SINK(device)->proplist, PA_PROP_DEVICE_DESCRIPTION)))
            n = PA_SINK(device)->name;
    } else {
        if (!(n = pa_proplist_gets(PA_SOURCE(device)->proplist, PA_PROP_DEVICE_DESCRIPTION)))
            n = PA_SOURCE(device)->name;
    }

    s->service_name = pa_truncate_utf8(pa_sprintf_malloc("%s@%s: %s",
                                                         pa_get_user_name(un, sizeof(un)),
                                                         pa_get_host_name(hn, sizeof(hn)),
                                                         n),
                                       AVAHI_LABEL_MAX-1);

    pa_hashmap_put(u->services, s->device, s);

    return s;
}

static void service_free(struct service *s) {
    pa_assert(s);

    pa_hashmap_remove(s->userdata->services, s->device);

    if (s->entry_group) {
        pa_log_debug("Removing entry group for %s.", s->service_name);
        avahi_entry_group_free(s->entry_group);
    }

    pa_xfree(s->service_name);
    pa_xfree(s);
}

static pa_bool_t shall_ignore(pa_object *o) {
    pa_object_assert_ref(o);

    if (pa_sink_isinstance(o))
        return !!(PA_SINK(o)->flags & PA_SINK_NETWORK);

    if (pa_source_isinstance(o))
        return PA_SOURCE(o)->monitor_of || (PA_SOURCE(o)->flags & PA_SOURCE_NETWORK);

    pa_assert_not_reached();
}

static pa_hook_result_t device_new_or_changed_cb(pa_core *c, pa_object *o, struct userdata *u) {
    pa_assert(c);
    pa_object_assert_ref(o);

    if (!shall_ignore(o))
        publish_service(get_service(u, o));

    return PA_HOOK_OK;
}

static pa_hook_result_t device_unlink_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct service *s;

    pa_assert(c);
    pa_object_assert_ref(o);

    if ((s = pa_hashmap_get(u->services, o)))
        service_free(s);

    return PA_HOOK_OK;
}

static int publish_main_service(struct userdata *u);

static void main_entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata) {
    struct userdata *u = userdata;
    pa_assert(u);

    switch (state) {

        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            pa_log_info("Successfully established main service.");
            break;

        case AVAHI_ENTRY_GROUP_COLLISION: {
            char *t;

            t = avahi_alternative_service_name(u->service_name);
            pa_log_info("Name collision: renaming main service %s to %s.", u->service_name, t);
            pa_xfree(u->service_name);
            u->service_name = t;

            publish_main_service(u);
            break;
        }

        case AVAHI_ENTRY_GROUP_FAILURE: {
            pa_log("Failed to register main service: %s", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));

            avahi_entry_group_free(g);
            u->main_entry_group = NULL;
            break;
        }

        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            break;
    }
}

static int publish_main_service(struct userdata *u) {
    AvahiStringList *txt = NULL;
    int r = -1;

    pa_assert(u);

    if (!u->main_entry_group) {
        if (!(u->main_entry_group = avahi_entry_group_new(u->client, main_entry_group_callback, u))) {
            pa_log("avahi_entry_group_new() failed: %s", avahi_strerror(avahi_client_errno(u->client)));
            goto fail;
        }
    } else
        avahi_entry_group_reset(u->main_entry_group);

    txt = txt_record_server_data(u->core, txt);

    if (avahi_entry_group_add_service_strlst(
                u->main_entry_group,
                AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                0,
                u->service_name,
                SERVICE_TYPE_SERVER,
                NULL,
                NULL,
                compute_port(u),
                txt) < 0) {

        pa_log("avahi_entry_group_add_service_strlst() failed: %s", avahi_strerror(avahi_client_errno(u->client)));
        goto fail;
    }

    if (avahi_entry_group_commit(u->main_entry_group) < 0) {
        pa_log("avahi_entry_group_commit() failed: %s", avahi_strerror(avahi_client_errno(u->client)));
        goto fail;
    }

    r = 0;

fail:
    avahi_string_list_free(txt);

    return r;
}

static int publish_all_services(struct userdata *u) {
    pa_sink *sink;
    pa_source *source;
    int r = -1;
    uint32_t idx;

    pa_assert(u);

    pa_log_debug("Publishing services in Zeroconf");

    for (sink = PA_SINK(pa_idxset_first(u->core->sinks, &idx)); sink; sink = PA_SINK(pa_idxset_next(u->core->sinks, &idx)))
        if (!shall_ignore(PA_OBJECT(sink)))
            publish_service(get_service(u, PA_OBJECT(sink)));

    for (source = PA_SOURCE(pa_idxset_first(u->core->sources, &idx)); source; source = PA_SOURCE(pa_idxset_next(u->core->sources, &idx)))
        if (!shall_ignore(PA_OBJECT(source)))
            publish_service(get_service(u, PA_OBJECT(source)));

    if (publish_main_service(u) < 0)
        goto fail;

    r = 0;

fail:
    return r;
}

static void unpublish_all_services(struct userdata *u, pa_bool_t rem) {
    void *state = NULL;
    struct service *s;

    pa_assert(u);

    pa_log_debug("Unpublishing services in Zeroconf");

    while ((s = pa_hashmap_iterate(u->services, &state, NULL))) {
        if (s->entry_group) {
            if (rem) {
                pa_log_debug("Removing entry group for %s.", s->service_name);
                avahi_entry_group_free(s->entry_group);
                s->entry_group = NULL;
            } else {
                avahi_entry_group_reset(s->entry_group);
                pa_log_debug("Resetting entry group for %s.", s->service_name);
            }
        }
    }

    if (u->main_entry_group) {
        if (rem) {
            pa_log_debug("Removing main entry group.");
            avahi_entry_group_free(u->main_entry_group);
            u->main_entry_group = NULL;
        } else {
            avahi_entry_group_reset(u->main_entry_group);
            pa_log_debug("Resetting main entry group.");
        }
    }
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(c);
    pa_assert(u);

    u->client = c;

    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            publish_all_services(u);
            break;

        case AVAHI_CLIENT_S_COLLISION:
            pa_log_debug("Host name collision");
            unpublish_all_services(u, FALSE);
            break;

        case AVAHI_CLIENT_FAILURE:
            if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
                int error;

                pa_log_debug("Avahi daemon disconnected.");

                unpublish_all_services(u, TRUE);
                avahi_client_free(u->client);

                if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error))) {
                    pa_log("avahi_client_new() failed: %s", avahi_strerror(error));
                    pa_module_unload_request(u->module, TRUE);
                }
            }

            break;

        default: ;
    }
}

int pa__init(pa_module*m) {

    struct userdata *u;
    pa_modargs *ma = NULL;
    char hn[256], un[256];
    int error;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->native = pa_native_protocol_get(u->core);

    u->avahi_poll = pa_avahi_poll_new(m->core->mainloop);

    u->services = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->sink_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_LATE, (pa_hook_cb_t) device_new_or_changed_cb, u);
    u->sink_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) device_new_or_changed_cb, u);
    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) device_unlink_cb, u);
    u->source_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_LATE, (pa_hook_cb_t) device_new_or_changed_cb, u);
    u->source_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) device_new_or_changed_cb, u);
    u->source_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) device_unlink_cb, u);

    u->main_entry_group = NULL;

    u->service_name = pa_truncate_utf8(pa_sprintf_malloc("%s@%s", pa_get_user_name(un, sizeof(un)), pa_get_host_name(hn, sizeof(hn))), AVAHI_LABEL_MAX);

    if (!(u->client = avahi_client_new(u->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, u, &error))) {
        pa_log("avahi_client_new() failed: %s", avahi_strerror(error));
        goto fail;
    }

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata*u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->services) {
        struct service *s;

        while ((s = pa_hashmap_first(u->services)))
            service_free(s);

        pa_hashmap_free(u->services, NULL, NULL);
    }

    if (u->sink_new_slot)
        pa_hook_slot_free(u->sink_new_slot);
    if (u->source_new_slot)
        pa_hook_slot_free(u->source_new_slot);
    if (u->sink_changed_slot)
        pa_hook_slot_free(u->sink_changed_slot);
    if (u->source_changed_slot)
        pa_hook_slot_free(u->source_changed_slot);
    if (u->sink_unlink_slot)
        pa_hook_slot_free(u->sink_unlink_slot);
    if (u->source_unlink_slot)
        pa_hook_slot_free(u->source_unlink_slot);

    if (u->main_entry_group)
        avahi_entry_group_free(u->main_entry_group);

    if (u->client)
        avahi_client_free(u->client);

    if (u->avahi_poll)
        pa_avahi_poll_free(u->avahi_poll);

    pa_xfree(u->service_name);
    pa_xfree(u);
}
