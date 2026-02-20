/*
 * echo_server.c - Example echo server using iroh with libevent2
 *
 * This example demonstrates how to:
 *   1. Initialize iroh
 *   2. Create an endpoint
 *   3. Accept incoming connections
 *   4. Handle bidirectional streams
 *   5. Echo received data back
 *
 * Compile with:
 *   gcc -o echo_server echo_server.c -liroh_libevent -levent -lpthread
 */

#include <event2/event.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iroh_libevent.h"

#define ALPN_ECHO "iroh-example/echo/1"

/* Forward declarations */
static void on_endpoint_created(IrohAsyncHandle *handle, void *user_data);
static void on_endpoint_online(IrohAsyncHandle *handle, void *user_data);
static void on_connection_accepted(IrohAsyncHandle *handle, void *user_data);
static void on_stream_accepted(IrohAsyncHandle *handle, void *user_data);
static void on_data_read(IrohAsyncHandle *handle, void *user_data);
static void on_data_written(IrohAsyncHandle *handle, void *user_data);

/* Application context */
typedef struct
{
    struct event_base *base;
    IrohEndpoint *endpoint;
    bool running;
} AppContext;

/* Stream context for echo handling */
typedef struct
{
    AppContext *app;
    IrohConnection *conn;
    IrohSendStream *send;
    IrohRecvStream *recv;
} StreamContext;

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Iroh Echo Server Example\n");
    printf("========================\n\n");

    /* Initialize iroh runtime */
    IrohError err = iroh_init();
    if (err != IROH_ERROR_OK)
    {
        fprintf(stderr, "Failed to initialize iroh: %d\n", err);
        char *msg = iroh_last_error();
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        return 1;
    }

    /* Create libevent base */
    struct event_base *base = event_base_new();
    if (!base)
    {
        fprintf(stderr, "Failed to create event base\n");
        return 1;
    }

    /* Initialize app context */
    AppContext *app = malloc(sizeof(AppContext));
    app->base = base;
    app->endpoint = NULL;
    app->running = true;

    /* Create endpoint configuration */
    IrohEndpointConfig *config = iroh_endpoint_config_default();
    iroh_endpoint_config_add_alpn(config, ALPN_ECHO);

    /* Start creating the endpoint (config is consumed) */
    printf("Creating endpoint...\n");
    IROH_EVENT_ADD(base, iroh_endpoint_create(config), on_endpoint_created, app);

    /* Run the event loop */
    printf("Starting event loop...\n");
    event_base_dispatch(base);

    /* Cleanup */
    if (app->endpoint)
    {
        iroh_endpoint_free(app->endpoint);
    }
    event_base_free(base);
    free(app);

    printf("Server stopped.\n");
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

    /* Get the endpoint */
    app->endpoint = iroh_endpoint_create_result(handle);
    iroh_async_free(handle);

    if (!app->endpoint)
    {
        fprintf(stderr, "Failed to get endpoint result\n");
        event_base_loopbreak(app->base);
        return;
    }

    printf("Endpoint created!\n");

    /* Wait for the endpoint to be online */
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

    /* Print the endpoint address */
    IrohEndpointAddr *addr = iroh_endpoint_addr(app->endpoint);
    if (addr)
    {
        char *addr_str = iroh_endpoint_addr_to_string(addr);
        if (addr_str)
        {
            printf("\nEndpoint Address:\n%s\n\n", addr_str);
            iroh_string_free(addr_str);
        }
        iroh_endpoint_addr_free(addr);
    }

    IrohPublicKey *id = iroh_endpoint_id(app->endpoint);
    if (id)
    {
        char *id_str = iroh_public_key_to_string(id);
        if (id_str)
        {
            printf("Endpoint ID: %s\n\n", id_str);
            iroh_string_free(id_str);
        }
        iroh_public_key_free(id);
    }

    printf("Waiting for connections...\n");

    /* Start accepting connections */
    IROH_EVENT_ADD(app->base, iroh_endpoint_accept(app->endpoint), on_connection_accepted, app);
}

static void
on_connection_accepted(IrohAsyncHandle *handle, void *user_data)
{
    AppContext *app = (AppContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to accept connection\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        /* Continue accepting */
        IROH_EVENT_ADD(app->base, iroh_endpoint_accept(app->endpoint), on_connection_accepted,
                       app);
        return;
    }

    IrohConnection *conn = iroh_connection_accept_result(handle);
    iroh_async_free(handle);

    if (!conn)
    {
        fprintf(stderr, "Failed to get connection result\n");
        IROH_EVENT_ADD(app->base, iroh_endpoint_accept(app->endpoint), on_connection_accepted,
                       app);
        return;
    }

    /* Print remote endpoint info */
    IrohPublicKey *remote_id = iroh_connection_remote_id(conn);
    if (remote_id)
    {
        char *id_str = iroh_public_key_to_string(remote_id);
        if (id_str)
        {
            printf("New connection from: %s\n", id_str);
            iroh_string_free(id_str);
        }
        iroh_public_key_free(remote_id);
    }

    /* Create stream context */
    StreamContext *ctx = malloc(sizeof(StreamContext));
    ctx->app = app;
    ctx->conn = conn;
    ctx->send = NULL;
    ctx->recv = NULL;

    /* Accept a bidirectional stream */
    IROH_EVENT_ADD(app->base, iroh_connection_accept_bi(conn), on_stream_accepted, ctx);

    /* Continue accepting more connections */
    IROH_EVENT_ADD(app->base, iroh_endpoint_accept(app->endpoint), on_connection_accepted, app);
}

static void
on_stream_accepted(IrohAsyncHandle *handle, void *user_data)
{
    StreamContext *ctx = (StreamContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to accept stream\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        iroh_connection_free(ctx->conn);
        free(ctx);
        return;
    }

    /* Get both streams */
    ctx->send = iroh_stream_open_bi_send_result(handle);
    ctx->recv = iroh_stream_open_bi_recv_result(handle);
    iroh_async_free(handle);

    if (!ctx->send || !ctx->recv)
    {
        fprintf(stderr, "Failed to get stream results\n");
        if (ctx->send)
            iroh_send_stream_free(ctx->send);
        if (ctx->recv)
            iroh_recv_stream_free(ctx->recv);
        iroh_connection_free(ctx->conn);
        free(ctx);
        return;
    }

    printf("Stream accepted, starting echo...\n");

    /* Start reading data */
    IROH_EVENT_ADD(ctx->app->base, iroh_recv_stream_read(ctx->recv, 65536), on_data_read, ctx);
}

static void
on_data_read(IrohAsyncHandle *handle, void *user_data)
{
    StreamContext *ctx = (StreamContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to read data\n");
        char *msg = iroh_async_get_error(handle);
        if (msg)
        {
            fprintf(stderr, "Error: %s\n", msg);
            iroh_string_free(msg);
        }
        iroh_async_free(handle);
        goto cleanup;
    }

    IrohReadResult result = iroh_recv_stream_read_result(handle);
    iroh_async_free(handle);

    if (result.finished)
    {
        printf("Stream finished\n");
        /* Finish the send stream to signal we're done sending */
        iroh_send_stream_finish(ctx->send);
        /* 
         * IMPORTANT: Don't close the connection!
         * According to QUIC protocol, the receiver of the last data (the client)
         * should close the connection. If the server closes it, the data may be lost.
         * The connection will be closed when the client closes it.
         * We just free the streams and let the connection handle stay alive.
         */
        iroh_send_stream_free(ctx->send);
        iroh_recv_stream_free(ctx->recv);
        /* Note: We intentionally don't free the connection here.
         * In a real application, you'd want to track connections and clean them up
         * when they're closed by the peer. For this example, we let them leak
         * since the server is meant to run indefinitely. */
        free(ctx);
        printf("Echo complete, waiting for client to close connection\n");
        return;
    }

    if (result.len == 0)
    {
        /* No data, continue reading */
        IROH_EVENT_ADD(ctx->app->base, iroh_recv_stream_read(ctx->recv, 65536), on_data_read, ctx);
        return;
    }

    printf("Received %zu bytes, echoing back...\n", result.len);

    /* Echo the data back */
    /* We need to save the data for the write callback to free it */
    /* For simplicity, we'll just write it directly */
    IROH_EVENT_ADD(ctx->app->base, iroh_send_stream_write(ctx->send, result.data, result.len),
                   on_data_written, ctx);

    /* Free the received data */
    iroh_bytes_free(result.data, result.len);
    return;

cleanup:
    iroh_send_stream_free(ctx->send);
    iroh_recv_stream_free(ctx->recv);
    /* Don't close connection on error either - let it timeout naturally */
    free(ctx);
}

static void
on_data_written(IrohAsyncHandle *handle, void *user_data)
{
    StreamContext *ctx = (StreamContext *)user_data;

    IrohAsyncState state = iroh_async_poll(handle);
    if (state != IROH_ASYNC_READY)
    {
        fprintf(stderr, "Failed to write data\n");
        iroh_async_free(handle);
        iroh_send_stream_free(ctx->send);
        iroh_recv_stream_free(ctx->recv);
        iroh_connection_close(ctx->conn, 1, "write error");
        free(ctx);
        return;
    }

    ssize_t written = iroh_send_stream_write_result(handle);
    iroh_async_free(handle);

    printf("Wrote %zd bytes\n", written);

    /* Continue reading */
    IROH_EVENT_ADD(ctx->app->base, iroh_recv_stream_read(ctx->recv, 65536), on_data_read, ctx);
}
