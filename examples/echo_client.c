/*
 * echo_client.c - Example echo client using iroh with libevent2
 *
 * This example demonstrates how to:
 *   1. Initialize iroh
 *   2. Create an endpoint
 *   3. Connect to a remote endpoint
 *   4. Open bidirectional streams
 *   5. Send and receive data
 *
 * Usage:
 *   ./echo_client <endpoint_address>
 *
 * Compile with:
 *   gcc -o echo_client echo_client.c -liroh_libevent -levent -lpthread
 */

#include <event2/event.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iroh_libevent.h"

#define ALPN_ECHO "iroh-example/echo/1"
#define MESSAGE "Hello, iroh!"

/* Forward declarations */
static void on_endpoint_created(IrohAsyncHandle *handle, void *user_data);
static void on_endpoint_online(IrohAsyncHandle *handle, void *user_data);
static void on_connected(IrohAsyncHandle *handle, void *user_data);
static void on_stream_opened(IrohAsyncHandle *handle, void *user_data);
static void on_data_written(IrohAsyncHandle *handle, void *user_data);
static void on_data_read(IrohAsyncHandle *handle, void *user_data);

/* Application context */
typedef struct
{
    struct event_base *base;
    IrohEndpoint *endpoint;
    IrohEndpointAddr *remote_addr;
    IrohConnection *conn;
    IrohSendStream *send;
    IrohRecvStream *recv;
} AppContext;

int
main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <endpoint_address>\n", argv[0]);
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s \"abc123...?relay=https://...&addr=...\"\n", argv[0]);
        return 1;
    }

    printf("Iroh Echo Client Example\n");
    printf("========================\n\n");

    /* Parse the remote endpoint address */
    IrohEndpointAddr *remote_addr = iroh_endpoint_addr_from_string(argv[1]);
    if (!remote_addr)
    {
        fprintf(stderr, "Failed to parse endpoint address\n");
        char *msg = iroh_last_error();
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        return 1;
    }

    /* Initialize iroh runtime */
    IrohError err = iroh_init();
    if (err != IROH_ERROR_OK)
    {
        fprintf(stderr, "Failed to initialize iroh: %d\n", err);
        iroh_endpoint_addr_free(remote_addr);
        return 1;
    }

    /* Create libevent base */
    struct event_base *base = event_base_new();
    if (!base)
    {
        fprintf(stderr, "Failed to create event base\n");
        iroh_endpoint_addr_free(remote_addr);
        return 1;
    }

    /* Initialize app context */
    AppContext *app = malloc(sizeof(AppContext));
    memset(app, 0, sizeof(AppContext));
    app->base = base;
    app->remote_addr = remote_addr;

    /* Create endpoint (no need for ALPNs on client side for connecting) */
    printf("Creating endpoint...\n");
    IROH_EVENT_ADD(base, iroh_endpoint_create(NULL), on_endpoint_created, app);

    /* Run the event loop */
    printf("Starting event loop...\n");
    event_base_dispatch(base);

    /* Cleanup */
    if (app->recv)
        iroh_recv_stream_free(app->recv);
    if (app->send)
        iroh_send_stream_free(app->send);
    if (app->conn)
        iroh_connection_free(app->conn);
    if (app->endpoint)
        iroh_endpoint_free(app->endpoint);
    iroh_endpoint_addr_free(app->remote_addr);
    event_base_free(base);
    free(app);

    printf("Client stopped.\n");
    return 0;
}

static void
on_endpoint_created(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to create endpoint\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        event_base_loopbreak(app->base);
        return;
    }

    app->endpoint = iroh_endpoint_create_result(handle);
    iroh_async_free(handle);

    if (!app->endpoint)
    {
        fprintf(stderr, "Failed to get endpoint result\n");
        event_base_loopbreak(app->base);
        return;
    }

    printf("Endpoint created!\n");

    /* Wait for online status */
    printf("Waiting for endpoint to be online...\n");
    IROH_EVENT_ADD(app->base, iroh_endpoint_wait_online(app->endpoint), on_endpoint_online, app);
}

static void
on_endpoint_online(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    iroh_async_free(handle);

    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to go online\n");
        event_base_loopbreak(app->base);
        return;
    }

    printf("Endpoint is online!\n");

    /* Print our endpoint ID */
    IrohPublicKey *id = iroh_endpoint_id(app->endpoint);
    if (id)
    {
        char *id_str = iroh_public_key_to_string(id);
        if (id_str)
        {
            printf("Our Endpoint ID: %s\n\n", id_str);
            iroh_string_free(id_str);
        }
        iroh_public_key_free(id);
    }

    /* Connect to remote endpoint */
    printf("Connecting to remote endpoint...\n");
    IROH_EVENT_ADD(app->base, iroh_endpoint_connect(app->endpoint, app->remote_addr, ALPN_ECHO),
                   on_connected, app);
}

static void
on_connected(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to connect\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        event_base_loopbreak(app->base);
        return;
    }

    app->conn = iroh_connection_connect_result(handle);
    iroh_async_free(handle);

    if (!app->conn)
    {
        fprintf(stderr, "Failed to get connection result\n");
        event_base_loopbreak(app->base);
        return;
    }

    printf("Connected!\n");

    /* Print remote endpoint info */
    IrohPublicKey *remote_id = iroh_connection_remote_id(app->conn);
    if (remote_id)
    {
        char *id_str = iroh_public_key_to_string(remote_id);
        if (id_str)
        {
            printf("Connected to: %s\n", id_str);
            iroh_string_free(id_str);
        }
        iroh_public_key_free(remote_id);
    }

    /* Open a bidirectional stream */
    printf("Opening stream...\n");
    IROH_EVENT_ADD(app->base, iroh_connection_open_bi(app->conn), on_stream_opened, app);
}

static void
on_stream_opened(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to open stream\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        event_base_loopbreak(app->base);
        return;
    }

    app->send = iroh_stream_open_bi_send_result(handle);
    app->recv = iroh_stream_open_bi_recv_result(handle);
    iroh_async_free(handle);

    if (!app->send || !app->recv)
    {
        fprintf(stderr, "Failed to get stream results\n");
        event_base_loopbreak(app->base);
        return;
    }

    printf("Stream opened!\n");

    /* Send the message */
    printf("Sending: %s\n", MESSAGE);
    IROH_EVENT_ADD(app->base,
                   iroh_send_stream_write(app->send, (const uint8_t *)MESSAGE, strlen(MESSAGE)),
                   on_data_written, app);
}

static void
on_data_written(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to write data\n");
        iroh_async_free(handle);
        event_base_loopbreak(app->base);
        return;
    }

    ssize_t written = iroh_send_stream_write_result(handle);
    iroh_async_free(handle);

    printf("Sent %zd bytes\n", written);

    /* Finish the send stream to signal we're done sending */
    iroh_send_stream_finish(app->send);

    /* Now read the echo response */
    printf("Waiting for echo response...\n");
    IROH_EVENT_ADD(app->base, iroh_recv_stream_read_to_end(app->recv, 65536), on_data_read, app);
}

static void
on_data_read(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to read response\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        event_base_loopbreak(app->base);
        return;
    }

    IrohReadResult result = iroh_recv_stream_read_result(handle);
    iroh_async_free(handle);

    if (result.len > 0)
    {
        printf("Received echo: %.*s\n", (int)result.len, result.data);
        iroh_bytes_free(result.data, result.len);
    }
    else
    {
        printf("Received empty response\n");
    }

    /* Close the connection gracefully */
    iroh_connection_close(app->conn, 0, "bye!");
    app->conn = NULL;

    printf("\nEcho test completed successfully!\n");

    /* Exit the event loop */
    event_base_loopbreak(app->base);
}
