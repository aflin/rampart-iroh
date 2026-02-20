/**
 * blobs_persistent.c - Example persistent blob storage using iroh-blobs
 *
 * This example demonstrates persistent storage for iroh-blobs. Blobs survive
 * process restarts, allowing you to add blobs, exit, and serve them later.
 *
 * Storage is kept in /tmp/iroh-blobs-<pid>/ by default.
 *
 * Usage:
 *   ./blobs_persistent serve                 # Start serving blobs (shows node ID)
 *   ./blobs_persistent add <file>            # Add a file and get ticket
 *   ./blobs_persistent get <ticket>          # Download blob from ticket
 *   ./blobs_persistent list                  # List local blob hashes
 *
 * Example session:
 *   # Terminal 1: Add a file and get a ticket
 *   ./blobs_persistent add myfile.txt
 *   # Note the storage path and ticket
 *   
 *   # Ctrl+C to exit
 *   
 *   # Restart with same storage - blob still available!
 *   IROH_STORAGE=/tmp/iroh-blobs-12345 ./blobs_persistent serve
 *   
 *   # Terminal 2: Download using the ticket
 *   ./blobs_persistent get <ticket>
 */

#include <event2/event.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    IrohBlobTicket *ticket;
    char *storage_path;
    char *file_path;

    /* Command mode: 0 = serve, 1 = add, 2 = get */
    int mode;
};

/* Forward declarations */
void on_endpoint_created(evutil_socket_t fd, short what, void *arg);
void on_endpoint_online(evutil_socket_t fd, short what, void *arg);
void on_blobs_created(evutil_socket_t fd, short what, void *arg);
void on_blob_added(evutil_socket_t fd, short what, void *arg);
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

/* Get or create storage path */
char *get_storage_path(void)
{
    const char *env_path = getenv("IROH_STORAGE");
    if (env_path && strlen(env_path) > 0)
    {
        return strdup(env_path);
    }

    char *path = malloc(256);
    snprintf(path, 256, "/tmp/iroh-blobs-%d", getpid());
    return path;
}

/* Main entry point */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s serve              # Serve blobs from persistent storage\n", argv[0]);
        fprintf(stderr, "       %s add <file>         # Add file to persistent storage\n", argv[0]);
        fprintf(stderr, "       %s get <ticket>       # Download blob from ticket\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Environment variables:\n");
        fprintf(stderr, "  IROH_STORAGE  - Path to persistent storage directory\n");
        fprintf(stderr, "                  (default: /tmp/iroh-blobs-<pid>)\n");
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
    state->storage_path = get_storage_path();

    /* Create storage directory */
    mkdir(state->storage_path, 0755);

    printf("Using persistent storage: %s\n", state->storage_path);

    /* Parse command */
    if (strcmp(argv[1], "serve") == 0)
    {
        state->mode = 0;
        printf("Starting blob server with persistent storage...\n");
    }
    else if (strcmp(argv[1], "add") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Usage: %s add <file>\n", argv[0]);
            return 1;
        }
        state->mode = 1;
        state->file_path = strdup(argv[2]);

        /* Check file exists */
        if (access(state->file_path, R_OK) != 0)
        {
            fprintf(stderr, "Cannot read file: %s\n", state->file_path);
            return 1;
        }
        printf("Adding file: %s\n", state->file_path);
    }
    else if (strcmp(argv[1], "get") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Usage: %s get <ticket>\n", argv[0]);
            return 1;
        }
        state->mode = 2;
        state->ticket = iroh_blob_ticket_from_string(argv[2]);
        if (!state->ticket)
        {
            check_error("parse ticket");
            return 1;
        }
        printf("Downloading blob from ticket...\n");
    }
    else
    {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    /* Create endpoint with blobs ALPN */
    IrohEndpointConfig *config = iroh_endpoint_config_default();
    iroh_endpoint_config_add_alpn(config, "iroh-blobs/1");

    IrohAsyncHandle *handle = iroh_endpoint_create(config);
    if (!handle)
    {
        check_error("endpoint create");
        return 1;
    }

    int fd = iroh_async_get_fd(handle);
    struct event *ev = event_new(base, fd, EV_READ, on_endpoint_created, state);
    event_add(ev, NULL);

    state->endpoint = (IrohEndpoint *)handle;

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
    if (state->ticket)
        iroh_blob_ticket_free(state->ticket);
    if (state->file_path)
        free(state->file_path);
    free(state->storage_path);
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

    IrohPublicKey *id = iroh_endpoint_id(state->endpoint);
    char *id_str = iroh_public_key_to_string(id);
    printf("My endpoint ID: %s\n", id_str);
    iroh_string_free(id_str);
    iroh_public_key_free(id);

    printf("Waiting to connect to relay...\n");
    IrohAsyncHandle *online_handle = iroh_endpoint_wait_online(state->endpoint);

    int online_fd = iroh_async_get_fd(online_handle);
    struct event *ev = event_new(state->base, online_fd, EV_READ, on_endpoint_online, state);
    event_add(ev, NULL);

    state->store = (IrohBlobStore *)online_handle;
}

void on_endpoint_online(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->store;
    state->store = NULL; /* Clear to prevent double-free on error */

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
        state->store = (IrohBlobStore *)handle; /* Restore for next poll */
        int online_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, online_fd, EV_READ, on_endpoint_online, state);
        event_add(ev, NULL);
        return;
    }

    iroh_async_free(handle);
    printf("Connected to relay!\n");

    /* Create blobs with PERSISTENT storage */
    printf("Creating blobs protocol with persistent storage at: %s\n", state->storage_path);
    IrohAsyncHandle *blobs_handle = iroh_blobs_create_with_router_persistent(
        state->endpoint, state->storage_path);

    if (!blobs_handle)
    {
        check_error("blobs create persistent");
        event_base_loopbreak(state->base);
        return;
    }

    int blobs_fd = iroh_async_get_fd(blobs_handle);
    struct event *ev = event_new(state->base, blobs_fd, EV_READ, on_blobs_created, state);
    event_add(ev, NULL);

    state->store = (IrohBlobStore *)blobs_handle;
}

void on_blobs_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->store;
    state->store = NULL; /* Clear to prevent double-free on error */

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
        state->store = (IrohBlobStore *)handle; /* Restore for next poll */
        int blobs_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, blobs_fd, EV_READ, on_blobs_created, state);
        event_add(ev, NULL);
        return;
    }

    /* Extract components */
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

    printf("Blobs protocol ready with persistent storage!\n");

    if (state->mode == 0)
    {
        /* Serve mode - just wait */
        printf("\n========================================\n");
        printf("Blob server running with PERSISTENT storage!\n");
        printf("Storage path: %s\n", state->storage_path);
        printf("\nTo add files, run:\n");
        printf("  IROH_STORAGE=%s ./blobs_persistent add <file>\n", state->storage_path);
        printf("========================================\n\n");
        printf("Waiting for connections... (Ctrl+C to exit)\n");
        fflush(stdout);

        struct event *sigint_ev = evsignal_new(state->base, SIGINT, on_sigint, state);
        event_add(sigint_ev, NULL);
    }
    else if (state->mode == 1)
    {
        /* Add mode - add the file */
        printf("Adding file to persistent storage: %s\n", state->file_path);
        IrohAsyncHandle *add_handle = iroh_blobs_add_file(state->store, state->file_path);
        if (!add_handle)
        {
            check_error("add file");
            event_base_loopbreak(state->base);
            return;
        }

        int add_fd = iroh_async_get_fd(add_handle);
        struct event *ev = event_new(state->base, add_fd, EV_READ, on_blob_added, state);
        event_add(ev, NULL);

        state->ticket = (IrohBlobTicket *)add_handle;
    }
    else if (state->mode == 2)
    {
        /* Get mode - download the blob */
        printf("Downloading blob...\n");
        IrohAsyncHandle *dl_handle = iroh_blobs_download(
            state->blobs, state->endpoint, state->ticket);

        if (!dl_handle)
        {
            check_error("download");
            event_base_loopbreak(state->base);
            return;
        }

        int dl_fd = iroh_async_get_fd(dl_handle);
        struct event *ev = event_new(state->base, dl_fd, EV_READ, on_blob_downloaded, state);
        event_add(ev, NULL);

        /* Reuse pointer */
        state->blobs = (IrohBlobsProtocol *)dl_handle;
    }
}

void on_blob_added(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->ticket;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Add file failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }

    if (poll_state != IROH_ASYNC_READY)
    {
        int add_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, add_fd, EV_READ, on_blob_added, state);
        event_add(ev, NULL);
        return;
    }

    IrohBlobHash *hash = iroh_blobs_add_result(handle);
    iroh_async_free(handle);

    if (!hash)
    {
        fprintf(stderr, "Failed to get hash\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *hash_str = iroh_blob_hash_to_string(hash);
    printf("File added with hash: %s\n", hash_str);

    /* Create ticket */
    state->ticket = iroh_blobs_create_ticket(state->endpoint, hash);
    char *ticket_str = iroh_blob_ticket_to_string(state->ticket);

    printf("\n========================================\n");
    printf("File added to PERSISTENT storage!\n");
    printf("\nStorage path: %s\n", state->storage_path);
    printf("Hash: %s\n", hash_str);
    printf("\nTicket:\n%s\n", ticket_str);
    printf("\nTo serve this blob after restart:\n");
    printf("  IROH_STORAGE=%s ./blobs_persistent serve\n", state->storage_path);
    printf("\nTo download:\n");
    printf("  ./blobs_persistent get %s\n", ticket_str);
    printf("========================================\n\n");

    iroh_string_free(hash_str);
    iroh_string_free(ticket_str);
    iroh_blob_hash_free(hash);

    printf("Serving blob... (Ctrl+C to exit)\n");
    fflush(stdout);

    struct event *sigint_ev = evsignal_new(state->base, SIGINT, on_sigint, state);
    event_add(sigint_ev, NULL);
}

void on_blob_downloaded(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->blobs;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Download failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        state->blobs = NULL;
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

    IrohBlobHash *hash = iroh_blobs_download_result(handle);
    iroh_async_free(handle);
    state->blobs = NULL;

    if (!hash)
    {
        fprintf(stderr, "Failed to get downloaded hash\n");
        event_base_loopbreak(state->base);
        return;
    }

    char *hash_str = iroh_blob_hash_to_string(hash);
    printf("Downloaded blob: %s\n", hash_str);
    printf("Blob saved to persistent storage: %s\n", state->storage_path);

    /* Read and display the content */
    printf("Reading content...\n");
    IrohAsyncHandle *read_handle = iroh_blobs_read(state->store, hash);
    iroh_blob_hash_free(hash);
    iroh_string_free(hash_str);

    if (!read_handle)
    {
        check_error("read blob");
        event_base_loopbreak(state->base);
        return;
    }

    int read_fd = iroh_async_get_fd(read_handle);
    struct event *ev = event_new(state->base, read_fd, EV_READ, on_blob_read, state);
    event_add(ev, NULL);

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
        state->blobs = NULL;
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
    state->blobs = NULL;

    printf("\n========================================\n");
    printf("Blob content (%zu bytes):\n", data_len);
    if (data && data_len > 0)
    {
        /* Print as text if reasonable size, otherwise just show stats */
        if (data_len < 1024)
        {
            printf("%.*s\n", (int)data_len, data);
        }
        else
        {
            printf("[%zu bytes of data - too large to display]\n", data_len);
        }
        iroh_bytes_free(data, data_len);
    }
    else
    {
        printf("[empty]\n");
    }
    printf("========================================\n");

    event_base_loopbreak(state->base);
}

void on_sigint(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    printf("\nShutting down... Data persisted to: %s\n", state->storage_path);
    event_base_loopbreak(state->base);
}
