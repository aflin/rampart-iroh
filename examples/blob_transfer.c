/**
 * blob_transfer.c - Example file transfer using iroh-blobs
 *
 * This example demonstrates how to use iroh-blobs for content-addressed
 * file transfers. Files are identified by their BLAKE3 hash and can be
 * fetched from any peer that has them.
 *
 * Usage:
 *   ./blob_transfer send <file>           # Share a file, prints ticket
 *   ./blob_transfer receive <ticket>      # Download a file using ticket
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
    IrohBlobStore *store;
    IrohBlobsProtocol *blobs;
    IrohRouter *router;
    
    /* For send mode */
    const char *file_path;
    IrohBlobHash *hash;
    
    /* For receive mode */
    IrohBlobTicket *ticket;
    
    int mode; /* 0 = send, 1 = receive */
};

/* Forward declarations */
void on_endpoint_created(evutil_socket_t fd, short what, void *arg);
void on_endpoint_online(evutil_socket_t fd, short what, void *arg);
void on_blobs_created(evutil_socket_t fd, short what, void *arg);
void on_file_added(evutil_socket_t fd, short what, void *arg);
void on_blob_downloaded(evutil_socket_t fd, short what, void *arg);
void on_blob_read(evutil_socket_t fd, short what, void *arg);
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
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s send <file>\n", argv[0]);
        fprintf(stderr, "       %s receive <ticket>\n", argv[0]);
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
    if (strcmp(argv[1], "send") == 0)
    {
        state->mode = 0;
        state->file_path = argv[2];
        printf("Preparing to share file: %s\n", state->file_path);
    }
    else if (strcmp(argv[1], "receive") == 0)
    {
        state->mode = 1;
        state->ticket = iroh_blob_ticket_from_string(argv[2]);
        if (!state->ticket)
        {
            check_error("parse ticket");
            return 1;
        }
        printf("Preparing to download blob...\n");
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

    printf("Starting blob transfer...\n");
    event_base_dispatch(base);

    /* Cleanup */
    if (state->router)
        iroh_router_free(state->router);
    if (state->blobs)
        iroh_blobs_protocol_free(state->blobs);
    if (state->store)
        iroh_blobs_store_free(state->store);
    if (state->endpoint)
        iroh_endpoint_free(state->endpoint);
    if (state->hash)
        iroh_blob_hash_free(state->hash);
    if (state->ticket)
        iroh_blob_ticket_free(state->ticket);
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

    /* Store handle temporarily (reuse store pointer) */
    state->store = (IrohBlobStore *)online_handle;
}

void on_endpoint_online(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->store;

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

    printf("Online! Creating blobs protocol with router...\n");

    /* Create blobs with router */
    IrohAsyncHandle *blobs_handle = iroh_blobs_create_with_router(state->endpoint);
    if (!blobs_handle)
    {
        check_error("blobs create");
        event_base_loopbreak(state->base);
        return;
    }

    int blobs_fd = iroh_async_get_fd(blobs_handle);
    struct event *ev = event_new(state->base, blobs_fd, EV_READ, on_blobs_created, state);
    event_add(ev, NULL);

    /* Store handle temporarily */
    state->store = (IrohBlobStore *)blobs_handle;
}

void on_blobs_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->store;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Blobs creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        int blobs_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, blobs_fd, EV_READ, on_blobs_created, state);
        event_add(ev, NULL);
        return;
    }

    /* Get store, blobs protocol, and router */
    state->store = iroh_blobs_store_from_router_result(handle);
    state->blobs = iroh_blobs_protocol_from_router_result(handle);
    state->router = iroh_router_from_blobs_result(handle);
    iroh_async_free(handle);

    if (!state->store || !state->blobs || !state->router)
    {
        fprintf(stderr, "Failed to get blobs components\n");
        event_base_loopbreak(state->base);
        return;
    }

    printf("Blobs protocol and router created.\n");

    if (state->mode == 0)
    {
        /* Send mode: add file to store */
        printf("Adding file to store...\n");
        IrohAsyncHandle *add_handle = iroh_blobs_add_file(state->store, state->file_path);
        if (!add_handle)
        {
            check_error("add file");
            event_base_loopbreak(state->base);
            return;
        }

        int add_fd = iroh_async_get_fd(add_handle);
        struct event *ev = event_new(state->base, add_fd, EV_READ, on_file_added, state);
        event_add(ev, NULL);

        /* Store handle temporarily (reuse hash pointer) */
        state->hash = (IrohBlobHash *)add_handle;
    }
    else
    {
        /* Receive mode: download blob */
        printf("Starting download...\n");
        IrohAsyncHandle *dl_handle = iroh_blobs_download(state->blobs, state->endpoint, state->ticket);
        if (!dl_handle)
        {
            check_error("download");
            event_base_loopbreak(state->base);
            return;
        }

        int dl_fd = iroh_async_get_fd(dl_handle);
        struct event *ev = event_new(state->base, dl_fd, EV_READ, on_blob_downloaded, state);
        event_add(ev, NULL);

        /* Store handle temporarily */
        state->hash = (IrohBlobHash *)dl_handle;
    }
}

void on_file_added(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->hash;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "File add failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        int add_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, add_fd, EV_READ, on_file_added, state);
        event_add(ev, NULL);
        return;
    }

    state->hash = iroh_blobs_add_result(handle);
    iroh_async_free(handle);

    if (!state->hash)
    {
        fprintf(stderr, "Failed to get blob hash\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *hash_str = iroh_blob_hash_to_string(state->hash);
    printf("File added with hash: %s\n", hash_str);
    iroh_string_free(hash_str);

    /* Create ticket */
    IrohBlobTicket *ticket = iroh_blobs_create_ticket(state->endpoint, state->hash);
    if (!ticket)
    {
        check_error("create ticket");
        event_base_loopbreak(state->base);
        return;
    }

    char *ticket_str = iroh_blob_ticket_to_string(ticket);
    printf("\n========================================\n");
    printf("Share this ticket to download the file:\n");
    printf("%s\n", ticket_str);
    printf("========================================\n\n");
    printf("Waiting for connections... (Ctrl+C to exit)\n");
    fflush(stdout);
    iroh_string_free(ticket_str);
    iroh_blob_ticket_free(ticket);

    /* Add a signal handler to keep the event loop running and handle Ctrl+C */
    struct event *sigint_ev = evsignal_new(state->base, SIGINT, on_sigint, state);
    event_add(sigint_ev, NULL);
}

void on_blob_downloaded(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->hash;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Download failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        int dl_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, dl_fd, EV_READ, on_blob_downloaded, state);
        event_add(ev, NULL);
        return;
    }

    state->hash = iroh_blobs_download_result(handle);
    iroh_async_free(handle);

    if (!state->hash)
    {
        fprintf(stderr, "Failed to get downloaded blob hash\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *hash_str = iroh_blob_hash_to_string(state->hash);
    printf("Downloaded blob: %s\n", hash_str);
    iroh_string_free(hash_str);

    /* Read the blob data */
    printf("Reading blob data...\n");
    IrohAsyncHandle *read_handle = iroh_blobs_read(state->store, state->hash);
    if (!read_handle)
    {
        check_error("read blob");
        event_base_loopbreak(state->base);
        return;
    }

    int read_fd = iroh_async_get_fd(read_handle);
    struct event *ev = event_new(state->base, read_fd, EV_READ, on_blob_read, state);
    event_add(ev, NULL);

    /* Store handle temporarily (reuse blobs pointer since we're done with it) */
    state->blobs = (IrohBlobsProtocol *)read_handle;
}

void on_blob_read(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->blobs;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Read failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        int read_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, read_fd, EV_READ, on_blob_read, state);
        event_add(ev, NULL);
        return;
    }

    size_t data_len = 0;
    uint8_t *data = iroh_blobs_read_result(handle, &data_len);
    iroh_async_free(handle);
    state->blobs = NULL; /* Clear since we reused the pointer */

    if (!data || data_len == 0)
    {
        fprintf(stderr, "Failed to read blob data\n");
        event_base_loopbreak(state->base);
        return;
    }

    printf("Received %zu bytes\n", data_len);

    /* Write to stdout or a file */
    /* For this example, we'll write to stdout if small, or save to received.dat */
    if (data_len < 1024)
    {
        printf("Content:\n");
        fwrite(data, 1, data_len, stdout);
        printf("\n");
    }
    else
    {
        FILE *f = fopen("received.dat", "wb");
        if (f)
        {
            fwrite(data, 1, data_len, f);
            fclose(f);
            printf("Saved to received.dat\n");
        }
        else
        {
            fprintf(stderr, "Failed to save file\n");
        }
    }

    iroh_bytes_free(data, data_len);

    printf("Transfer complete!\n");
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
