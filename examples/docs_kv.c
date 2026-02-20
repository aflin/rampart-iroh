/**
 * docs_kv.c - Example synchronized key-value store using iroh-docs
 *
 * This example demonstrates how to use iroh-docs for a distributed
 * key-value store that automatically syncs between peers.
 *
 * Usage:
 *   ./docs_kv create                    # Create a new document, prints ticket
 *   ./docs_kv join <ticket>             # Join an existing document
 *   ./docs_kv set <ticket> <key> <val>  # Set a key-value pair
 *   ./docs_kv get <ticket> <key>        # Get a value by key
 */

#include <event2/event.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iroh_libevent.h"

/* Application state */
struct app_state
{
    struct event_base *base;
    IrohEndpoint *endpoint;
    IrohDocs *docs;
    IrohBlobStore *store;
    IrohRouter *router;
    IrohAuthorId *author;
    IrohNamespaceId *namespace_id;
    IrohDocTicket *ticket;

    /* Command arguments */
    int mode; /* 0 = create, 1 = join, 2 = set, 3 = get */
    const char *key;
    const char *value;
};

/* Forward declarations */
void on_endpoint_created(evutil_socket_t fd, short what, void *arg);
void on_endpoint_online(evutil_socket_t fd, short what, void *arg);
void on_docs_created(evutil_socket_t fd, short what, void *arg);
void on_author_created(evutil_socket_t fd, short what, void *arg);
void on_doc_created(evutil_socket_t fd, short what, void *arg);
void on_ticket_created(evutil_socket_t fd, short what, void *arg);
void on_doc_joined(evutil_socket_t fd, short what, void *arg);
void on_value_set(evutil_socket_t fd, short what, void *arg);
void on_value_got(evutil_socket_t fd, short what, void *arg);
void on_sigint(evutil_socket_t fd, short what, void *arg);

/* Error handling helper */
void check_error(const char *msg)
{
    char *err = iroh_last_error();
    if (err)
    {
        fprintf(stderr, "Error: %s: %s\n", msg, err);
        iroh_string_free(err);
        exit(1);
    }
}

/* Main entry point */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s create\n", argv[0]);
        fprintf(stderr, "       %s join <ticket>\n", argv[0]);
        fprintf(stderr, "       %s set <ticket> <key> <value>\n", argv[0]);
        fprintf(stderr, "       %s get <ticket> <key>\n", argv[0]);
        return 1;
    }

    /* Initialize iroh */
    IrohError err = iroh_init();
    if (err != IROH_ERROR_OK)
    {
        check_error("iroh_init");
        return 1;
    }

    /* Create libevent base */
    struct event_base *base = event_base_new();
    if (!base)
    {
        fprintf(stderr, "Failed to create event base\n");
        return 1;
    }

    /* Initialize app state */
    struct app_state *state = calloc(1, sizeof(struct app_state));
    state->base = base;

    /* Parse command */
    if (strcmp(argv[1], "create") == 0)
    {
        state->mode = 0;
        printf("Creating new document...\n");
    }
    else if (strcmp(argv[1], "join") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Usage: %s join <ticket>\n", argv[0]);
            return 1;
        }
        state->mode = 1;
        state->ticket = iroh_doc_ticket_from_string(argv[2]);
        if (!state->ticket)
        {
            check_error("parse ticket");
            return 1;
        }
        printf("Joining document...\n");
    }
    else if (strcmp(argv[1], "set") == 0)
    {
        if (argc < 5)
        {
            fprintf(stderr, "Usage: %s set <ticket> <key> <value>\n", argv[0]);
            return 1;
        }
        state->mode = 2;
        state->ticket = iroh_doc_ticket_from_string(argv[2]);
        if (!state->ticket)
        {
            check_error("parse ticket");
            return 1;
        }
        state->key = argv[3];
        state->value = argv[4];
        printf("Setting key '%s' to '%s'...\n", state->key, state->value);
    }
    else if (strcmp(argv[1], "get") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr, "Usage: %s get <ticket> <key>\n", argv[0]);
            return 1;
        }
        state->mode = 3;
        state->ticket = iroh_doc_ticket_from_string(argv[2]);
        if (!state->ticket)
        {
            check_error("parse ticket");
            return 1;
        }
        state->key = argv[3];
        printf("Getting key '%s'...\n", state->key);
    }
    else
    {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    /* Create endpoint */
    IrohAsyncHandle *handle = iroh_endpoint_create(NULL);
    if (!handle)
    {
        check_error("endpoint create");
        return 1;
    }

    /* Add to event loop */
    int fd = iroh_async_get_fd(handle);
    struct event *ev = event_new(base, fd, EV_READ, on_endpoint_created, state);
    event_add(ev, NULL);

    /* Store handle for callback */
    state->endpoint = (IrohEndpoint *)handle;

    printf("Starting docs key-value store...\n");
    event_base_dispatch(base);

    /* Cleanup */
    if (state->router)
        iroh_router_free(state->router);
    if (state->store)
        iroh_blobs_store_free(state->store);
    if (state->docs)
        iroh_docs_free(state->docs);
    if (state->endpoint)
        iroh_endpoint_free(state->endpoint);
    if (state->author)
        iroh_author_id_free(state->author);
    if (state->namespace_id)
        iroh_namespace_id_free(state->namespace_id);
    if (state->ticket)
        iroh_doc_ticket_free(state->ticket);
    event_base_free(base);
    free(state);
    return 0;
}

void on_endpoint_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->endpoint;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Endpoint creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int ep_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, ep_fd, EV_READ, on_endpoint_created, state);
        event_add(ev, NULL);
        return;
    }

    state->endpoint = iroh_endpoint_create_result(handle);
    iroh_async_free(handle);

    if (!state->endpoint)
    {
        fprintf(stderr, "Failed to get endpoint\n");
        event_base_loopbreak(state->base);
        return;
    }

    /* Get our ID */
    IrohPublicKey *id = iroh_endpoint_id(state->endpoint);
    char *id_str = iroh_public_key_to_string(id);
    printf("My endpoint ID: %s\n", id_str);
    iroh_string_free(id_str);
    iroh_public_key_free(id);

    /* Wait for online */
    printf("Waiting to connect to relay...\n");
    IrohAsyncHandle *online_handle = iroh_endpoint_wait_online(state->endpoint);

    int online_fd = iroh_async_get_fd(online_handle);
    struct event *ev = event_new(state->base, online_fd, EV_READ, on_endpoint_online, state);
    event_add(ev, NULL);

    /* Store handle temporarily */
    state->docs = (IrohDocs *)online_handle;
}

void on_endpoint_online(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->docs;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Wait online failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int online_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, online_fd, EV_READ, on_endpoint_online, state);
        event_add(ev, NULL);
        return;
    }

    iroh_endpoint_wait_online_result(handle);
    iroh_async_free(handle);

    printf("Online! Creating docs protocol...\n");

    /* Create docs with router (includes blobs and gossip) */
    IrohAsyncHandle *docs_handle = iroh_docs_create_with_router(state->endpoint);
    if (!docs_handle)
    {
        check_error("docs create");
        event_base_loopbreak(state->base);
        return;
    }

    int docs_fd = iroh_async_get_fd(docs_handle);
    struct event *ev = event_new(state->base, docs_fd, EV_READ, on_docs_created, state);
    event_add(ev, NULL);

    /* Store handle temporarily */
    state->docs = (IrohDocs *)docs_handle;
}

void on_docs_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->docs;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Docs creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int docs_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, docs_fd, EV_READ, on_docs_created, state);
        event_add(ev, NULL);
        return;
    }

    /* Get docs, store, and router */
    state->docs = iroh_docs_from_router_result(handle);
    state->store = iroh_blobs_store_from_docs_result(handle);
    state->router = iroh_router_from_docs_result(handle);
    iroh_async_free(handle);

    if (!state->docs || !state->store || !state->router)
    {
        fprintf(stderr, "Failed to get docs components\n");
        event_base_loopbreak(state->base);
        return;
    }

    printf("Docs protocol created.\n");

    /* Create an author */
    printf("Creating author...\n");
    IrohAsyncHandle *author_handle = iroh_docs_create_author(state->docs);
    if (!author_handle)
    {
        check_error("create author");
        event_base_loopbreak(state->base);
        return;
    }

    int author_fd = iroh_async_get_fd(author_handle);
    struct event *ev = event_new(state->base, author_fd, EV_READ, on_author_created, state);
    event_add(ev, NULL);

    /* Store handle temporarily */
    state->author = (IrohAuthorId *)author_handle;
}

void on_author_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->author;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Author creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int author_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, author_fd, EV_READ, on_author_created, state);
        event_add(ev, NULL);
        return;
    }

    state->author = iroh_docs_create_author_result(handle);
    iroh_async_free(handle);

    if (!state->author)
    {
        fprintf(stderr, "Failed to get author\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *author_str = iroh_author_id_to_string(state->author);
    printf("Author created: %s\n", author_str);
    iroh_string_free(author_str);

    /* Now proceed based on mode */
    if (state->mode == 0)
    {
        /* Create mode: create a new document */
        printf("Creating new document...\n");
        IrohAsyncHandle *doc_handle = iroh_docs_create_doc(state->docs);
        if (!doc_handle)
        {
            check_error("create doc");
            event_base_loopbreak(state->base);
            return;
        }

        int doc_fd = iroh_async_get_fd(doc_handle);
        struct event *ev = event_new(state->base, doc_fd, EV_READ, on_doc_created, state);
        event_add(ev, NULL);

        state->namespace_id = (IrohNamespaceId *)doc_handle;
    }
    else
    {
        /* Join/set/get mode: join the document from ticket */
        printf("Joining document from ticket...\n");
        IrohAsyncHandle *join_handle = iroh_docs_join(state->docs, state->ticket);
        if (!join_handle)
        {
            check_error("join doc");
            event_base_loopbreak(state->base);
            return;
        }

        int join_fd = iroh_async_get_fd(join_handle);
        struct event *ev = event_new(state->base, join_fd, EV_READ, on_doc_joined, state);
        event_add(ev, NULL);

        state->namespace_id = (IrohNamespaceId *)join_handle;
    }
}

void on_doc_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->namespace_id;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Doc creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int doc_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, doc_fd, EV_READ, on_doc_created, state);
        event_add(ev, NULL);
        return;
    }

    state->namespace_id = iroh_docs_create_doc_result(handle);
    iroh_async_free(handle);

    if (!state->namespace_id)
    {
        fprintf(stderr, "Failed to get namespace ID\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *ns_str = iroh_namespace_id_to_string(state->namespace_id);
    printf("Document created: %s\n", ns_str);
    iroh_string_free(ns_str);

    /* Create a ticket for sharing */
    printf("Creating ticket...\n");
    IrohAsyncHandle *ticket_handle = iroh_docs_create_ticket(state->docs, state->namespace_id, state->endpoint);
    if (!ticket_handle)
    {
        check_error("create ticket");
        event_base_loopbreak(state->base);
        return;
    }

    int ticket_fd = iroh_async_get_fd(ticket_handle);
    struct event *ev = event_new(state->base, ticket_fd, EV_READ, on_ticket_created, state);
    event_add(ev, NULL);

    state->ticket = (IrohDocTicket *)ticket_handle;
}

void on_ticket_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->ticket;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Ticket creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int ticket_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, ticket_fd, EV_READ, on_ticket_created, state);
        event_add(ev, NULL);
        return;
    }

    state->ticket = iroh_docs_create_ticket_result(handle);
    iroh_async_free(handle);

    if (!state->ticket)
    {
        fprintf(stderr, "Failed to get ticket\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *ticket_str = iroh_doc_ticket_to_string(state->ticket);
    printf("\n========================================\n");
    printf("Document created! Use this ticket to join:\n");
    printf("%s\n", ticket_str);
    printf("========================================\n\n");
    printf("Waiting for peers... (Ctrl+C to exit)\n");
    fflush(stdout);
    iroh_string_free(ticket_str);

    /* Add signal handler to keep running */
    struct event *sigint_ev = evsignal_new(state->base, SIGINT, on_sigint, state);
    event_add(sigint_ev, NULL);
}

void on_doc_joined(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->namespace_id;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Doc join failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int join_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, join_fd, EV_READ, on_doc_joined, state);
        event_add(ev, NULL);
        return;
    }

    state->namespace_id = iroh_docs_join_result(handle);
    iroh_async_free(handle);

    if (!state->namespace_id)
    {
        fprintf(stderr, "Failed to get namespace ID from join\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *ns_str = iroh_namespace_id_to_string(state->namespace_id);
    printf("Joined document: %s\n", ns_str);
    iroh_string_free(ns_str);

    if (state->mode == 1)
    {
        /* Join mode: just print success and wait */
        printf("\n========================================\n");
        printf("Successfully joined document!\n");
        printf("========================================\n\n");
        printf("Waiting for sync... (Ctrl+C to exit)\n");
        fflush(stdout);

        struct event *sigint_ev = evsignal_new(state->base, SIGINT, on_sigint, state);
        event_add(sigint_ev, NULL);
    }
    else if (state->mode == 2)
    {
        /* Set mode: set the key-value pair and wait for sync */
        printf("Setting key '%s' and waiting for peers to sync...\n", state->key);
        IrohAsyncHandle *set_handle = iroh_docs_set_and_sync(
            state->docs,
            state->namespace_id,
            state->author,
            state->key,
            (const uint8_t *)state->value,
            strlen(state->value),
            5); /* 5 second wait for peers to sync */

        if (!set_handle)
        {
            check_error("set_and_sync");
            event_base_loopbreak(state->base);
            return;
        }

        int set_fd = iroh_async_get_fd(set_handle);
        struct event *ev = event_new(state->base, set_fd, EV_READ, on_value_set, state);
        event_add(ev, NULL);

        /* Reuse a pointer to store handle */
        state->store = (IrohBlobStore *)set_handle;
    }
    else if (state->mode == 3)
    {
        /* Get mode: use get_latest to get value from any author */
        printf("Getting key '%s'...\n", state->key);
        IrohAsyncHandle *get_handle = iroh_docs_get_latest(
            state->docs,
            state->namespace_id,
            state->key);

        if (!get_handle)
        {
            check_error("get");
            event_base_loopbreak(state->base);
            return;
        }

        int get_fd = iroh_async_get_fd(get_handle);
        struct event *ev = event_new(state->base, get_fd, EV_READ, on_value_got, state);
        event_add(ev, NULL);

        /* Reuse a pointer to store handle */
        state->store = (IrohBlobStore *)get_handle;
    }
}

void on_value_set(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->store;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Set and sync failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        state->store = NULL; /* Prevent double-free in cleanup */
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int set_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, set_fd, EV_READ, on_value_set, state);
        event_add(ev, NULL);
        return;
    }

    IrohError err = iroh_docs_set_and_sync_result(handle);
    iroh_async_free(handle);
    state->store = NULL;

    if (err != IROH_ERROR_OK)
    {
        fprintf(stderr, "Set and sync operation failed\n");
        event_base_loopbreak(state->base);
        return;
    }

    printf("\n========================================\n");
    printf("Successfully set and synced '%s' = '%s'\n", state->key, state->value);
    printf("========================================\n");
    
    event_base_loopbreak(state->base);
}

void on_value_got(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->store;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Get failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        state->store = NULL; /* Prevent double-free in cleanup */
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int get_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, get_fd, EV_READ, on_value_got, state);
        event_add(ev, NULL);
        return;
    }

    size_t value_len = 0;
    uint8_t *value = iroh_docs_get_latest_result(handle, &value_len);
    iroh_async_free(handle);
    state->store = NULL;

    printf("\n========================================\n");
    if (value && value_len > 0)
    {
        printf("'%s' = '%.*s'\n", state->key, (int)value_len, value);
        iroh_bytes_free(value, value_len);
    }
    else
    {
        printf("Key '%s' not found (or empty)\n", state->key);
    }
    printf("========================================\n");

    event_base_loopbreak(state->base);
}

void on_sigint(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    printf("\nShutting down...\n");
    event_base_loopbreak(state->base);
}
