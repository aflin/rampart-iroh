/*
 * rampart-iroh.c - Rampart JavaScript module for iroh P2P networking
 *
 * Bridges iroh's P2P networking (Rust via C FFI) with rampart's Duktape
 * JavaScript engine. Provides Node.js-style event emitter API.
 *
 * Copyright (C) 2026 Aaron Flin
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "rampart.h"
#include "event2/event.h"
#include "iroh_libevent.h"

/* from rampart-utils.c, not declared in rampart.h */
extern void duk_rp_hexToBuf(duk_context *ctx, duk_idx_t idx);

/* ======================================================================
 * Type Definitions
 * ====================================================================== */

typedef enum {
    IROH_OBJ_ENDPOINT,
    IROH_OBJ_CONNECTION,
    IROH_OBJ_STREAM,
    IROH_OBJ_GOSSIP,
    IROH_OBJ_GOSSIP_TOPIC,
    IROH_OBJ_BLOBS,
    IROH_OBJ_DOCS,
} iroh_obj_type;

typedef enum {
    IROH_OP_ENDPOINT_CREATE,
    IROH_OP_ENDPOINT_WAIT_ONLINE,
    IROH_OP_ENDPOINT_CLOSE,
    IROH_OP_ENDPOINT_ACCEPT,
    IROH_OP_CONNECT,
    IROH_OP_OPEN_BI,
    IROH_OP_ACCEPT_BI,
    IROH_OP_STREAM_READ,
    IROH_OP_STREAM_WRITE,
    IROH_OP_RECV_DATAGRAM,
    IROH_OP_GOSSIP_CREATE,
    IROH_OP_GOSSIP_SUBSCRIBE,
    IROH_OP_GOSSIP_BROADCAST,
    IROH_OP_GOSSIP_RECV,
    IROH_OP_BLOBS_CREATE,
    IROH_OP_BLOBS_ADD_BYTES,
    IROH_OP_BLOBS_ADD_FILE,
    IROH_OP_BLOBS_READ,
    IROH_OP_BLOBS_DOWNLOAD,
    IROH_OP_DOCS_CREATE,
    IROH_OP_DOCS_CREATE_AUTHOR,
    IROH_OP_DOCS_CREATE_DOC,
    IROH_OP_DOCS_SET,
    IROH_OP_DOCS_GET,
    IROH_OP_DOCS_GET_LATEST,
    IROH_OP_DOCS_GET_ALL,
    IROH_OP_DOCS_DELETE,
    IROH_OP_DOCS_CREATE_TICKET,
    IROH_OP_DOCS_JOIN,
} iroh_op_type;

#define RPIROH struct rp_iroh_ctx_s
#define RPIROH_ASYNC struct rp_iroh_async_s

RPIROH;
RPIROH_ASYNC;

RPIROH {
    duk_context        *ctx;
    void               *thisptr;
    struct event_base  *base;
    uint32_t            thiskey;
    uint32_t            next_cb_id;
    iroh_obj_type       obj_type;
    uint16_t            flags;

    IrohEndpoint       *endpoint;
    IrohConnection     *conn;
    IrohSendStream     *send;
    IrohRecvStream     *recv;
    IrohGossip         *gossip;
    IrohGossipTopic    *topic_handle;
    IrohBlobStore      *blob_store;
    IrohBlobsProtocol  *blobs_proto;
    IrohDocs           *docs;
    IrohRouter         *router;

    RPIROH             *parent;

    RPIROH_ASYNC       **pending_ops;
    uint16_t            npending;
    uint16_t            pending_alloc;
};

RPIROH_ASYNC {
    RPIROH             *ictx;
    IrohAsyncHandle    *handle;
    iroh_op_type        op_type;
    struct event       *ev;
    void               *aux;
};

#define RPIROH_FLAG_CLOSED          0x0001
#define RPIROH_FLAG_ACCEPTING       0x0002
#define RPIROH_FLAG_READING         0x0004
#define RPIROH_FLAG_STREAM_ACCEPT   0x0008
#define RPIROH_FLAG_FINISHED        0x0010
#define RPIROH_FLAG_AUTO_CONNECT    0x0020
#define RPIROH_FLAG_DATAGRAM_RECV   0x0040
#define RPIROH_FLAG_WRITING         0x0080
#define RPIROH_FLAG_FINISH_PENDING  0x0100

/* ======================================================================
 * Globals
 * ====================================================================== */

static volatile uint32_t iroh_this_id = 0;
static volatile uint32_t iroh_evcb_id = 0;
static int rp_iroh_is_init = 0;

/* ======================================================================
 * Forward Declarations
 * ====================================================================== */

static void iroh_dispatch_result(RPIROH_ASYNC *aop, IrohAsyncState state);
static void iroh_start_accept_loop(RPIROH *ictx);
static void iroh_start_stream_accept_loop(RPIROH *ictx);
static void iroh_start_read_loop(RPIROH *ictx);
static void iroh_start_datagram_loop(RPIROH *ictx);
static void iroh_start_gossip_recv_loop(RPIROH *ictx);
static void iroh_cleanup(duk_context *ctx, RPIROH *ictx, int docb);
static void push_new_connection(duk_context *ctx, RPIROH *parent, IrohConnection *conn);
static void push_new_stream(duk_context *ctx, RPIROH *parent, IrohSendStream *send, IrohRecvStream *recv);
static RPIROH_ASYNC *iroh_start_async(RPIROH *ictx, IrohAsyncHandle *handle, iroh_op_type op, void *aux);
static void iroh_do_finish(RPIROH *ictx);

/* ======================================================================
 * GC Protection Helpers
 * Stores JS objects in global stash to prevent garbage collection.
 * ====================================================================== */

static int iroh_put_gs_object(duk_context *ctx, const char *objname, const char *key)
{
    duk_idx_t val_idx, stash_idx, obj_idx;

    val_idx = duk_get_top_index(ctx);
    duk_push_global_stash(ctx);
    stash_idx = duk_get_top_index(ctx);

    if (key == NULL) {
        if (!duk_is_object(ctx, val_idx))
            RP_THROW(ctx, "iroh: internal error in put_gs_object");
        duk_dup(ctx, val_idx);
        duk_put_prop_string(ctx, stash_idx, objname);
        duk_remove(ctx, stash_idx);
        return 1;
    }

    if (!duk_get_prop_string(ctx, stash_idx, objname)) {
        duk_pop(ctx);
        duk_push_object(ctx);
        duk_dup(ctx, -1);
        duk_put_prop_string(ctx, stash_idx, objname);
    }
    obj_idx = duk_get_top_index(ctx);
    duk_dup(ctx, val_idx);
    duk_put_prop_string(ctx, obj_idx, key);
    duk_remove(ctx, obj_idx);
    duk_remove(ctx, stash_idx);
    return 1;
}

static int iroh_del_gs_object(duk_context *ctx, const char *objname, const char *key)
{
    duk_idx_t stash_idx, obj_idx;
    int ret = 1;

    duk_push_global_stash(ctx);
    stash_idx = duk_get_top_index(ctx);

    if (key == NULL) {
        if (duk_get_prop_string(ctx, stash_idx, objname))
            duk_del_prop_string(ctx, stash_idx, objname);
        else
            ret = 0;
        duk_remove(ctx, stash_idx);
        return ret;
    }

    if (!duk_get_prop_string(ctx, stash_idx, objname)) {
        duk_remove(ctx, stash_idx);
        return 0;
    }
    obj_idx = duk_get_top_index(ctx);
    if (duk_get_prop_string(ctx, obj_idx, key))
        duk_del_prop_string(ctx, obj_idx, key);
    else
        ret = 0;
    duk_remove(ctx, obj_idx);
    duk_remove(ctx, stash_idx);
    return ret;
}

/* ======================================================================
 * Event Emitter
 * ====================================================================== */

static void iroh_ev_on(duk_context *ctx, const char *fname, const char *evname,
                       duk_idx_t cb_idx, duk_idx_t this_idx)
{
    duk_idx_t tidx, top = duk_get_top(ctx);
    int id = (int)(iroh_evcb_id++);

    cb_idx = duk_normalize_index(ctx, cb_idx);

    if (this_idx == DUK_INVALID_INDEX) {
        duk_push_this(ctx);
        tidx = duk_get_top_index(ctx);
    } else {
        tidx = duk_normalize_index(ctx, this_idx);
    }

    if (!duk_is_function(ctx, cb_idx))
        RP_THROW(ctx, "%s: argument must be a Function (listener)", fname);

    duk_get_prop_string(ctx, tidx, "_events");
    if (!duk_get_prop_string(ctx, -1, evname)) {
        duk_pop(ctx);
        duk_push_object(ctx);
        duk_dup(ctx, -1);
        duk_put_prop_string(ctx, -3, evname);
    }

    if (duk_get_prop_string(ctx, cb_idx, DUK_HIDDEN_SYMBOL("evcb_id"))) {
        id = duk_get_int(ctx, -1);
        if (duk_has_prop(ctx, -2)) {
            duk_set_top(ctx, top);
            return;
        }
    } else {
        duk_pop(ctx);
    }

    duk_push_int(ctx, id);
    duk_dup(ctx, cb_idx);
    duk_push_int(ctx, id);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("evcb_id"));
    duk_put_prop(ctx, -3);
    duk_set_top(ctx, top);
}

static int iroh_do_callback(duk_context *ctx, const char *ev_s, duk_idx_t nargs)
{
    duk_idx_t j, errobj = -1;
    duk_idx_t exit_top = duk_get_top(ctx) - 1 - nargs;
    int nerrorcb = -1;

    if (!strcmp("error", ev_s)) {
        const char *errstr = "unspecified error";
        nerrorcb = 0;
        errobj = duk_get_top_index(ctx);
        if (nargs > 0 && duk_is_string(ctx, -1))
            errstr = duk_get_string(ctx, -nargs);
        duk_push_error_object(ctx, DUK_ERR_ERROR, "%s", errstr);
        duk_replace(ctx, -2);
    }

    duk_get_prop_string(ctx, -1 - nargs, "_events");
    if (duk_get_prop_string(ctx, -1, ev_s)) {
        duk_enum(ctx, -1, 0);
        while (duk_next(ctx, -1, 1)) {
            if (nerrorcb > -1) nerrorcb++;
            if (duk_has_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("once"))) {
                duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("once"));
                duk_pull(ctx, -2);
                duk_del_prop(ctx, -4);
            } else {
                duk_remove(ctx, -2);
            }
            duk_dup(ctx, -5 - nargs);
            for (j = 0; j < nargs; j++)
                duk_dup(ctx, -5 - nargs);
            if (duk_pcall_method(ctx, nargs) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in %s callback:\n%s\n", ev_s, errmsg);
                duk_pop_2(ctx);
            } else {
                duk_pop(ctx);
            }
        }
    }

    if (nerrorcb == 0) {
        duk_pull(ctx, errobj);
        duk_get_prop_string(ctx, -1, "stack");
        fprintf(stderr, "Uncaught Async %s\n", duk_get_string(ctx, -1));
    }

    duk_set_top(ctx, exit_top);
    return 0;
}

/* fire event with 0 args, using ictx->thisptr as 'this' */
static void iroh_fire_event(RPIROH *ictx, const char *evname)
{
    duk_push_heapptr(ictx->ctx, ictx->thisptr);
    iroh_do_callback(ictx->ctx, evname, 0);
}

/* fire error event with message string */
static void iroh_fire_error(RPIROH *ictx, const char *msg)
{
    duk_context *ctx = ictx->ctx;
    duk_push_heapptr(ctx, ictx->thisptr);
    duk_push_string(ctx, msg ? msg : "unknown iroh error");
    iroh_do_callback(ctx, "error", 1);
}

/* ======================================================================
 * JS-callable event emitter methods: on, once, off, trigger
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_on(duk_context *ctx)
{
    const char *evname = REQUIRE_STRING(ctx, 0,
        "iroh.on: first argument must be a String (event name)");
    RPIROH *ictx;

    if (!duk_is_function(ctx, 1))
        RP_THROW(ctx, "iroh.on: second argument must be a function");

    duk_push_this(ctx);
    iroh_ev_on(ctx, "iroh.on", evname, 1, -1);

    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("ictx"));
    ictx = (RPIROH *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (ictx && !(ictx->flags & RPIROH_FLAG_CLOSED)) {
        if (ictx->obj_type == IROH_OBJ_ENDPOINT && strcmp(evname, "connection") == 0) {
            if (ictx->endpoint && !(ictx->flags & RPIROH_FLAG_ACCEPTING))
                iroh_start_accept_loop(ictx);
        } else if (ictx->obj_type == IROH_OBJ_CONNECTION && strcmp(evname, "stream") == 0) {
            if (ictx->conn && !(ictx->flags & RPIROH_FLAG_STREAM_ACCEPT))
                iroh_start_stream_accept_loop(ictx);
        } else if (ictx->obj_type == IROH_OBJ_STREAM && strcmp(evname, "data") == 0) {
            if (ictx->recv && !(ictx->flags & RPIROH_FLAG_READING))
                iroh_start_read_loop(ictx);
        } else if (ictx->obj_type == IROH_OBJ_CONNECTION && strcmp(evname, "datagram") == 0) {
            if (ictx->conn && !(ictx->flags & RPIROH_FLAG_DATAGRAM_RECV))
                iroh_start_datagram_loop(ictx);
        }
    }

    return 1;
}

static duk_ret_t duk_rp_iroh_once(duk_context *ctx)
{
    const char *evname = REQUIRE_STRING(ctx, 0,
        "iroh.once: first argument must be a String (event name)");
    RPIROH *ictx;

    if (!duk_is_function(ctx, 1))
        RP_THROW(ctx, "iroh.once: second argument must be a function");

    duk_push_true(ctx);
    duk_put_prop_string(ctx, 1, DUK_HIDDEN_SYMBOL("once"));

    duk_push_this(ctx);
    iroh_ev_on(ctx, "iroh.once", evname, 1, -1);

    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("ictx"));
    ictx = (RPIROH *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (ictx && !(ictx->flags & RPIROH_FLAG_CLOSED)) {
        if (ictx->obj_type == IROH_OBJ_ENDPOINT && strcmp(evname, "connection") == 0) {
            if (ictx->endpoint && !(ictx->flags & RPIROH_FLAG_ACCEPTING))
                iroh_start_accept_loop(ictx);
        } else if (ictx->obj_type == IROH_OBJ_CONNECTION && strcmp(evname, "stream") == 0) {
            if (ictx->conn && !(ictx->flags & RPIROH_FLAG_STREAM_ACCEPT))
                iroh_start_stream_accept_loop(ictx);
        } else if (ictx->obj_type == IROH_OBJ_STREAM && strcmp(evname, "data") == 0) {
            if (ictx->recv && !(ictx->flags & RPIROH_FLAG_READING))
                iroh_start_read_loop(ictx);
        } else if (ictx->obj_type == IROH_OBJ_CONNECTION && strcmp(evname, "datagram") == 0) {
            if (ictx->conn && !(ictx->flags & RPIROH_FLAG_DATAGRAM_RECV))
                iroh_start_datagram_loop(ictx);
        }
    }

    return 1;
}

static duk_ret_t duk_rp_iroh_off(duk_context *ctx)
{
    const char *evname = REQUIRE_STRING(ctx, 0,
        "iroh.off: first argument must be a String (event name)");

    if (!duk_is_function(ctx, 1))
        RP_THROW(ctx, "iroh.off: second argument must be a function");

    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "_events");
    if (duk_get_prop_string(ctx, -1, evname)) {
        duk_get_prop_string(ctx, 1, DUK_HIDDEN_SYMBOL("evcb_id"));
        duk_del_prop(ctx, -2);
    }

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_trigger(duk_context *ctx)
{
    const char *argzero = REQUIRE_STRING(ctx, 0,
        "iroh.trigger: first argument must be a string (event name)");
    char *ev = strdup(argzero);
    duk_idx_t nargs = 0;

    duk_push_this(ctx);
    duk_replace(ctx, 0);

    if (duk_is_undefined(ctx, 1))
        duk_pop(ctx);
    else
        nargs = 1;

    iroh_do_callback(ctx, ev, nargs);
    free(ev);
    return 0;
}

/* ======================================================================
 * One-Shot Callback Helpers
 * ====================================================================== */

static uint32_t iroh_store_cb(duk_context *ctx, RPIROH *ictx, duk_idx_t cb_idx)
{
    uint32_t cb_id = ++ictx->next_cb_id;  /* start at 1; 0 = no callback */
    cb_idx = duk_normalize_index(ctx, cb_idx);
    duk_push_heapptr(ctx, ictx->thisptr);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_cbs"));
    duk_dup(ctx, cb_idx);
    duk_put_prop_index(ctx, -2, (duk_uarridx_t)cb_id);
    duk_pop_2(ctx);
    return cb_id;
}

/* Push [func this] onto stack for duk_pcall_method. Returns 1 if found. */
static int iroh_prep_cb(duk_context *ctx, RPIROH *ictx, uint32_t cb_id)
{
    duk_push_heapptr(ctx, ictx->thisptr);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_cbs"));
    if (!duk_get_prop_index(ctx, -1, (duk_uarridx_t)cb_id)) {
        duk_pop_3(ctx);
        return 0;
    }
    duk_del_prop_index(ctx, -2, (duk_uarridx_t)cb_id);
    duk_remove(ctx, -2);   /* remove _cbs */
    duk_insert(ctx, -2);   /* [func this] */
    return 1;
}

/* ======================================================================
 * Async Bridge
 * ====================================================================== */

static void iroh_remove_pending(RPIROH *ictx, RPIROH_ASYNC *aop)
{
    int i;
    if (!ictx || !ictx->pending_ops) return;
    for (i = 0; i < ictx->npending; i++) {
        if (ictx->pending_ops[i] == aop) {
            memmove(&ictx->pending_ops[i], &ictx->pending_ops[i + 1],
                    (ictx->npending - i - 1) * sizeof(RPIROH_ASYNC *));
            ictx->npending--;
            return;
        }
    }
}

static void iroh_free_async_op(RPIROH_ASYNC *aop)
{
    if (!aop) return;
    iroh_remove_pending(aop->ictx, aop);
    if (aop->ev) { event_del(aop->ev); event_free(aop->ev); }
    if (aop->handle) iroh_async_free(aop->handle);
    free(aop);
}

/* On Windows, poll interval for async operations (no pipe fd available) */
#ifdef __CYGWIN__
static const struct timeval iroh_poll_tv = {0, 1000}; /* 1ms */
#endif

static void iroh_async_cb(evutil_socket_t fd, short events, void *arg)
{
    RPIROH_ASYNC *aop = (RPIROH_ASYNC *)arg;
    IrohAsyncState state;
    (void)fd; (void)events;

    state = iroh_async_poll(aop->handle);
    if (state == IROH_ASYNC_PENDING) {
#ifdef __CYGWIN__
        event_add(aop->ev, &iroh_poll_tv);
#else
        event_add(aop->ev, NULL);
#endif
        return;
    }

    /* Handler is responsible for freeing aop */
    iroh_dispatch_result(aop, state);
}

static RPIROH_ASYNC *iroh_start_async(RPIROH *ictx, IrohAsyncHandle *handle,
                                       iroh_op_type op, void *aux)
{
    RPIROH_ASYNC *aop;
    int fd;

    if (!handle) return NULL;

    fd = iroh_async_get_fd(handle);
#ifndef __CYGWIN__
    if (fd < 0) {
        iroh_async_free(handle);
        return NULL;
    }
#endif

    CALLOC(aop, sizeof(RPIROH_ASYNC));
    aop->ictx = ictx;
    aop->handle = handle;
    aop->op_type = op;
    aop->aux = aux;

#ifdef __CYGWIN__
    /* MinGW pipe fds are incompatible with MSYS/Cygwin libevent.
       Use a timer event to poll iroh_async_poll() instead. */
    aop->ev = evtimer_new(ictx->base, iroh_async_cb, aop);
#else
    aop->ev = event_new(ictx->base, fd, EV_READ, iroh_async_cb, aop);
#endif

    if (!aop->ev) {
        iroh_async_free(handle);
        free(aop);
        return NULL;
    }

#ifdef __CYGWIN__
    event_add(aop->ev, &iroh_poll_tv);
#else
    event_add(aop->ev, NULL);
#endif

    if (ictx->npending >= ictx->pending_alloc) {
        ictx->pending_alloc = ictx->pending_alloc ? ictx->pending_alloc * 2 : 8;
        ictx->pending_ops = realloc(ictx->pending_ops,
                                    ictx->pending_alloc * sizeof(RPIROH_ASYNC *));
    }
    ictx->pending_ops[ictx->npending++] = aop;
    return aop;
}

/* ======================================================================
 * Cleanup
 * ====================================================================== */

#define WITH_CALLBACKS 1
#define SKIP_CALLBACKS 0

static void iroh_cleanup(duk_context *ctx, RPIROH *ictx, int docb)
{
    duk_idx_t top;
    int i;
    char keystr[16];

    if (!ictx) return;
    if (ictx->flags & RPIROH_FLAG_CLOSED) return;
    if (!ctx) ctx = ictx->ctx;

    top = duk_get_top(ctx);
    ictx->flags |= RPIROH_FLAG_CLOSED;

    /* Update JS object */
    duk_push_heapptr(ctx, ictx->thisptr);
    duk_push_true(ctx);
    duk_put_prop_string(ctx, -2, "closed");
    duk_push_pointer(ctx, NULL);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("ictx"));
    duk_pop(ctx);

    /* Cancel and free all pending async ops */
    for (i = 0; i < ictx->npending; i++) {
        RPIROH_ASYNC *aop = ictx->pending_ops[i];
        if (aop->ev) { event_del(aop->ev); event_free(aop->ev); }
        if (aop->handle) { iroh_async_cancel(aop->handle); iroh_async_free(aop->handle); }
        free(aop);
    }
    if (ictx->pending_ops) free(ictx->pending_ops);
    ictx->pending_ops = NULL;
    ictx->npending = 0;
    ictx->pending_alloc = 0;

    /* Free iroh resources */
    switch (ictx->obj_type) {
        case IROH_OBJ_ENDPOINT:
            if (ictx->endpoint) { iroh_endpoint_free(ictx->endpoint); ictx->endpoint = NULL; }
            break;
        case IROH_OBJ_CONNECTION:
            if (ictx->conn) { iroh_connection_free(ictx->conn); ictx->conn = NULL; }
            break;
        case IROH_OBJ_STREAM:
            if (ictx->send) { iroh_send_stream_free(ictx->send); ictx->send = NULL; }
            if (ictx->recv) { iroh_recv_stream_free(ictx->recv); ictx->recv = NULL; }
            break;
        case IROH_OBJ_GOSSIP:
            if (ictx->router) { iroh_router_free(ictx->router); ictx->router = NULL; }
            if (ictx->gossip) { iroh_gossip_free(ictx->gossip); ictx->gossip = NULL; }
            break;
        case IROH_OBJ_GOSSIP_TOPIC:
            if (ictx->topic_handle) { iroh_gossip_topic_free(ictx->topic_handle); ictx->topic_handle = NULL; }
            break;
        case IROH_OBJ_BLOBS:
            if (ictx->router) { iroh_router_free(ictx->router); ictx->router = NULL; }
            if (ictx->blobs_proto) { iroh_blobs_protocol_free(ictx->blobs_proto); ictx->blobs_proto = NULL; }
            if (ictx->blob_store) { iroh_blobs_store_free(ictx->blob_store); ictx->blob_store = NULL; }
            break;
        case IROH_OBJ_DOCS:
            if (ictx->router) { iroh_router_free(ictx->router); ictx->router = NULL; }
            if (ictx->docs) { iroh_docs_free(ictx->docs); ictx->docs = NULL; }
            if (ictx->blob_store) { iroh_blobs_store_free(ictx->blob_store); ictx->blob_store = NULL; }
            break;
    }

    /* Remove from GC protection */
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_del_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    /* Fire close event */
    if (docb) {
        duk_push_heapptr(ctx, ictx->thisptr);
        iroh_do_callback(ctx, "close", 0);
    }

    duk_set_top(ctx, top);
    free(ictx);
}

/* ======================================================================
 * Push New Objects (Connection, Stream)
 * ====================================================================== */

static void push_new_connection(duk_context *ctx, RPIROH *parent, IrohConnection *conn)
{
    RPIROH *ictx;
    char keystr[16];
    RPTHR *thr = get_current_thread();
    duk_idx_t obj_idx;

    duk_push_object(ctx);
    obj_idx = duk_get_top_index(ctx);

    /* Set prototype from global stash */
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "iroh_conn_proto");
    duk_set_prototype(ctx, obj_idx);
    duk_pop(ctx); /* pop stash */

    /* Init _events and _cbs */
    duk_push_object(ctx);
    duk_put_prop_string(ctx, obj_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, obj_idx, DUK_HIDDEN_SYMBOL("_cbs"));

    /* Create RPIROH */
    CALLOC(ictx, sizeof(RPIROH));
    ictx->ctx = ctx;
    ictx->thisptr = duk_get_heapptr(ctx, obj_idx);
    ictx->base = thr->base;
    ictx->obj_type = IROH_OBJ_CONNECTION;
    ictx->conn = conn;
    ictx->parent = parent;
    ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, ictx);
    duk_put_prop_string(ctx, obj_idx, DUK_HIDDEN_SYMBOL("ictx"));

    /* Set connection properties */
    {
        IrohPublicKey *remote_id = iroh_connection_remote_id(conn);
        if (remote_id) {
            char *id_str = iroh_public_key_to_string(remote_id);
            if (id_str) {
                duk_push_string(ctx, id_str);
                duk_put_prop_string(ctx, obj_idx, "remoteId");
                iroh_string_free(id_str);
            }
            iroh_public_key_free(remote_id);
        }
    }
    {
        char *alpn = iroh_connection_alpn(conn);
        if (alpn) {
            duk_push_string(ctx, alpn);
            duk_put_prop_string(ctx, obj_idx, "alpn");
            iroh_string_free(alpn);
        }
    }

    duk_push_false(ctx);
    duk_put_prop_string(ctx, obj_idx, "closed");

    /* GC protection */
    duk_dup(ctx, obj_idx);
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    /* Connection object is left on the stack */
}

static void push_new_stream(duk_context *ctx, RPIROH *parent, IrohSendStream *send, IrohRecvStream *recv)
{
    RPIROH *ictx;
    char keystr[16];
    RPTHR *thr = get_current_thread();
    duk_idx_t obj_idx;

    duk_push_object(ctx);
    obj_idx = duk_get_top_index(ctx);

    /* Set prototype from global stash */
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "iroh_stream_proto");
    duk_set_prototype(ctx, obj_idx);
    duk_pop(ctx);

    duk_push_object(ctx);
    duk_put_prop_string(ctx, obj_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, obj_idx, DUK_HIDDEN_SYMBOL("_cbs"));

    CALLOC(ictx, sizeof(RPIROH));
    ictx->ctx = ctx;
    ictx->thisptr = duk_get_heapptr(ctx, obj_idx);
    ictx->base = thr->base;
    ictx->obj_type = IROH_OBJ_STREAM;
    ictx->send = send;
    ictx->recv = recv;
    ictx->parent = parent;
    ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, ictx);
    duk_put_prop_string(ctx, obj_idx, DUK_HIDDEN_SYMBOL("ictx"));
    duk_push_false(ctx);
    duk_put_prop_string(ctx, obj_idx, "closed");
    duk_push_false(ctx);
    duk_put_prop_string(ctx, obj_idx, "finished");

    duk_dup(ctx, obj_idx);
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);
}

/* ======================================================================
 * Auto-Start Loops
 * ====================================================================== */

static void iroh_start_accept_loop(RPIROH *ictx)
{
    IrohAsyncHandle *h;
    if (!ictx->endpoint || (ictx->flags & (RPIROH_FLAG_CLOSED | RPIROH_FLAG_ACCEPTING)))
        return;
    ictx->flags |= RPIROH_FLAG_ACCEPTING;
    h = iroh_endpoint_accept(ictx->endpoint);
    if (h) iroh_start_async(ictx, h, IROH_OP_ENDPOINT_ACCEPT, NULL);
    else ictx->flags &= ~RPIROH_FLAG_ACCEPTING;
}

static void iroh_start_stream_accept_loop(RPIROH *ictx)
{
    IrohAsyncHandle *h;
    if (!ictx->conn || (ictx->flags & (RPIROH_FLAG_CLOSED | RPIROH_FLAG_STREAM_ACCEPT)))
        return;
    ictx->flags |= RPIROH_FLAG_STREAM_ACCEPT;
    h = iroh_connection_accept_bi(ictx->conn);
    if (h) iroh_start_async(ictx, h, IROH_OP_ACCEPT_BI, NULL);
    else ictx->flags &= ~RPIROH_FLAG_STREAM_ACCEPT;
}

static void iroh_start_read_loop(RPIROH *ictx)
{
    IrohAsyncHandle *h;
    if (!ictx->recv || (ictx->flags & (RPIROH_FLAG_CLOSED | RPIROH_FLAG_READING)))
        return;
    ictx->flags |= RPIROH_FLAG_READING;
    h = iroh_recv_stream_read(ictx->recv, 65536);
    if (h) iroh_start_async(ictx, h, IROH_OP_STREAM_READ, NULL);
    else ictx->flags &= ~RPIROH_FLAG_READING;
}

static void iroh_start_datagram_loop(RPIROH *ictx)
{
    IrohAsyncHandle *h;
    if (!ictx->conn || (ictx->flags & (RPIROH_FLAG_CLOSED | RPIROH_FLAG_DATAGRAM_RECV)))
        return;
    ictx->flags |= RPIROH_FLAG_DATAGRAM_RECV;
    h = iroh_connection_recv_datagram(ictx->conn);
    if (h) iroh_start_async(ictx, h, IROH_OP_RECV_DATAGRAM, NULL);
    else ictx->flags &= ~RPIROH_FLAG_DATAGRAM_RECV;
}

static void iroh_start_gossip_recv_loop(RPIROH *ictx)
{
    IrohAsyncHandle *h;
    if (!ictx->topic_handle || (ictx->flags & (RPIROH_FLAG_CLOSED | RPIROH_FLAG_READING)))
        return;
    ictx->flags |= RPIROH_FLAG_READING;
    h = iroh_gossip_recv(ictx->topic_handle);
    if (h) iroh_start_async(ictx, h, IROH_OP_GOSSIP_RECV, NULL);
    else ictx->flags &= ~RPIROH_FLAG_READING;
}

/* ======================================================================
 * Result Handlers — Core (Endpoint, Connection, Stream)
 * ====================================================================== */

static void iroh_on_endpoint_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    IrohAsyncHandle *h;

    if (state != IROH_ASYNC_READY) {
        char *err = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err ? err : "endpoint creation failed");
        if (err) iroh_string_free(err);
        return;
    }

    ictx->endpoint = iroh_endpoint_create_result(aop->handle);
    iroh_free_async_op(aop);

    if (!ictx->endpoint) {
        iroh_fire_error(ictx, "Failed to get endpoint result");
        return;
    }

    /* Fire "ready" event */
    iroh_fire_event(ictx, "ready");

    /* Start wait_online */
    h = iroh_endpoint_wait_online(ictx->endpoint);
    if (h) iroh_start_async(ictx, h, IROH_OP_ENDPOINT_WAIT_ONLINE, NULL);
}

static void iroh_on_endpoint_online(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;

    iroh_free_async_op(aop);

    if (state != IROH_ASYNC_READY) {
        iroh_fire_error(ictx, "Failed to go online");
        return;
    }

    /* Set nodeId and address properties on JS object */
    duk_push_heapptr(ctx, ictx->thisptr);
    {
        IrohPublicKey *id = iroh_endpoint_id(ictx->endpoint);
        if (id) {
            char *id_str = iroh_public_key_to_string(id);
            if (id_str) {
                duk_push_string(ctx, id_str);
                duk_put_prop_string(ctx, -2, "nodeId");
                iroh_string_free(id_str);
            }
            iroh_public_key_free(id);
        }
    }
    {
        size_t sk_len = 0;
        uint8_t *sk_bytes = iroh_endpoint_secret_key(ictx->endpoint, &sk_len);
        if (sk_bytes && sk_len == 32) {
            /* secretKey: plain buffer (binary) */
            void *buf = duk_push_fixed_buffer(ctx, 32);
            memcpy(buf, sk_bytes, 32);
            duk_put_prop_string(ctx, -2, "secretKey");

            /* secretKeyHex: lowercase hex string */
            buf = duk_push_fixed_buffer(ctx, 32);
            memcpy(buf, sk_bytes, 32);
            duk_rp_toHex(ctx, -1, 0);
            duk_put_prop_string(ctx, -2, "secretKeyHex");

            iroh_bytes_free(sk_bytes, sk_len);
        }
    }
    {
        IrohEndpointAddr *addr = iroh_endpoint_addr(ictx->endpoint);
        if (addr) {
            char *addr_str = iroh_endpoint_addr_to_string(addr);
            if (addr_str) {
                duk_push_string(ctx, addr_str);
                duk_put_prop_string(ctx, -2, "address");
                iroh_string_free(addr_str);
            }
            iroh_endpoint_addr_free(addr);
        }
    }
    duk_push_true(ctx);
    duk_put_prop_string(ctx, -2, "online");
    duk_pop(ctx);

    /* Fire "online" event */
    iroh_fire_event(ictx, "online");

    /* Check for auto-connect */
    duk_push_heapptr(ctx, ictx->thisptr);
    if (duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_ac_addr"))) {
        const char *addr_str = duk_get_string(ctx, -1);
        const char *alpn_str;
        uint32_t cb_id;

        duk_pop(ctx);
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_ac_alpn"));
        alpn_str = duk_get_string(ctx, -1);
        duk_pop(ctx);
        duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_ac_cbid"));
        cb_id = (uint32_t)duk_get_uint(ctx, -1);
        duk_pop(ctx);

        {
            IrohEndpointAddr *raddr = iroh_endpoint_addr_from_string(addr_str);
            if (raddr) {
                IrohAsyncHandle *h = iroh_endpoint_connect(ictx->endpoint, raddr, alpn_str);
                iroh_endpoint_addr_free(raddr);
                if (h) iroh_start_async(ictx, h, IROH_OP_CONNECT, (void *)(uintptr_t)cb_id);
            }
        }

        duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_ac_addr"));
        duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_ac_alpn"));
        duk_del_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("_ac_cbid"));
    } else {
        duk_pop(ctx);
    }
    duk_pop(ctx);

    /* Auto-start accept loop if "connection" handlers registered */
    duk_push_heapptr(ctx, ictx->thisptr);
    duk_get_prop_string(ctx, -1, "_events");
    if (duk_get_prop_string(ctx, -1, "connection")) {
        if (!(ictx->flags & RPIROH_FLAG_ACCEPTING))
            iroh_start_accept_loop(ictx);
    }
    duk_pop_3(ctx);
}

static void iroh_on_endpoint_closed(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    (void)state;
    /* Free the close op first so cleanup won't double-free it */
    iroh_free_async_op(aop);
    /* Endpoint was closed; don't call iroh_endpoint_free on it */
    ictx->endpoint = NULL;
    iroh_cleanup(ictx->ctx, ictx, WITH_CALLBACKS);
}

static void iroh_on_connection_accepted(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    IrohConnection *conn = NULL;
    char *err_msg = NULL;

    if (state == IROH_ASYNC_READY)
        conn = iroh_connection_accept_result(aop->handle);
    else
        err_msg = iroh_async_get_error(aop->handle);

    iroh_free_async_op(aop);

    if (state != IROH_ASYNC_READY) {
        /* Don't fire error for closed endpoints - just stop the loop */
        if (err_msg) {
            if (strstr(err_msg, "closed") == NULL)
                iroh_fire_error(ictx, err_msg);
            iroh_string_free(err_msg);
        } else {
            iroh_fire_error(ictx, "accept failed");
        }
        ictx->flags &= ~RPIROH_FLAG_ACCEPTING;
        return;
    }

    if (!conn) goto restart;

    /* Create connection JS object and fire "connection" event */
    duk_push_heapptr(ctx, ictx->thisptr);
    push_new_connection(ctx, ictx, conn);
    iroh_do_callback(ctx, "connection", 1);

restart:
    if (!(ictx->flags & RPIROH_FLAG_CLOSED) && ictx->endpoint) {
        IrohAsyncHandle *h = iroh_endpoint_accept(ictx->endpoint);
        if (h) iroh_start_async(ictx, h, IROH_OP_ENDPOINT_ACCEPT, NULL);
        else ictx->flags &= ~RPIROH_FLAG_ACCEPTING;
    } else {
        ictx->flags &= ~RPIROH_FLAG_ACCEPTING;
    }
}

static void iroh_on_connected(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    IrohConnection *conn = NULL;
    char *err_msg = NULL;

    if (state == IROH_ASYNC_READY)
        conn = iroh_connection_connect_result(aop->handle);
    else
        err_msg = iroh_async_get_error(aop->handle);

    iroh_free_async_op(aop);

    if (state != IROH_ASYNC_READY || !conn) {
        iroh_fire_error(ictx, err_msg ? err_msg : "connect failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    /* Create connection and call callback */
    if (cb_id && iroh_prep_cb(ctx, ictx, cb_id)) {
        push_new_connection(ctx, ictx, conn);
        if (duk_pcall_method(ctx, 1) != 0) {
            const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
            fprintf(stderr, "Error in connect callback:\n%s\n", errmsg);
            duk_pop_2(ctx);
        } else {
            duk_pop(ctx);
        }
    } else {
        /* No callback - just fire "connect" event on endpoint */
        duk_push_heapptr(ctx, ictx->thisptr);
        push_new_connection(ctx, ictx, conn);
        iroh_do_callback(ctx, "connect", 1);
    }
}

static void iroh_on_stream_accepted(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    IrohSendStream *send = NULL;
    IrohRecvStream *recv = NULL;
    char *err_msg = NULL;

    if (state == IROH_ASYNC_READY) {
        send = iroh_stream_open_bi_send_result(aop->handle);
        recv = iroh_stream_open_bi_recv_result(aop->handle);
    } else {
        err_msg = iroh_async_get_error(aop->handle);
    }

    iroh_free_async_op(aop);

    if (state != IROH_ASYNC_READY || !send || !recv) {
        if (send) iroh_send_stream_free(send);
        if (recv) iroh_recv_stream_free(recv);
        /* Don't fire error for closed connections - just stop the loop */
        if (err_msg) {
            if (strstr(err_msg, "closed") == NULL)
                iroh_fire_error(ictx, err_msg);
            iroh_string_free(err_msg);
        } else {
            iroh_fire_error(ictx, "stream accept failed");
        }
        /* Stop the stream accept loop on error */
        ictx->flags &= ~RPIROH_FLAG_STREAM_ACCEPT;
        return;
    }

    /* Create stream and fire "stream" event on connection */
    duk_push_heapptr(ctx, ictx->thisptr);
    push_new_stream(ctx, ictx, send, recv);
    iroh_do_callback(ctx, "stream", 1);

    /* Restart accept loop */
    if (!(ictx->flags & RPIROH_FLAG_CLOSED) && ictx->conn) {
        IrohAsyncHandle *h = iroh_connection_accept_bi(ictx->conn);
        if (h) iroh_start_async(ictx, h, IROH_OP_ACCEPT_BI, NULL);
        else ictx->flags &= ~RPIROH_FLAG_STREAM_ACCEPT;
    } else {
        ictx->flags &= ~RPIROH_FLAG_STREAM_ACCEPT;
    }
}

static void iroh_on_bi_opened(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    IrohSendStream *send = NULL;
    IrohRecvStream *recv = NULL;
    char *err_msg = NULL;

    if (state == IROH_ASYNC_READY) {
        send = iroh_stream_open_bi_send_result(aop->handle);
        recv = iroh_stream_open_bi_recv_result(aop->handle);
    } else {
        err_msg = iroh_async_get_error(aop->handle);
    }

    iroh_free_async_op(aop);

    if (state != IROH_ASYNC_READY || !send || !recv) {
        if (send) iroh_send_stream_free(send);
        if (recv) iroh_recv_stream_free(recv);
        iroh_fire_error(ictx, err_msg ? err_msg : "openBi failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    if (iroh_prep_cb(ctx, ictx, cb_id)) {
        push_new_stream(ctx, ictx, send, recv);
        if (duk_pcall_method(ctx, 1) != 0) {
            const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
            fprintf(stderr, "Error in openBi callback:\n%s\n", errmsg);
            duk_pop_2(ctx);
        } else {
            duk_pop(ctx);
        }
    }
}

static void iroh_on_stream_data(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    IrohReadResult result;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        /* Treat connection-lost/closed errors as normal end-of-stream */
        if (err_msg && (strstr(err_msg, "closed") || strstr(err_msg, "connection lost"))) {
            iroh_string_free(err_msg);
        } else {
            iroh_fire_error(ictx, err_msg ? err_msg : "stream read failed");
            if (err_msg) iroh_string_free(err_msg);
        }
        ictx->flags &= ~RPIROH_FLAG_READING;
        iroh_fire_event(ictx, "end");
        return;
    }

    result = iroh_recv_stream_read_result(aop->handle);
    iroh_free_async_op(aop);

    if (result.data && result.len > 0) {
        void *buf;
        duk_push_heapptr(ctx, ictx->thisptr);
        buf = duk_push_buffer(ctx, result.len, 0);
        memcpy(buf, result.data, result.len);
        iroh_do_callback(ctx, "data", 1);
    }
    if (result.data)
        iroh_bytes_free(result.data, result.len);

    if (result.finished) {
        ictx->flags &= ~RPIROH_FLAG_READING;
        iroh_fire_event(ictx, "end");
        return;
    }

    /* Continue reading */
    if (!(ictx->flags & RPIROH_FLAG_CLOSED) && ictx->recv) {
        IrohAsyncHandle *h = iroh_recv_stream_read(ictx->recv, 65536);
        if (h) iroh_start_async(ictx, h, IROH_OP_STREAM_READ, NULL);
        else ictx->flags &= ~RPIROH_FLAG_READING;
    } else {
        ictx->flags &= ~RPIROH_FLAG_READING;
    }
}

static void iroh_on_stream_written(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    char *err_msg = NULL;

    ictx->flags &= ~RPIROH_FLAG_WRITING;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "stream write failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    iroh_free_async_op(aop);
    iroh_fire_event(ictx, "drain");

    /* Handle deferred finish */
    if (ictx->flags & RPIROH_FLAG_FINISH_PENDING) {
        ictx->flags &= ~RPIROH_FLAG_FINISH_PENDING;
        iroh_do_finish(ictx);
    }
}

static void iroh_on_datagram_received(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "datagram recv failed");
        if (err_msg) iroh_string_free(err_msg);
        ictx->flags &= ~RPIROH_FLAG_DATAGRAM_RECV;
        return;
    }

    {
        size_t len = 0;
        uint8_t *data = iroh_connection_recv_datagram_result(aop->handle, &len);
        iroh_free_async_op(aop);

        if (data && len > 0) {
            void *buf;
            duk_push_heapptr(ctx, ictx->thisptr);
            buf = duk_push_buffer(ctx, len, 0);
            memcpy(buf, data, len);
            iroh_do_callback(ctx, "datagram", 1);
            iroh_bytes_free(data, len);
        }
    }

    /* Continue recv loop */
    if (!(ictx->flags & RPIROH_FLAG_CLOSED) && ictx->conn) {
        IrohAsyncHandle *h = iroh_connection_recv_datagram(ictx->conn);
        if (h) iroh_start_async(ictx, h, IROH_OP_RECV_DATAGRAM, NULL);
        else ictx->flags &= ~RPIROH_FLAG_DATAGRAM_RECV;
    } else {
        ictx->flags &= ~RPIROH_FLAG_DATAGRAM_RECV;
    }
}

/* ======================================================================
 * Result Handlers — Gossip
 * ====================================================================== */

static void iroh_on_gossip_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "gossip creation failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    ictx->gossip = iroh_gossip_from_router_result(aop->handle);
    ictx->router = iroh_router_from_gossip_result(aop->handle);
    iroh_free_async_op(aop);

    if (!ictx->gossip) {
        iroh_fire_error(ictx, "Failed to get gossip result");
        return;
    }

    iroh_fire_event(ictx, "ready");
}

static void iroh_on_gossip_subscribed(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "gossip subscribe failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    ictx->topic_handle = iroh_gossip_subscribe_result(aop->handle);
    iroh_free_async_op(aop);

    if (!ictx->topic_handle) {
        iroh_fire_error(ictx, "Failed to get topic result");
        return;
    }

    iroh_start_gossip_recv_loop(ictx);
    iroh_fire_event(ictx, "ready");
}

static void iroh_on_gossip_broadcast(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "broadcast failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }
    iroh_free_async_op(aop);
}

static void iroh_on_gossip_recv(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        if (err_msg && (strstr(err_msg, "closed") || strstr(err_msg, "connection lost"))) {
            iroh_string_free(err_msg);
        } else {
            iroh_fire_error(ictx, err_msg ? err_msg : "gossip recv failed");
            if (err_msg) iroh_string_free(err_msg);
        }
        ictx->flags &= ~RPIROH_FLAG_READING;
        return;
    }

    {
        IrohGossipEvent event = iroh_gossip_recv_result(aop->handle);
        iroh_free_async_op(aop);

        switch (event.event_type) {
            case IROH_GOSSIP_EVENT_RECEIVED: {
                duk_push_heapptr(ctx, ictx->thisptr);
                duk_push_object(ctx);
                if (event.data && event.data_len > 0) {
                    void *buf = duk_push_buffer(ctx, event.data_len, 0);
                    memcpy(buf, event.data, event.data_len);
                    duk_put_prop_string(ctx, -2, "data");
                }
                if (event.peer) {
                    char *ps = iroh_public_key_to_string(event.peer);
                    if (ps) { duk_push_string(ctx, ps); iroh_string_free(ps); }
                    else duk_push_string(ctx, "");
                    duk_put_prop_string(ctx, -2, "sender");
                }
                iroh_do_callback(ctx, "message", 1);
                break;
            }
            case IROH_GOSSIP_EVENT_NEIGHBORUP: {
                duk_push_heapptr(ctx, ictx->thisptr);
                if (event.peer) {
                    char *ps = iroh_public_key_to_string(event.peer);
                    duk_push_string(ctx, ps ? ps : "");
                    if (ps) iroh_string_free(ps);
                } else duk_push_string(ctx, "");
                iroh_do_callback(ctx, "peerJoin", 1);
                break;
            }
            case IROH_GOSSIP_EVENT_NEIGHBORDOWN: {
                duk_push_heapptr(ctx, ictx->thisptr);
                if (event.peer) {
                    char *ps = iroh_public_key_to_string(event.peer);
                    duk_push_string(ctx, ps ? ps : "");
                    if (ps) iroh_string_free(ps);
                } else duk_push_string(ctx, "");
                iroh_do_callback(ctx, "peerLeave", 1);
                break;
            }
            default:
                break;
        }

        iroh_gossip_event_free(&event);
    }

    /* Continue recv loop */
    if (!(ictx->flags & RPIROH_FLAG_CLOSED) && ictx->topic_handle) {
        IrohAsyncHandle *h = iroh_gossip_recv(ictx->topic_handle);
        if (h) iroh_start_async(ictx, h, IROH_OP_GOSSIP_RECV, NULL);
        else ictx->flags &= ~RPIROH_FLAG_READING;
    } else {
        ictx->flags &= ~RPIROH_FLAG_READING;
    }
}

/* ======================================================================
 * Result Handlers — Blobs
 * ====================================================================== */

static void iroh_on_blobs_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "blobs creation failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    ictx->blob_store = iroh_blobs_store_from_router_result(aop->handle);
    ictx->blobs_proto = iroh_blobs_protocol_from_router_result(aop->handle);
    ictx->router = iroh_router_from_blobs_result(aop->handle);
    iroh_free_async_op(aop);

    if (!ictx->blob_store) {
        iroh_fire_error(ictx, "Failed to get blobs result");
        return;
    }

    iroh_fire_event(ictx, "ready");
}

static void iroh_on_blobs_added(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "blob add failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        IrohBlobHash *hash = iroh_blobs_add_result(aop->handle);
        iroh_free_async_op(aop);

        if (hash && iroh_prep_cb(ctx, ictx, cb_id)) {
            duk_push_object(ctx);
            {
                char *hs = iroh_blob_hash_to_string(hash);
                if (hs) { duk_push_string(ctx, hs); iroh_string_free(hs); }
                else duk_push_null(ctx);
                duk_put_prop_string(ctx, -2, "hash");
            }
            /* Also create ticket if endpoint available */
            if (ictx->parent && ((RPIROH *)ictx->parent)->endpoint) {
                IrohBlobTicket *ticket = iroh_blobs_create_ticket(
                    ((RPIROH *)ictx->parent)->endpoint, hash);
                if (ticket) {
                    char *ts = iroh_blob_ticket_to_string(ticket);
                    if (ts) { duk_push_string(ctx, ts); iroh_string_free(ts); }
                    else duk_push_null(ctx);
                    duk_put_prop_string(ctx, -2, "ticket");
                    iroh_blob_ticket_free(ticket);
                }
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in addBytes callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
            iroh_blob_hash_free(hash);
        } else {
            if (hash) iroh_blob_hash_free(hash);
        }
    }
}

static void iroh_on_blobs_read(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "blob read failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        size_t len = 0;
        uint8_t *data = iroh_blobs_read_result(aop->handle, &len);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            if (data && len > 0) {
                void *buf = duk_push_buffer(ctx, len, 0);
                memcpy(buf, data, len);
            } else {
                duk_push_null(ctx);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in blobs.read callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (data) iroh_bytes_free(data, len);
    }
}

static void iroh_on_blobs_downloaded(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "blob download failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        IrohBlobHash *hash = iroh_blobs_download_result(aop->handle);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            if (hash) {
                char *hs = iroh_blob_hash_to_string(hash);
                duk_push_string(ctx, hs ? hs : "");
                if (hs) iroh_string_free(hs);
            } else {
                duk_push_null(ctx);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in download callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (hash) iroh_blob_hash_free(hash);
    }
}

/* ======================================================================
 * Result Handlers — Docs
 * ====================================================================== */

static void iroh_on_docs_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs creation failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    ictx->docs = iroh_docs_from_router_result(aop->handle);
    ictx->blob_store = iroh_blobs_store_from_docs_result(aop->handle);
    ictx->router = iroh_router_from_docs_result(aop->handle);
    iroh_free_async_op(aop);

    if (!ictx->docs) {
        iroh_fire_error(ictx, "Failed to get docs result");
        return;
    }

    iroh_fire_event(ictx, "ready");
}

/* Generic one-shot callback handler for docs operations that return a string ID */
static void iroh_on_docs_id_result(RPIROH_ASYNC *aop, IrohAsyncState state,
                                    char *(*to_string)(const void *),
                                    void *(*get_result)(IrohAsyncHandle *),
                                    void (*free_result)(void *))
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs operation failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        void *result = get_result(aop->handle);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            if (result) {
                char *s = to_string(result);
                duk_push_string(ctx, s ? s : "");
                if (s) iroh_string_free(s);
            } else {
                duk_push_null(ctx);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in docs callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (result && free_result) free_result(result);
    }
}

static void iroh_on_docs_author_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    iroh_on_docs_id_result(aop, state,
        (char *(*)(const void *))iroh_author_id_to_string,
        (void *(*)(IrohAsyncHandle *))iroh_docs_create_author_result,
        (void (*)(void *))iroh_author_id_free);
}

static void iroh_on_docs_doc_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    iroh_on_docs_id_result(aop, state,
        (char *(*)(const void *))iroh_namespace_id_to_string,
        (void *(*)(IrohAsyncHandle *))iroh_docs_create_doc_result,
        (void (*)(void *))iroh_namespace_id_free);
}

static void iroh_on_docs_set_done(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs set failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    iroh_free_async_op(aop);

    if (iroh_prep_cb(ctx, ictx, cb_id)) {
        if (duk_pcall_method(ctx, 0) != 0) {
            const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
            fprintf(stderr, "Error in docs.set callback:\n%s\n", errmsg);
            duk_pop_2(ctx);
        } else duk_pop(ctx);
    }
}

static void iroh_on_docs_get_done(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs get failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        size_t len = 0;
        uint8_t *data = iroh_docs_get_result(aop->handle, &len);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            if (data && len > 0) {
                void *buf = duk_push_buffer(ctx, len, 0);
                memcpy(buf, data, len);
            } else {
                duk_push_null(ctx);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in docs.get callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (data) iroh_bytes_free(data, len);
    }
}

static void iroh_on_docs_get_latest_done(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs getAttr failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        size_t len = 0;
        uint8_t *data = iroh_docs_get_latest_result(aop->handle, &len);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            if (data && len > 0) {
                void *buf = duk_push_buffer(ctx, len, 0);
                memcpy(buf, data, len);
            } else {
                duk_push_null(ctx);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in docs.getAttr callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (data) iroh_bytes_free(data, len);
    }
}

static void iroh_on_docs_get_all_done(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs getAll failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        size_t count = 0;
        IrohKeyValuePair *pairs = iroh_docs_get_all_result(aop->handle, &count);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            duk_push_object(ctx);
            for (size_t i = 0; i < count; i++) {
                if (pairs[i].value && pairs[i].value_len > 0) {
                    void *buf = duk_push_buffer(ctx, pairs[i].value_len, 0);
                    memcpy(buf, pairs[i].value, pairs[i].value_len);
                } else {
                    duk_push_null(ctx);
                }
                duk_put_prop_string(ctx, -2, pairs[i].key);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in docs.getAll callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (pairs) iroh_key_value_pairs_free(pairs, count);
    }
}

static void iroh_on_docs_delete_done(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    /* Same pattern as set_done */
    iroh_on_docs_set_done(aop, state);
}

static void iroh_on_docs_ticket_created(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    RPIROH *ictx = aop->ictx;
    duk_context *ctx = ictx->ctx;
    uint32_t cb_id = (uint32_t)(uintptr_t)aop->aux;
    char *err_msg = NULL;

    if (state != IROH_ASYNC_READY) {
        err_msg = iroh_async_get_error(aop->handle);
        iroh_free_async_op(aop);
        iroh_fire_error(ictx, err_msg ? err_msg : "docs createTicket failed");
        if (err_msg) iroh_string_free(err_msg);
        return;
    }

    {
        IrohDocTicket *ticket = iroh_docs_create_ticket_result(aop->handle);
        iroh_free_async_op(aop);

        if (iroh_prep_cb(ctx, ictx, cb_id)) {
            if (ticket) {
                char *ts = iroh_doc_ticket_to_string(ticket);
                duk_push_string(ctx, ts ? ts : "");
                if (ts) iroh_string_free(ts);
            } else {
                duk_push_null(ctx);
            }
            if (duk_pcall_method(ctx, 1) != 0) {
                const char *errmsg = rp_push_error(ctx, -1, NULL, rp_print_error_lines);
                fprintf(stderr, "Error in docs.createTicket callback:\n%s\n", errmsg);
                duk_pop_2(ctx);
            } else duk_pop(ctx);
        }
        if (ticket) iroh_doc_ticket_free(ticket);
    }
}

static void iroh_on_docs_joined(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    iroh_on_docs_id_result(aop, state,
        (char *(*)(const void *))iroh_namespace_id_to_string,
        (void *(*)(IrohAsyncHandle *))iroh_docs_join_result,
        (void (*)(void *))iroh_namespace_id_free);
}

/* ======================================================================
 * Dispatch — routes async results to the appropriate handler
 * ====================================================================== */

static void iroh_dispatch_result(RPIROH_ASYNC *aop, IrohAsyncState state)
{
    switch (aop->op_type) {
        case IROH_OP_ENDPOINT_CREATE:      iroh_on_endpoint_created(aop, state); break;
        case IROH_OP_ENDPOINT_WAIT_ONLINE: iroh_on_endpoint_online(aop, state); break;
        case IROH_OP_ENDPOINT_CLOSE:       iroh_on_endpoint_closed(aop, state); break;
        case IROH_OP_ENDPOINT_ACCEPT:      iroh_on_connection_accepted(aop, state); break;
        case IROH_OP_CONNECT:              iroh_on_connected(aop, state); break;
        case IROH_OP_OPEN_BI:              iroh_on_bi_opened(aop, state); break;
        case IROH_OP_ACCEPT_BI:            iroh_on_stream_accepted(aop, state); break;
        case IROH_OP_STREAM_READ:          iroh_on_stream_data(aop, state); break;
        case IROH_OP_STREAM_WRITE:         iroh_on_stream_written(aop, state); break;
        case IROH_OP_RECV_DATAGRAM:        iroh_on_datagram_received(aop, state); break;
        case IROH_OP_GOSSIP_CREATE:        iroh_on_gossip_created(aop, state); break;
        case IROH_OP_GOSSIP_SUBSCRIBE:     iroh_on_gossip_subscribed(aop, state); break;
        case IROH_OP_GOSSIP_BROADCAST:     iroh_on_gossip_broadcast(aop, state); break;
        case IROH_OP_GOSSIP_RECV:          iroh_on_gossip_recv(aop, state); break;
        case IROH_OP_BLOBS_CREATE:         iroh_on_blobs_created(aop, state); break;
        case IROH_OP_BLOBS_ADD_BYTES:      iroh_on_blobs_added(aop, state); break;
        case IROH_OP_BLOBS_ADD_FILE:       iroh_on_blobs_added(aop, state); break;
        case IROH_OP_BLOBS_READ:           iroh_on_blobs_read(aop, state); break;
        case IROH_OP_BLOBS_DOWNLOAD:       iroh_on_blobs_downloaded(aop, state); break;
        case IROH_OP_DOCS_CREATE:          iroh_on_docs_created(aop, state); break;
        case IROH_OP_DOCS_CREATE_AUTHOR:   iroh_on_docs_author_created(aop, state); break;
        case IROH_OP_DOCS_CREATE_DOC:      iroh_on_docs_doc_created(aop, state); break;
        case IROH_OP_DOCS_SET:             iroh_on_docs_set_done(aop, state); break;
        case IROH_OP_DOCS_GET:             iroh_on_docs_get_done(aop, state); break;
        case IROH_OP_DOCS_GET_LATEST:      iroh_on_docs_get_latest_done(aop, state); break;
        case IROH_OP_DOCS_GET_ALL:         iroh_on_docs_get_all_done(aop, state); break;
        case IROH_OP_DOCS_DELETE:          iroh_on_docs_delete_done(aop, state); break;
        case IROH_OP_DOCS_CREATE_TICKET:   iroh_on_docs_ticket_created(aop, state); break;
        case IROH_OP_DOCS_JOIN:            iroh_on_docs_joined(aop, state); break;
        default:
            iroh_free_async_op(aop);
            break;
    }
}

/* ======================================================================
 * GET ICTX helper — extract RPIROH* from 'this'
 * ====================================================================== */

#define GET_ICTX(ctx, varname, errmsg) \
    RPIROH *varname; \
    do { \
        duk_push_this(ctx); \
        duk_get_prop_string((ctx), -1, DUK_HIDDEN_SYMBOL("ictx")); \
        varname = (RPIROH *)duk_get_pointer((ctx), -1); \
        duk_pop_2(ctx); \
        if (!varname) RP_THROW((ctx), "%s", (errmsg)); \
    } while(0)

/* ======================================================================
 * Endpoint Constructor + Methods
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_endpoint(duk_context *ctx)
{
    RPIROH *ictx;
    RPTHR *thr = get_current_thread();
    char keystr[16];
    duk_idx_t this_idx;
    IrohEndpointConfig *config;
    IrohAsyncHandle *h;

    if (!duk_is_constructor_call(ctx))
        return DUK_RET_TYPE_ERROR;

    duk_push_this(ctx);
    this_idx = duk_get_top_index(ctx);

    /* Initialize _events and _cbs */
    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_cbs"));
    duk_push_false(ctx);
    duk_put_prop_string(ctx, this_idx, "closed");
    duk_push_false(ctx);
    duk_put_prop_string(ctx, this_idx, "online");

    /* Create RPIROH */
    CALLOC(ictx, sizeof(RPIROH));
    ictx->ctx = ctx;
    ictx->thisptr = duk_get_heapptr(ctx, this_idx);
    ictx->base = thr->base;
    ictx->obj_type = IROH_OBJ_ENDPOINT;
    ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, ictx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("ictx"));

    /* GC protection */
    duk_dup(ctx, this_idx);
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    /* Build endpoint config */
    config = iroh_endpoint_config_default();

    if (duk_is_object(ctx, 0)) {
        /* alpn */
        if (duk_get_prop_string(ctx, 0, "alpn")) {
            if (duk_is_string(ctx, -1))
                iroh_endpoint_config_add_alpn(config, duk_get_string(ctx, -1));
            else if (duk_is_array(ctx, -1)) {
                duk_uarridx_t i, len = (duk_uarridx_t)duk_get_length(ctx, -1);
                for (i = 0; i < len; i++) {
                    duk_get_prop_index(ctx, -1, i);
                    if (duk_is_string(ctx, -1))
                        iroh_endpoint_config_add_alpn(config, duk_get_string(ctx, -1));
                    duk_pop(ctx);
                }
            }
        }
        duk_pop(ctx);

        /* secretKey: string (hex) or buffer (binary 32 bytes) */
        if (duk_get_prop_string(ctx, 0, "secretKey")) {
            if (duk_is_string(ctx, -1)) {
                /* hex string -> decode to binary buffer */
                duk_rp_hexToBuf(ctx, -1);  /* pushes buffer on top */
                {
                    duk_size_t blen;
                    const uint8_t *bptr = (const uint8_t *)duk_get_buffer_data(ctx, -1, &blen);
                    if (bptr && blen == 32)
                        iroh_endpoint_config_secret_key_bytes(config, bptr, blen);
                }
                duk_pop(ctx); /* pop the buffer */
            } else if (duk_is_buffer_data(ctx, -1)) {
                duk_size_t blen;
                const uint8_t *bptr = (const uint8_t *)duk_get_buffer_data(ctx, -1, &blen);
                if (bptr && blen == 32)
                    iroh_endpoint_config_secret_key_bytes(config, bptr, blen);
            }
        }
        duk_pop(ctx);

        /* discovery */
        if (duk_get_prop_string(ctx, 0, "discovery")) {
            duk_bool_t disc = duk_to_boolean(ctx, -1);
            iroh_endpoint_config_discovery_n0(config, disc);
            iroh_endpoint_config_discovery_dns(config, disc);
            iroh_endpoint_config_discovery_pkarr_publish(config, disc);
        }
        duk_pop(ctx);

        /* relay */
        if (duk_get_prop_string(ctx, 0, "relay")) {
            if (duk_is_string(ctx, -1))
                iroh_endpoint_config_relay_url(config, duk_get_string(ctx, -1));
            else if (duk_is_boolean(ctx, -1) && !duk_get_boolean(ctx, -1))
                iroh_endpoint_config_relay_mode(config, IROH_RELAY_MODE_DISABLED);
        }
        duk_pop(ctx);

        /* port */
        if (duk_get_prop_string(ctx, 0, "port")) {
            iroh_endpoint_config_bind_port(config, (uint16_t)duk_get_uint(ctx, -1));
        }
        duk_pop(ctx);

        /* timeout */
        if (duk_get_prop_string(ctx, 0, "timeout")) {
            iroh_endpoint_config_max_idle_timeout(config, (uint64_t)duk_get_number(ctx, -1));
        }
        duk_pop(ctx);
    }

    /* Start async endpoint creation */
    h = iroh_endpoint_create(config);
    if (!h) {
        char *err = iroh_last_error();
        iroh_endpoint_config_free(config);
        RP_THROW(ctx, "iroh.Endpoint: failed to start creation: %s",
                 err ? err : "unknown error");
    }
    /* config is consumed by iroh_endpoint_create */

    iroh_start_async(ictx, h, IROH_OP_ENDPOINT_CREATE, NULL);

    return 0;
}

static duk_ret_t duk_rp_iroh_ep_connect(duk_context *ctx)
{
    const char *addr_str = REQUIRE_STRING(ctx, 0,
        "endpoint.connect: first arg must be a string (address)");
    const char *alpn = REQUIRE_STRING(ctx, 1,
        "endpoint.connect: second arg must be a string (ALPN)");
    IrohEndpointAddr *addr;
    IrohAsyncHandle *h;
    void *aux = NULL;

    GET_ICTX(ctx, ictx, "endpoint.connect: endpoint not initialized");

    if (!ictx->endpoint)
        RP_THROW(ctx, "endpoint.connect: endpoint not ready");

    addr = iroh_endpoint_addr_from_string(addr_str);
    if (!addr) {
        char *err = iroh_last_error();
        RP_THROW(ctx, "endpoint.connect: invalid address: %s", err ? err : "parse error");
    }

    if (duk_is_function(ctx, 2)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 2);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_endpoint_connect(ictx->endpoint, addr, alpn);
    iroh_endpoint_addr_free(addr);

    if (!h)
        RP_THROW(ctx, "endpoint.connect: failed to start connection");

    iroh_start_async(ictx, h, IROH_OP_CONNECT, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_ep_close(duk_context *ctx)
{
    int i;
    GET_ICTX(ctx, ictx, "endpoint.close: not initialized");

    if (ictx->flags & RPIROH_FLAG_CLOSED)
        return 0;

    ictx->flags |= RPIROH_FLAG_CLOSED;

    /* Cancel pending ops */
    for (i = ictx->npending - 1; i >= 0; i--) {
        RPIROH_ASYNC *aop = ictx->pending_ops[i];
        if (aop->ev) { event_del(aop->ev); event_free(aop->ev); aop->ev = NULL; }
        if (aop->handle) { iroh_async_cancel(aop->handle); iroh_async_free(aop->handle); aop->handle = NULL; }
        free(aop);
    }
    ictx->npending = 0;

    /* Try graceful close */
    if (ictx->endpoint) {
        IrohAsyncHandle *h = iroh_endpoint_close(ictx->endpoint);
        if (h) {
            ictx->flags &= ~RPIROH_FLAG_CLOSED; /* let cleanup set it */
            iroh_start_async(ictx, h, IROH_OP_ENDPOINT_CLOSE, NULL);
            return 0;
        }
    }

    iroh_cleanup(ctx, ictx, WITH_CALLBACKS);
    return 0;
}

static duk_ret_t duk_rp_iroh_ep_node_id(duk_context *ctx)
{
    GET_ICTX(ctx, ictx, "endpoint.nodeId: not initialized");
    if (!ictx->endpoint) { duk_push_undefined(ctx); return 1; }

    {
        IrohPublicKey *id = iroh_endpoint_id(ictx->endpoint);
        if (id) {
            char *s = iroh_public_key_to_string(id);
            duk_push_string(ctx, s ? s : "");
            if (s) iroh_string_free(s);
            iroh_public_key_free(id);
        } else {
            duk_push_undefined(ctx);
        }
    }
    return 1;
}

static duk_ret_t duk_rp_iroh_ep_address(duk_context *ctx)
{
    GET_ICTX(ctx, ictx, "endpoint.address: not initialized");
    if (!ictx->endpoint) { duk_push_undefined(ctx); return 1; }

    {
        IrohEndpointAddr *addr = iroh_endpoint_addr(ictx->endpoint);
        if (addr) {
            char *s = iroh_endpoint_addr_to_string(addr);
            duk_push_string(ctx, s ? s : "");
            if (s) iroh_string_free(s);
            iroh_endpoint_addr_free(addr);
        } else {
            duk_push_undefined(ctx);
        }
    }
    return 1;
}

/* ======================================================================
 * Connection Methods
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_conn_open_bi(duk_context *ctx)
{
    IrohAsyncHandle *h;
    void *aux = NULL;

    if (!duk_is_function(ctx, 0))
        RP_THROW(ctx, "connection.openBi: first arg must be a function (callback)");

    GET_ICTX(ctx, ictx, "connection.openBi: not initialized");
    if (!ictx->conn) RP_THROW(ctx, "connection.openBi: connection not ready");

    {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 0);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_connection_open_bi(ictx->conn);
    if (!h) RP_THROW(ctx, "connection.openBi: failed to start");

    iroh_start_async(ictx, h, IROH_OP_OPEN_BI, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_conn_close(duk_context *ctx)
{
    uint32_t code = 0;
    const char *reason = "closed";

    GET_ICTX(ctx, ictx, "connection.close: not initialized");

    if (duk_is_number(ctx, 0)) code = (uint32_t)duk_get_uint(ctx, 0);
    if (duk_is_string(ctx, 1)) reason = duk_get_string(ctx, 1);

    if (ictx->conn && !(ictx->flags & RPIROH_FLAG_CLOSED))
        iroh_connection_close(ictx->conn, code, reason);

    iroh_cleanup(ctx, ictx, WITH_CALLBACKS);
    return 0;
}

static duk_ret_t duk_rp_iroh_conn_send_datagram(duk_context *ctx)
{
    const uint8_t *data;
    duk_size_t len;
    IrohError err;

    GET_ICTX(ctx, ictx, "connection.sendDatagram: not initialized");
    if (!ictx->conn) RP_THROW(ctx, "connection.sendDatagram: not connected");

    if (duk_is_string(ctx, 0)) {
        data = (const uint8_t *)duk_get_lstring(ctx, 0, &len);
    } else if (duk_is_buffer_data(ctx, 0)) {
        data = (const uint8_t *)duk_get_buffer_data(ctx, 0, &len);
    } else {
        RP_THROW(ctx, "connection.sendDatagram: arg must be a string or buffer");
        return 0; /* not reached */
    }

    err = iroh_connection_send_datagram(ictx->conn, data, (size_t)len);
    if (err != IROH_ERROR_OK) {
        char *msg = iroh_last_error();
        RP_THROW(ctx, "connection.sendDatagram: %s", msg ? msg : "send failed");
    }

    return 0;
}

/* ======================================================================
 * Stream Methods
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_stream_write(duk_context *ctx)
{
    const uint8_t *data;
    duk_size_t len;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "stream.write: not initialized");
    if (!ictx->send) RP_THROW(ctx, "stream.write: no send stream");
    if (ictx->flags & RPIROH_FLAG_FINISHED)
        RP_THROW(ctx, "stream.write: stream already finished");

    if (duk_is_string(ctx, 0)) {
        data = (const uint8_t *)duk_get_lstring(ctx, 0, &len);
    } else if (duk_is_buffer_data(ctx, 0)) {
        data = (const uint8_t *)duk_get_buffer_data(ctx, 0, &len);
    } else {
        RP_THROW(ctx, "stream.write: arg must be a string or buffer");
        return 0;
    }

    h = iroh_send_stream_write(ictx->send, data, (size_t)len);
    if (!h) RP_THROW(ctx, "stream.write: failed to start write");

    ictx->flags |= RPIROH_FLAG_WRITING;
    iroh_start_async(ictx, h, IROH_OP_STREAM_WRITE, NULL);

    duk_push_this(ctx);
    return 1;
}

static void iroh_do_finish(RPIROH *ictx)
{
    duk_context *ctx = ictx->ctx;
    if (ictx->send && !(ictx->flags & RPIROH_FLAG_FINISHED)) {
        iroh_send_stream_finish(ictx->send);
        ictx->flags |= RPIROH_FLAG_FINISHED;

        duk_push_heapptr(ctx, ictx->thisptr);
        duk_push_true(ctx);
        duk_put_prop_string(ctx, -2, "finished");
        duk_pop(ctx);
    }
}

static duk_ret_t duk_rp_iroh_stream_finish(duk_context *ctx)
{
    GET_ICTX(ctx, ictx, "stream.finish: not initialized");

    if (ictx->flags & RPIROH_FLAG_WRITING) {
        /* Defer finish until write completes */
        ictx->flags |= RPIROH_FLAG_FINISH_PENDING;
    } else {
        iroh_do_finish(ictx);
    }

    return 0;
}

static duk_ret_t duk_rp_iroh_stream_destroy(duk_context *ctx)
{
    GET_ICTX(ctx, ictx, "stream.destroy: not initialized");
    iroh_cleanup(ctx, ictx, WITH_CALLBACKS);
    return 0;
}

/* ======================================================================
 * Convenience: createServer, connect
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_create_server(duk_context *ctx)
{
    duk_idx_t func_idx = -1, obj_idx = -1, ep_idx;
    int i = 0;

    while (i < 2 && !duk_is_undefined(ctx, i)) {
        if (duk_is_function(ctx, i)) func_idx = i;
        else if (duk_is_object(ctx, i)) obj_idx = i;
        i++;
    }

    /* Get Endpoint constructor */
    duk_push_current_function(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("Endpoint"));
    duk_remove(ctx, -2);

    /* new Endpoint(options) */
    if (obj_idx >= 0) duk_dup(ctx, obj_idx);
    else duk_push_object(ctx);
    duk_new(ctx, 1);
    ep_idx = duk_get_top_index(ctx);

    /* Register connection callback */
    if (func_idx >= 0) {
        iroh_ev_on(ctx, "createServer", "connection", func_idx, ep_idx);
    }

    return 1;
}

static duk_ret_t duk_rp_iroh_connect_to(duk_context *ctx)
{
    const char *addr_str, *alpn;
    duk_idx_t ep_idx;
    RPIROH *ictx;

    addr_str = REQUIRE_STRING(ctx, 0, "iroh.connect: first arg must be a string (address)");
    alpn = REQUIRE_STRING(ctx, 1, "iroh.connect: second arg must be a string (ALPN)");

    if (!duk_is_function(ctx, 2))
        RP_THROW(ctx, "iroh.connect: third arg must be a function (callback)");

    /* Create ephemeral endpoint */
    duk_push_current_function(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("Endpoint"));
    duk_remove(ctx, -2);
    duk_push_undefined(ctx);
    duk_new(ctx, 1);
    ep_idx = duk_get_top_index(ctx);

    /* Store auto-connect params on the endpoint */
    duk_get_prop_string(ctx, ep_idx, DUK_HIDDEN_SYMBOL("ictx"));
    ictx = (RPIROH *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (ictx) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 2);
        duk_push_string(ctx, addr_str);
        duk_put_prop_string(ctx, ep_idx, DUK_HIDDEN_SYMBOL("_ac_addr"));
        duk_push_string(ctx, alpn);
        duk_put_prop_string(ctx, ep_idx, DUK_HIDDEN_SYMBOL("_ac_alpn"));
        duk_push_uint(ctx, cb_id);
        duk_put_prop_string(ctx, ep_idx, DUK_HIDDEN_SYMBOL("_ac_cbid"));
    }

    return 1;
}

/* ======================================================================
 * Generic close method (works for any iroh object)
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_close(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("ictx"));
    {
        RPIROH *ictx = (RPIROH *)duk_get_pointer(ctx, -1);
        duk_pop_2(ctx);
        if (ictx && !(ictx->flags & RPIROH_FLAG_CLOSED))
            iroh_cleanup(ctx, ictx, WITH_CALLBACKS);
    }
    return 0;
}

/* ======================================================================
 * Gossip Constructor + Methods
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_gossip(duk_context *ctx)
{
    RPIROH *ictx, *ep_ictx;
    RPTHR *thr = get_current_thread();
    char keystr[16];
    duk_idx_t this_idx;
    IrohAsyncHandle *h;

    if (!duk_is_constructor_call(ctx))
        return DUK_RET_TYPE_ERROR;

    /* Get endpoint from arg 0 */
    if (!duk_is_object(ctx, 0))
        RP_THROW(ctx, "new Gossip: first argument must be an Endpoint object");

    duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("ictx"));
    ep_ictx = (RPIROH *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (!ep_ictx || ep_ictx->obj_type != IROH_OBJ_ENDPOINT || !ep_ictx->endpoint)
        RP_THROW(ctx, "new Gossip: argument must be a ready Endpoint");

    duk_push_this(ctx);
    this_idx = duk_get_top_index(ctx);

    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_cbs"));
    duk_push_false(ctx);
    duk_put_prop_string(ctx, this_idx, "closed");

    CALLOC(ictx, sizeof(RPIROH));
    ictx->ctx = ctx;
    ictx->thisptr = duk_get_heapptr(ctx, this_idx);
    ictx->base = thr->base;
    ictx->obj_type = IROH_OBJ_GOSSIP;
    ictx->parent = ep_ictx;
    ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, ictx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("ictx"));

    /* Store endpoint ref for later use */
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_ep"));

    duk_dup(ctx, this_idx);
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    h = iroh_gossip_create_with_router(ep_ictx->endpoint);
    if (!h) RP_THROW(ctx, "new Gossip: failed to start creation");
    iroh_start_async(ictx, h, IROH_OP_GOSSIP_CREATE, NULL);

    return 0;
}

static duk_ret_t duk_rp_iroh_gossip_subscribe(duk_context *ctx)
{
    RPIROH *topic_ictx;
    RPTHR *thr = get_current_thread();
    char keystr[16];
    IrohTopicId *topic_id;
    IrohPublicKey **peers = NULL;
    size_t npeer = 0;
    duk_idx_t obj_idx;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "gossip.subscribe: not initialized");
    if (!ictx->gossip)
        RP_THROW(ctx, "gossip.subscribe: gossip not ready");

    /* arg 0: topic name (string) */
    {
        const char *name = REQUIRE_STRING(ctx, 0,
            "gossip.subscribe: first arg must be a string (topic name)");
        topic_id = iroh_topic_id_from_name(name);
        if (!topic_id) RP_THROW(ctx, "gossip.subscribe: invalid topic name");
    }

    /* arg 1: optional array of peer public key strings */
    if (duk_is_array(ctx, 1)) {
        duk_uarridx_t i, len = (duk_uarridx_t)duk_get_length(ctx, 1);
        if (len > 0) {
            peers = calloc(len, sizeof(IrohPublicKey *));
            for (i = 0; i < len; i++) {
                duk_get_prop_index(ctx, 1, i);
                if (duk_is_string(ctx, -1)) {
                    peers[npeer] = iroh_public_key_from_string(duk_get_string(ctx, -1));
                    if (peers[npeer]) npeer++;
                }
                duk_pop(ctx);
            }
        }
    }

    /* Create GossipTopic JS object */
    duk_push_object(ctx);
    obj_idx = duk_get_top_index(ctx);

    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "iroh_topic_proto");
    duk_set_prototype(ctx, obj_idx);
    duk_pop(ctx);

    duk_push_object(ctx);
    duk_put_prop_string(ctx, obj_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, obj_idx, DUK_HIDDEN_SYMBOL("_cbs"));
    duk_push_false(ctx);
    duk_put_prop_string(ctx, obj_idx, "closed");

    CALLOC(topic_ictx, sizeof(RPIROH));
    topic_ictx->ctx = ctx;
    topic_ictx->thisptr = duk_get_heapptr(ctx, obj_idx);
    topic_ictx->base = thr->base;
    topic_ictx->obj_type = IROH_OBJ_GOSSIP_TOPIC;
    topic_ictx->parent = ictx;
    topic_ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, topic_ictx);
    duk_put_prop_string(ctx, obj_idx, DUK_HIDDEN_SYMBOL("ictx"));

    duk_dup(ctx, obj_idx);
    sprintf(keystr, "%d", (int)topic_ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    /* Start async subscribe - tracked on topic_ictx */
    h = iroh_gossip_subscribe(ictx->gossip, topic_id,
                              (const IrohPublicKey *const *)peers, npeer);
    iroh_topic_id_free(topic_id);

    {
        size_t i;
        for (i = 0; i < npeer; i++)
            if (peers[i]) iroh_public_key_free(peers[i]);
        if (peers) free(peers);
    }

    if (!h) RP_THROW(ctx, "gossip.subscribe: failed to start");
    iroh_start_async(topic_ictx, h, IROH_OP_GOSSIP_SUBSCRIBE, NULL);

    /* Return the topic object (at obj_idx, on top of stack) */
    return 1;
}

static duk_ret_t duk_rp_iroh_topic_broadcast(duk_context *ctx)
{
    const uint8_t *data;
    duk_size_t len;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "topic.broadcast: not initialized");
    if (!ictx->topic_handle) RP_THROW(ctx, "topic.broadcast: topic not ready");

    if (duk_is_string(ctx, 0)) {
        data = (const uint8_t *)duk_get_lstring(ctx, 0, &len);
    } else if (duk_is_buffer_data(ctx, 0)) {
        data = (const uint8_t *)duk_get_buffer_data(ctx, 0, &len);
    } else {
        RP_THROW(ctx, "topic.broadcast: arg must be a string or buffer");
        return 0;
    }

    h = iroh_gossip_broadcast(ictx->topic_handle, data, (size_t)len);
    if (!h) RP_THROW(ctx, "topic.broadcast: failed");
    iroh_start_async(ictx, h, IROH_OP_GOSSIP_BROADCAST, NULL);

    duk_push_this(ctx);
    return 1;
}

/* ======================================================================
 * Blobs Constructor + Methods
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_blobs(duk_context *ctx)
{
    RPIROH *ictx, *ep_ictx;
    RPTHR *thr = get_current_thread();
    char keystr[16];
    duk_idx_t this_idx;
    IrohAsyncHandle *h;
    const char *path = NULL;

    if (!duk_is_constructor_call(ctx))
        return DUK_RET_TYPE_ERROR;

    if (!duk_is_object(ctx, 0))
        RP_THROW(ctx, "new Blobs: first argument must be an Endpoint object");

    duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("ictx"));
    ep_ictx = (RPIROH *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (!ep_ictx || ep_ictx->obj_type != IROH_OBJ_ENDPOINT || !ep_ictx->endpoint)
        RP_THROW(ctx, "new Blobs: argument must be a ready Endpoint");

    /* Check options */
    if (duk_is_object(ctx, 1)) {
        if (duk_get_prop_string(ctx, 1, "path")) {
            path = duk_get_string(ctx, -1);
        }
        duk_pop(ctx);
    }

    duk_push_this(ctx);
    this_idx = duk_get_top_index(ctx);

    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_cbs"));
    duk_push_false(ctx);
    duk_put_prop_string(ctx, this_idx, "closed");

    CALLOC(ictx, sizeof(RPIROH));
    ictx->ctx = ctx;
    ictx->thisptr = duk_get_heapptr(ctx, this_idx);
    ictx->base = thr->base;
    ictx->obj_type = IROH_OBJ_BLOBS;
    ictx->parent = ep_ictx;
    ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, ictx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("ictx"));

    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_ep"));

    duk_dup(ctx, this_idx);
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    if (path)
        h = iroh_blobs_create_with_router_persistent(ep_ictx->endpoint, path);
    else
        h = iroh_blobs_create_with_router(ep_ictx->endpoint);

    if (!h) RP_THROW(ctx, "new Blobs: failed to start creation");
    iroh_start_async(ictx, h, IROH_OP_BLOBS_CREATE, NULL);

    return 0;
}

static duk_ret_t duk_rp_iroh_blobs_add_bytes(duk_context *ctx)
{
    const uint8_t *data;
    duk_size_t len;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "blobs.addBytes: not initialized");
    if (!ictx->blob_store) RP_THROW(ctx, "blobs.addBytes: blobs not ready");

    if (duk_is_string(ctx, 0)) {
        data = (const uint8_t *)duk_get_lstring(ctx, 0, &len);
    } else if (duk_is_buffer_data(ctx, 0)) {
        data = (const uint8_t *)duk_get_buffer_data(ctx, 0, &len);
    } else {
        RP_THROW(ctx, "blobs.addBytes: first arg must be a string or buffer");
        return 0;
    }

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_blobs_add_bytes(ictx->blob_store, data, (size_t)len);
    if (!h) RP_THROW(ctx, "blobs.addBytes: failed");
    iroh_start_async(ictx, h, IROH_OP_BLOBS_ADD_BYTES, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_blobs_add_file(duk_context *ctx)
{
    const char *path;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "blobs.addFile: not initialized");
    if (!ictx->blob_store) RP_THROW(ctx, "blobs.addFile: blobs not ready");

    path = REQUIRE_STRING(ctx, 0, "blobs.addFile: first arg must be a string (path)");

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_blobs_add_file(ictx->blob_store, path);
    if (!h) RP_THROW(ctx, "blobs.addFile: failed");
    iroh_start_async(ictx, h, IROH_OP_BLOBS_ADD_FILE, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_blobs_read(duk_context *ctx)
{
    const char *hash_str;
    IrohBlobHash *hash;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "blobs.read: not initialized");
    if (!ictx->blob_store) RP_THROW(ctx, "blobs.read: blobs not ready");

    hash_str = REQUIRE_STRING(ctx, 0, "blobs.read: first arg must be a string (hash)");
    hash = iroh_blob_hash_from_string(hash_str);
    if (!hash) RP_THROW(ctx, "blobs.read: invalid hash");

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_blobs_read(ictx->blob_store, hash);
    iroh_blob_hash_free(hash);
    if (!h) RP_THROW(ctx, "blobs.read: failed");
    iroh_start_async(ictx, h, IROH_OP_BLOBS_READ, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_blobs_download(duk_context *ctx)
{
    const char *ticket_str;
    IrohBlobTicket *ticket;
    RPIROH *ep_ictx;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "blobs.download: not initialized");
    if (!ictx->blobs_proto) RP_THROW(ctx, "blobs.download: blobs not ready");

    ticket_str = REQUIRE_STRING(ctx, 0, "blobs.download: first arg must be a string (ticket)");
    ticket = iroh_blob_ticket_from_string(ticket_str);
    if (!ticket) RP_THROW(ctx, "blobs.download: invalid ticket");

    ep_ictx = ictx->parent;
    if (!ep_ictx || !ep_ictx->endpoint) {
        iroh_blob_ticket_free(ticket);
        RP_THROW(ctx, "blobs.download: endpoint not available");
    }

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_blobs_download(ictx->blobs_proto, ep_ictx->endpoint, ticket);
    iroh_blob_ticket_free(ticket);
    if (!h) RP_THROW(ctx, "blobs.download: failed");
    iroh_start_async(ictx, h, IROH_OP_BLOBS_DOWNLOAD, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_blobs_ticket(duk_context *ctx)
{
    const char *hash_str;
    IrohBlobHash *hash;
    RPIROH *ep_ictx;

    GET_ICTX(ctx, ictx, "blobs.createTicket: not initialized");

    hash_str = REQUIRE_STRING(ctx, 0, "blobs.createTicket: first arg must be a string (hash)");
    hash = iroh_blob_hash_from_string(hash_str);
    if (!hash) RP_THROW(ctx, "blobs.createTicket: invalid hash");

    ep_ictx = ictx->parent;
    if (!ep_ictx || !ep_ictx->endpoint) {
        iroh_blob_hash_free(hash);
        RP_THROW(ctx, "blobs.createTicket: endpoint not available");
    }

    {
        IrohBlobTicket *ticket = iroh_blobs_create_ticket(ep_ictx->endpoint, hash);
        iroh_blob_hash_free(hash);
        if (!ticket) { duk_push_null(ctx); return 1; }
        {
            char *ts = iroh_blob_ticket_to_string(ticket);
            duk_push_string(ctx, ts ? ts : "");
            if (ts) iroh_string_free(ts);
            iroh_blob_ticket_free(ticket);
        }
    }
    return 1;
}

/* ======================================================================
 * Docs Constructor + Methods
 * ====================================================================== */

static duk_ret_t duk_rp_iroh_docs(duk_context *ctx)
{
    RPIROH *ictx, *ep_ictx;
    RPTHR *thr = get_current_thread();
    char keystr[16];
    duk_idx_t this_idx;
    IrohAsyncHandle *h;
    const char *path = NULL;

    if (!duk_is_constructor_call(ctx))
        return DUK_RET_TYPE_ERROR;

    if (!duk_is_object(ctx, 0))
        RP_THROW(ctx, "new Docs: first argument must be an Endpoint object");

    duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("ictx"));
    ep_ictx = (RPIROH *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (!ep_ictx || ep_ictx->obj_type != IROH_OBJ_ENDPOINT || !ep_ictx->endpoint)
        RP_THROW(ctx, "new Docs: argument must be a ready Endpoint");

    if (duk_is_object(ctx, 1)) {
        if (duk_get_prop_string(ctx, 1, "path"))
            path = duk_get_string(ctx, -1);
        duk_pop(ctx);
    }

    duk_push_this(ctx);
    this_idx = duk_get_top_index(ctx);

    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, "_events");
    duk_push_object(ctx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_cbs"));
    duk_push_false(ctx);
    duk_put_prop_string(ctx, this_idx, "closed");

    CALLOC(ictx, sizeof(RPIROH));
    ictx->ctx = ctx;
    ictx->thisptr = duk_get_heapptr(ctx, this_idx);
    ictx->base = thr->base;
    ictx->obj_type = IROH_OBJ_DOCS;
    ictx->parent = ep_ictx;
    ictx->thiskey = iroh_this_id++;

    duk_push_pointer(ctx, ictx);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("ictx"));

    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, this_idx, DUK_HIDDEN_SYMBOL("_ep"));

    duk_dup(ctx, this_idx);
    sprintf(keystr, "%d", (int)ictx->thiskey);
    iroh_put_gs_object(ctx, "iroh_connkeymap", keystr);
    duk_pop(ctx);

    if (path)
        h = iroh_docs_create_with_router_persistent(ep_ictx->endpoint, path);
    else
        h = iroh_docs_create_with_router(ep_ictx->endpoint);

    if (!h) RP_THROW(ctx, "new Docs: failed to start creation");
    iroh_start_async(ictx, h, IROH_OP_DOCS_CREATE, NULL);

    return 0;
}

static duk_ret_t duk_rp_iroh_docs_create_author(duk_context *ctx)
{
    void *aux = NULL;
    IrohAsyncHandle *h;
    GET_ICTX(ctx, ictx, "docs.createAuthor: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.createAuthor: docs not ready");

    if (duk_is_function(ctx, 0)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 0);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_create_author(ictx->docs);
    if (!h) RP_THROW(ctx, "docs.createAuthor: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_CREATE_AUTHOR, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_create_doc(duk_context *ctx)
{
    void *aux = NULL;
    IrohAsyncHandle *h;
    GET_ICTX(ctx, ictx, "docs.createDoc: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.createDoc: docs not ready");

    if (duk_is_function(ctx, 0)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 0);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_create_doc(ictx->docs);
    if (!h) RP_THROW(ctx, "docs.createDoc: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_CREATE_DOC, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_set(duk_context *ctx)
{
    const char *ns_str, *author_str;
    IrohNamespaceId *ns;
    IrohAuthorId *author;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.set: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.set: docs not ready");

    ns_str = REQUIRE_STRING(ctx, 0, "docs.set: arg 1 must be namespace ID string");
    author_str = REQUIRE_STRING(ctx, 1, "docs.set: arg 2 must be author ID string");

    ns = iroh_namespace_id_from_string(ns_str);
    if (!ns) RP_THROW(ctx, "docs.set: invalid namespace ID");
    author = iroh_author_id_from_string(author_str);
    if (!author) { iroh_namespace_id_free(ns); RP_THROW(ctx, "docs.set: invalid author ID"); }

    if (rp_gettype(ctx, 2) == RP_TYPE_OBJECT) {
        /* Object form: docs.set(nsId, authorId, {key: val, ...}, callback) */
        const char **keys = NULL;
        const uint8_t **values = NULL;
        size_t *value_lens = NULL;
        size_t count = 0, cap = 8;
        keys = (const char **)malloc(cap * sizeof(const char *));
        values = (const uint8_t **)malloc(cap * sizeof(const uint8_t *));
        value_lens = (size_t *)malloc(cap * sizeof(size_t));
        if (!keys || !values || !value_lens) {
            free(keys); free(values); free(value_lens);
            iroh_namespace_id_free(ns); iroh_author_id_free(author);
            RP_THROW(ctx, "docs.set: allocation failed");
        }

        duk_enum(ctx, 2, DUK_ENUM_OWN_PROPERTIES_ONLY);
        while (duk_next(ctx, -1, 1 /* get value */)) {
            const char *k = duk_get_string(ctx, -2);
            const uint8_t *v;
            duk_size_t vlen;

            if (duk_is_string(ctx, -1)) {
                v = (const uint8_t *)duk_get_lstring(ctx, -1, &vlen);
            } else if (duk_is_buffer_data(ctx, -1)) {
                v = (const uint8_t *)duk_get_buffer_data(ctx, -1, &vlen);
            } else {
                duk_pop_3(ctx); /* value, key, enum */
                free(keys); free(values); free(value_lens);
                iroh_namespace_id_free(ns); iroh_author_id_free(author);
                RP_THROW(ctx, "docs.set: object values must be strings or buffers");
            }

            if (count >= cap) {
                cap *= 2;
                keys = (const char **)realloc(keys, cap * sizeof(const char *));
                values = (const uint8_t **)realloc(values, cap * sizeof(const uint8_t *));
                value_lens = (size_t *)realloc(value_lens, cap * sizeof(size_t));
                if (!keys || !values || !value_lens) {
                    free(keys); free(values); free(value_lens);
                    duk_pop_3(ctx);
                    iroh_namespace_id_free(ns); iroh_author_id_free(author);
                    RP_THROW(ctx, "docs.set: reallocation failed");
                }
            }

            keys[count] = k;
            values[count] = v;
            value_lens[count] = (size_t)vlen;
            count++;
            duk_pop_2(ctx); /* pop key and value */
        }
        duk_pop(ctx); /* pop enum */

        if (count == 0) {
            free(keys); free(values); free(value_lens);
            iroh_namespace_id_free(ns); iroh_author_id_free(author);
            RP_THROW(ctx, "docs.set: object must have at least one property");
        }

        if (duk_is_function(ctx, 3)) {
            uint32_t cb_id = iroh_store_cb(ctx, ictx, 3);
            aux = (void *)(uintptr_t)cb_id;
        }

        h = iroh_docs_set_multi(ictx->docs, ns, author, keys, values, value_lens, count);
        free(keys); free(values); free(value_lens);
        iroh_namespace_id_free(ns);
        iroh_author_id_free(author);
        if (!h) RP_THROW(ctx, "docs.set: failed");
        iroh_start_async(ictx, h, IROH_OP_DOCS_SET, aux);
    } else if (duk_is_string(ctx, 2)) {
        /* Single form: docs.set(nsId, authorId, key, value, callback) */
        const char *key = duk_get_string(ctx, 2);
        const uint8_t *value;
        duk_size_t vlen;

        if (duk_is_string(ctx, 3)) {
            value = (const uint8_t *)duk_get_lstring(ctx, 3, &vlen);
        } else if (duk_is_buffer_data(ctx, 3)) {
            value = (const uint8_t *)duk_get_buffer_data(ctx, 3, &vlen);
        } else {
            iroh_namespace_id_free(ns); iroh_author_id_free(author);
            RP_THROW(ctx, "docs.set: arg 4 must be a string or buffer (value)");
            return 0;
        }

        if (duk_is_function(ctx, 4)) {
            uint32_t cb_id = iroh_store_cb(ctx, ictx, 4);
            aux = (void *)(uintptr_t)cb_id;
        }

        h = iroh_docs_set(ictx->docs, ns, author, key, value, (size_t)vlen);
        iroh_namespace_id_free(ns);
        iroh_author_id_free(author);
        if (!h) RP_THROW(ctx, "docs.set: failed");
        iroh_start_async(ictx, h, IROH_OP_DOCS_SET, aux);
    } else {
        iroh_namespace_id_free(ns);
        iroh_author_id_free(author);
        RP_THROW(ctx, "docs.set: arg 3 must be a key string or an object of key-value pairs");
    }

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_get(duk_context *ctx)
{
    const char *ns_str, *author_str, *key;
    IrohNamespaceId *ns;
    IrohAuthorId *author;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.get: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.get: docs not ready");

    ns_str = REQUIRE_STRING(ctx, 0, "docs.get: arg 1 must be namespace ID string");
    author_str = REQUIRE_STRING(ctx, 1, "docs.get: arg 2 must be author ID string");
    key = REQUIRE_STRING(ctx, 2, "docs.get: arg 3 must be key string");

    ns = iroh_namespace_id_from_string(ns_str);
    if (!ns) RP_THROW(ctx, "docs.get: invalid namespace ID");
    author = iroh_author_id_from_string(author_str);
    if (!author) { iroh_namespace_id_free(ns); RP_THROW(ctx, "docs.get: invalid author ID"); }

    if (duk_is_function(ctx, 3)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 3);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_get(ictx->docs, ns, author, key);
    iroh_namespace_id_free(ns);
    iroh_author_id_free(author);
    if (!h) RP_THROW(ctx, "docs.get: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_GET, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_get_latest(duk_context *ctx)
{
    const char *ns_str, *key;
    IrohNamespaceId *ns;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.getAttr: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.getAttr: docs not ready");

    ns_str = REQUIRE_STRING(ctx, 0, "docs.getAttr: arg 1 must be namespace ID string");
    key = REQUIRE_STRING(ctx, 1, "docs.getAttr: arg 2 must be key string");

    ns = iroh_namespace_id_from_string(ns_str);
    if (!ns) RP_THROW(ctx, "docs.getAttr: invalid namespace ID");

    if (duk_is_function(ctx, 2)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 2);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_get_latest(ictx->docs, ns, key);
    iroh_namespace_id_free(ns);
    if (!h) RP_THROW(ctx, "docs.getAttr: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_GET_LATEST, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_get_all(duk_context *ctx)
{
    const char *ns_str;
    IrohNamespaceId *ns;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.getAll: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.getAll: docs not ready");

    ns_str = REQUIRE_STRING(ctx, 0, "docs.getAll: arg 1 must be namespace ID string");

    ns = iroh_namespace_id_from_string(ns_str);
    if (!ns) RP_THROW(ctx, "docs.getAll: invalid namespace ID");

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_get_all(ictx->docs, ns);
    iroh_namespace_id_free(ns);
    if (!h) RP_THROW(ctx, "docs.getAll: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_GET_ALL, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_delete(duk_context *ctx)
{
    const char *ns_str, *author_str, *key;
    IrohNamespaceId *ns;
    IrohAuthorId *author;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.delete: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.delete: docs not ready");

    ns_str = REQUIRE_STRING(ctx, 0, "docs.delete: arg 1 must be namespace ID string");
    author_str = REQUIRE_STRING(ctx, 1, "docs.delete: arg 2 must be author ID string");
    key = REQUIRE_STRING(ctx, 2, "docs.delete: arg 3 must be key string");

    ns = iroh_namespace_id_from_string(ns_str);
    if (!ns) RP_THROW(ctx, "docs.delete: invalid namespace ID");
    author = iroh_author_id_from_string(author_str);
    if (!author) { iroh_namespace_id_free(ns); RP_THROW(ctx, "docs.delete: invalid author ID"); }

    if (duk_is_function(ctx, 3)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 3);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_delete(ictx->docs, ns, author, key);
    iroh_namespace_id_free(ns);
    iroh_author_id_free(author);
    if (!h) RP_THROW(ctx, "docs.delete: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_DELETE, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_ticket(duk_context *ctx)
{
    const char *ns_str;
    IrohNamespaceId *ns;
    RPIROH *ep_ictx;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.createTicket: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.createTicket: docs not ready");

    ns_str = REQUIRE_STRING(ctx, 0, "docs.createTicket: arg 1 must be namespace ID string");
    ns = iroh_namespace_id_from_string(ns_str);
    if (!ns) RP_THROW(ctx, "docs.createTicket: invalid namespace ID");

    ep_ictx = ictx->parent;
    if (!ep_ictx || !ep_ictx->endpoint) {
        iroh_namespace_id_free(ns);
        RP_THROW(ctx, "docs.createTicket: endpoint not available");
    }

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_create_ticket(ictx->docs, ns, ep_ictx->endpoint);
    iroh_namespace_id_free(ns);
    if (!h) RP_THROW(ctx, "docs.createTicket: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_CREATE_TICKET, aux);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t duk_rp_iroh_docs_join(duk_context *ctx)
{
    const char *ticket_str;
    IrohDocTicket *ticket;
    void *aux = NULL;
    IrohAsyncHandle *h;

    GET_ICTX(ctx, ictx, "docs.join: not initialized");
    if (!ictx->docs) RP_THROW(ctx, "docs.join: docs not ready");

    ticket_str = REQUIRE_STRING(ctx, 0, "docs.join: arg 1 must be a ticket string");
    ticket = iroh_doc_ticket_from_string(ticket_str);
    if (!ticket) RP_THROW(ctx, "docs.join: invalid ticket");

    if (duk_is_function(ctx, 1)) {
        uint32_t cb_id = iroh_store_cb(ctx, ictx, 1);
        aux = (void *)(uintptr_t)cb_id;
    }

    h = iroh_docs_join(ictx->docs, ticket);
    iroh_doc_ticket_free(ticket);
    if (!h) RP_THROW(ctx, "docs.join: failed");
    iroh_start_async(ictx, h, IROH_OP_DOCS_JOIN, aux);

    duk_push_this(ctx);
    return 1;
}

/* ======================================================================
 * Helper: add event emitter methods to a prototype object at obj_idx
 * ====================================================================== */

static void iroh_add_emitter_methods(duk_context *ctx, duk_idx_t obj_idx)
{
    duk_push_c_function(ctx, duk_rp_iroh_on, 2);
    duk_put_prop_string(ctx, obj_idx, "on");
    duk_push_c_function(ctx, duk_rp_iroh_once, 2);
    duk_put_prop_string(ctx, obj_idx, "once");
    duk_push_c_function(ctx, duk_rp_iroh_off, 2);
    duk_put_prop_string(ctx, obj_idx, "off");
    duk_push_c_function(ctx, duk_rp_iroh_trigger, 2);
    duk_put_prop_string(ctx, obj_idx, "trigger");
}

/* ======================================================================
 * duk_open_module — Module initialization
 * ====================================================================== */

duk_ret_t duk_open_module(duk_context *ctx)
{
    duk_idx_t mod_idx, ep_con_idx, proto_idx;

    /* Initialize iroh runtime (once) */
    if (!rp_iroh_is_init) {
        IrohError err = iroh_init();
        if (err != IROH_ERROR_OK) {
            char *msg = iroh_last_error();
            if (msg) {
                duk_push_string(ctx, msg);
                iroh_string_free(msg);
            } else {
                duk_push_string(ctx, "unknown error");
            }
            RP_THROW(ctx, "Failed to initialize iroh: %s", duk_get_string(ctx, -1));
        }
        rp_iroh_is_init = 1;
    }

    /* Module object */
    duk_push_object(ctx);
    mod_idx = duk_get_top_index(ctx);

    /* ---- Endpoint constructor + prototype ---- */
    duk_push_c_function(ctx, duk_rp_iroh_endpoint, 1);
    ep_con_idx = duk_get_top_index(ctx);

    duk_push_object(ctx); /* prototype */
    proto_idx = duk_get_top_index(ctx);

    duk_push_c_function(ctx, duk_rp_iroh_ep_connect, 3);
    duk_put_prop_string(ctx, proto_idx, "connect");
    duk_push_c_function(ctx, duk_rp_iroh_ep_close, 0);
    duk_put_prop_string(ctx, proto_idx, "close");
    duk_push_c_function(ctx, duk_rp_iroh_ep_node_id, 0);
    duk_put_prop_string(ctx, proto_idx, "getNodeId");
    duk_push_c_function(ctx, duk_rp_iroh_ep_address, 0);
    duk_put_prop_string(ctx, proto_idx, "getAddress");
    iroh_add_emitter_methods(ctx, proto_idx);

    duk_put_prop_string(ctx, ep_con_idx, "prototype");
    duk_dup(ctx, ep_con_idx);
    duk_put_prop_string(ctx, mod_idx, "Endpoint");

    /* ---- Connection prototype (stored in global stash) ---- */
    duk_push_global_stash(ctx);
    {
        duk_idx_t stash_idx = duk_get_top_index(ctx);

        duk_push_object(ctx);
        proto_idx = duk_get_top_index(ctx);

        duk_push_c_function(ctx, duk_rp_iroh_conn_open_bi, 1);
        duk_put_prop_string(ctx, proto_idx, "openBi");
        duk_push_c_function(ctx, duk_rp_iroh_conn_close, 2);
        duk_put_prop_string(ctx, proto_idx, "close");
        duk_push_c_function(ctx, duk_rp_iroh_conn_send_datagram, 1);
        duk_put_prop_string(ctx, proto_idx, "sendDatagram");
        iroh_add_emitter_methods(ctx, proto_idx);

        duk_put_prop_string(ctx, stash_idx, "iroh_conn_proto");

        /* ---- Stream prototype ---- */
        duk_push_object(ctx);
        proto_idx = duk_get_top_index(ctx);

        duk_push_c_function(ctx, duk_rp_iroh_stream_write, 1);
        duk_put_prop_string(ctx, proto_idx, "write");
        duk_push_c_function(ctx, duk_rp_iroh_stream_finish, 0);
        duk_put_prop_string(ctx, proto_idx, "finish");
        duk_push_c_function(ctx, duk_rp_iroh_stream_destroy, 0);
        duk_put_prop_string(ctx, proto_idx, "destroy");
        iroh_add_emitter_methods(ctx, proto_idx);

        duk_put_prop_string(ctx, stash_idx, "iroh_stream_proto");

        /* ---- GossipTopic prototype ---- */
        duk_push_object(ctx);
        proto_idx = duk_get_top_index(ctx);

        duk_push_c_function(ctx, duk_rp_iroh_topic_broadcast, 1);
        duk_put_prop_string(ctx, proto_idx, "broadcast");
        duk_push_c_function(ctx, duk_rp_iroh_close, 0);
        duk_put_prop_string(ctx, proto_idx, "close");
        iroh_add_emitter_methods(ctx, proto_idx);

        duk_put_prop_string(ctx, stash_idx, "iroh_topic_proto");
    }
    duk_pop(ctx); /* pop stash */

    /* ---- Gossip constructor + prototype ---- */
    duk_push_c_function(ctx, duk_rp_iroh_gossip, 1);
    {
        duk_idx_t gos_idx = duk_get_top_index(ctx);
        duk_push_object(ctx);
        proto_idx = duk_get_top_index(ctx);

        duk_push_c_function(ctx, duk_rp_iroh_gossip_subscribe, 2);
        duk_put_prop_string(ctx, proto_idx, "subscribe");
        duk_push_c_function(ctx, duk_rp_iroh_close, 0);
        duk_put_prop_string(ctx, proto_idx, "close");
        iroh_add_emitter_methods(ctx, proto_idx);

        duk_put_prop_string(ctx, gos_idx, "prototype");
    }
    duk_put_prop_string(ctx, mod_idx, "Gossip");

    /* ---- Blobs constructor + prototype ---- */
    duk_push_c_function(ctx, duk_rp_iroh_blobs, 2);
    {
        duk_idx_t blobs_idx = duk_get_top_index(ctx);
        duk_push_object(ctx);
        proto_idx = duk_get_top_index(ctx);

        duk_push_c_function(ctx, duk_rp_iroh_blobs_add_bytes, 2);
        duk_put_prop_string(ctx, proto_idx, "addBytes");
        duk_push_c_function(ctx, duk_rp_iroh_blobs_add_file, 2);
        duk_put_prop_string(ctx, proto_idx, "addFile");
        duk_push_c_function(ctx, duk_rp_iroh_blobs_read, 2);
        duk_put_prop_string(ctx, proto_idx, "read");
        duk_push_c_function(ctx, duk_rp_iroh_blobs_download, 2);
        duk_put_prop_string(ctx, proto_idx, "download");
        duk_push_c_function(ctx, duk_rp_iroh_blobs_ticket, 1);
        duk_put_prop_string(ctx, proto_idx, "createTicket");
        duk_push_c_function(ctx, duk_rp_iroh_close, 0);
        duk_put_prop_string(ctx, proto_idx, "close");
        iroh_add_emitter_methods(ctx, proto_idx);

        duk_put_prop_string(ctx, blobs_idx, "prototype");
    }
    duk_put_prop_string(ctx, mod_idx, "Blobs");

    /* ---- Docs constructor + prototype ---- */
    duk_push_c_function(ctx, duk_rp_iroh_docs, 2);
    {
        duk_idx_t docs_idx = duk_get_top_index(ctx);
        duk_push_object(ctx);
        proto_idx = duk_get_top_index(ctx);

        duk_push_c_function(ctx, duk_rp_iroh_docs_create_author, 1);
        duk_put_prop_string(ctx, proto_idx, "createAuthor");
        duk_push_c_function(ctx, duk_rp_iroh_docs_create_doc, 1);
        duk_put_prop_string(ctx, proto_idx, "createDoc");
        duk_push_c_function(ctx, duk_rp_iroh_docs_set, 5);
        duk_put_prop_string(ctx, proto_idx, "set");
        duk_push_c_function(ctx, duk_rp_iroh_docs_get, 4);
        duk_put_prop_string(ctx, proto_idx, "get");
        duk_push_c_function(ctx, duk_rp_iroh_docs_get_latest, 3);
        duk_put_prop_string(ctx, proto_idx, "getAttr");
        duk_push_c_function(ctx, duk_rp_iroh_docs_get_all, 2);
        duk_put_prop_string(ctx, proto_idx, "getAll");
        duk_push_c_function(ctx, duk_rp_iroh_docs_delete, 4);
        duk_put_prop_string(ctx, proto_idx, "delete");
        duk_push_c_function(ctx, duk_rp_iroh_docs_ticket, 2);
        duk_put_prop_string(ctx, proto_idx, "createTicket");
        duk_push_c_function(ctx, duk_rp_iroh_docs_join, 2);
        duk_put_prop_string(ctx, proto_idx, "join");
        duk_push_c_function(ctx, duk_rp_iroh_close, 0);
        duk_put_prop_string(ctx, proto_idx, "close");
        iroh_add_emitter_methods(ctx, proto_idx);

        duk_put_prop_string(ctx, docs_idx, "prototype");
    }
    duk_put_prop_string(ctx, mod_idx, "Docs");

    /* ---- Convenience functions ---- */
    duk_push_c_function(ctx, duk_rp_iroh_create_server, DUK_VARARGS);
    duk_dup(ctx, ep_con_idx);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("Endpoint"));
    duk_put_prop_string(ctx, mod_idx, "createServer");

    duk_push_c_function(ctx, duk_rp_iroh_connect_to, 3);
    duk_dup(ctx, ep_con_idx);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("Endpoint"));
    duk_put_prop_string(ctx, mod_idx, "connect");

    /* Clean up Endpoint constructor from stack */
    duk_pop(ctx); /* ep_con_idx */

    return 1;
}
