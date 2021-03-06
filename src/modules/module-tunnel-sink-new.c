/***
    This file is part of PulseAudio.

    Copyright 2013 Alexander Couzens

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/context.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulse/stream.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

#include "module-tunnel-sink-new-symdef.h"

PA_MODULE_AUTHOR("Alexander Couzens");
PA_MODULE_DESCRIPTION(_("Create a network sink which connects via a stream to a remote pulseserver"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(_("sink_name=<name of sink>"));

#define DEFAULT_SINK_NAME "remote_sink"

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)

/* libpulse callbacks */
static void stream_state_callback(pa_stream *stream, void *userdata);
static void context_state_callback(pa_context *c, void *userdata);

struct userdata {
    pa_module *module;

    pa_sink *sink;
    pa_rtpoll *rtpoll;
    pa_thread_mq thread_mq;
    pa_thread *thread;

    pa_memchunk memchunk;

    pa_bool_t auto_desc;

    unsigned channels;
    pa_usec_t block_usec;
    pa_usec_t timestamp;

    // libpulse context
    pa_context *context;
    pa_stream *stream;

    bool connected;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "remote_server",
    NULL,
};

enum {
    SINK_MESSAGE_PASS_SOCKET = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_RIP_SOCKET
};

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Tunnelstream: Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for(;;)
    {
        int ret;
        const void *p;
        pa_usec_t now = 0;

        size_t writeable = 0;

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            pa_sink_process_rewind(u->sink, 0);

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            now = pa_rtclock_now();

        if (u->connected &&
                PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream)) &&
                PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            /* TODO: use IS_RUNNING + cork stream */

            if (pa_stream_is_corked(u->stream)) {
                pa_stream_cork(u->stream, 0, NULL, NULL);
            } else {
                writeable = pa_stream_writable_size(u->stream);
                if (writeable > 0) {
                    if (u->memchunk.length <= 0)
                        pa_sink_render(u->sink, writeable, &u->memchunk);

                    pa_assert(u->memchunk.length > 0);


                    /* we have new data to write */
                    p = (const uint8_t *) pa_memblock_acquire(u->memchunk.memblock);
                    ret = pa_stream_write(u->stream,
                                        ((uint8_t*) p + u->memchunk.index),         /**< The data to write */
                                        u->memchunk.length,            /**< The length of the data to write in bytes */
                                        NULL,     /**< A cleanup routine for the data or NULL to request an internal copy */
                                        0,          /**< Offset for seeking, must be 0 for upload streams */
                                        PA_SEEK_RELATIVE      /**< Seek mode, must be PA_SEEK_RELATIVE for upload streams */
                                        );
                    pa_memblock_release(u->memchunk.memblock);
                    pa_memblock_unref(u->memchunk.memblock);
                    pa_memchunk_reset(&u->memchunk);

                    if (ret != 0) {
                        /* TODO: we should consider a state change or is that already done ? */
                        pa_log_warn("Could not write data into the stream ... ret = %i", ret);
                    }
                }
            }
        }
        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

    }
fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->module->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

static void stream_state_callback(pa_stream *stream, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(stream == u->stream);

    switch(pa_stream_get_state(stream)) {
        case PA_STREAM_FAILED:
            pa_log_debug("Context failed.");
            pa_stream_unref(stream);
            u->stream = NULL;
            /* TODO: think about killing the context or should we just try again a creationg of a stream ? */
            break;
        case PA_STREAM_TERMINATED:
            pa_log_debug("Context terminated.");
            pa_stream_unref(stream);
            u->stream = NULL;
            break;
        default:
            break;
    }
}

static void context_state_callback(pa_context *c, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(u->context == c);

    switch(pa_context_get_state(c)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            pa_log_debug("Connection unconnected");
            break;
        case PA_CONTEXT_READY: {
            pa_proplist *proplist;
            pa_buffer_attr bufferattr;

            pa_log_debug("Connection successful. Creating stream.");
            pa_assert(!u->stream);

            proplist = pa_proplist_new();
            pa_assert(proplist);


            u->stream = pa_stream_new_with_proplist(u->context,
                                                    "mod-tunnelstream",
                                                    &u->sink->sample_spec,
                                                    &u->sink->channel_map,
                                                    proplist);

            pa_proplist_free(proplist);


            memset(&bufferattr, 0, sizeof(pa_buffer_attr));

            bufferattr.maxlength = (uint32_t) - 1;
            bufferattr.minreq = (uint32_t) - 1;
            bufferattr.prebuf = (uint32_t) - 1;
            bufferattr.tlength = (uint32_t) - 1;

            pa_stream_set_state_callback(u->stream, stream_state_callback, userdata);
            pa_stream_connect_playback(u->stream,
                                       NULL,
                                       &bufferattr,
                                       PA_STREAM_START_CORKED | PA_STREAM_AUTO_TIMING_UPDATE,
                                       NULL,
                                       NULL);

            pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_PASS_SOCKET, NULL, 0, NULL, NULL);
            break;
        }
        case PA_CONTEXT_FAILED:
            pa_log_debug("Context failed.");
            pa_context_unref(u->context);
            u->context = NULL;
            u->connected = false;
            break;

        case PA_CONTEXT_TERMINATED:
            pa_log_debug("Context terminated.");
            pa_context_unref(u->context);
            u->context = NULL;
            u->connected = false;
            break;
        default:
            break;
    }
}

static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            int negative;
            pa_usec_t remote_latency;

            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if(pa_stream_get_latency(u->stream, &remote_latency, &negative) < 0) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =
                /* Add the latency from libpulse */
                remote_latency;
                /* do we have to add more latency here ? */
            return 0;
        }
        case SINK_MESSAGE_PASS_SOCKET: {
            u->connected = true;
            return 0;
        }
    }
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_sink_new_data sink_data;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_proplist *proplist = NULL;
    const char *remote_server = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    remote_server = pa_modargs_get_value(ma, "remote_server", NULL);
    if (!remote_server) {
        pa_log("No remote_server given!");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;
    pa_memchunk_reset(&u->memchunk);
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;

    pa_sink_new_data_set_name(&sink_data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);

    /* TODO: set DEVICE CLASS */
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "abstract");
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Remote Sink of _replace_me"));

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }
    /* TODO: check PA_SINK_LATENCY + PA_SINK_DYNAMIC_LATENCY */
    if (!(u->sink = pa_sink_new(m->core, &sink_data, (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY|PA_SINK_NETWORK)))) {
        pa_log("Failed to create sink.");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    pa_sink_new_data_done(&sink_data);
    u->sink->userdata = u;

    /* callbacks */
    u->sink->parent.process_msg = sink_process_msg_cb;


    /* set thread queue */
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    /* TODO: latency / rewind
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->block_usec = BLOCK_USEC;
    nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);
    pa_sink_set_max_rewind(u->sink, nbytes);
    pa_sink_set_max_request(u->sink, nbytes);
    pa_sink_set_latency_range(u->sink, 0, BLOCK_USEC); */

    /* TODO: think about volume stuff remote<--stream--source */
    proplist = pa_proplist_new();
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, _("PulseAudio mod-tunnelstream"));
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "mod-tunnelstream");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ICON_NAME, "audio-card");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PACKAGE_VERSION);

    /* init libpulse */
    if (!(u->context = pa_context_new_with_proplist(m->core->mainloop,
                                              "tunnelstream",
                                              proplist))) {
        pa_log("Failed to create libpulse context");
        goto fail;
    }

    pa_proplist_free(proplist);

    pa_context_set_state_callback(u->context, context_state_callback, u);
    if (pa_context_connect(u->context,
                          remote_server,
                          PA_CONTEXT_NOFAIL | PA_CONTEXT_NOAUTOSPAWN,
                          NULL) < 0) {
        pa_log("Failed to connect libpulse context");
        goto fail;
    }

    if (!(u->thread = pa_thread_new("tunnelstream-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);
    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (proplist)
        pa_proplist_free(proplist);

    pa__done(m);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->stream)
        pa_stream_disconnect(u->stream);

    if (u->context)
        pa_context_disconnect(u->context);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->sink)
        pa_sink_unref(u->sink);

    pa_xfree(u);
}
