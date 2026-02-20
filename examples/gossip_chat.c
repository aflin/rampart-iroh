/**
 * gossip_chat.c - Example gossip chat using iroh-libevent
 *
 * This example demonstrates how to use iroh-gossip for pub/sub messaging.
 *
 * Usage:
 *   ./gossip_chat create                    # Create a new topic
 *   ./gossip_chat join <topic_id> <peer_id> # Join an existing topic
 */

#include <event2/event.h>
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
    IrohGossip *gossip;
    IrohRouter *router;
    IrohGossipTopic *topic_handle;
    IrohTopicId *topic;
    IrohAsyncHandle *recv_handle;  /* Current pending recv handle */
    char *my_name;
    char *bootstrap_peer;
};

/* Forward declarations */
void on_endpoint_created(evutil_socket_t fd, short what, void *arg);
void on_endpoint_online(evutil_socket_t fd, short what, void *arg);
void on_gossip_created(evutil_socket_t fd, short what, void *arg);
void on_subscribed(evutil_socket_t fd, short what, void *arg);
void on_gossip_event(evutil_socket_t fd, short what, void *arg);
void on_stdin(evutil_socket_t fd, short what, void *arg);
void start_recv_loop(struct app_state *state);

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
        fprintf(stderr, "       %s join <topic_id> <peer_id>\n", argv[0]);
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
    state->my_name = getenv("USER");
    if (!state->my_name)
        state->my_name = "anonymous";

    /* Parse command */
    if (strcmp(argv[1], "create") == 0)
    {
        /* Create a new topic with random ID */
        uint8_t topic_bytes[32];
        for (int i = 0; i < 32; i++)
        {
            topic_bytes[i] = rand() & 0xFF;
        }
        state->topic = iroh_topic_id_from_bytes(topic_bytes);
    }
    else if (strcmp(argv[1], "join") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr, "Usage: %s join <topic_id> <peer_id>\n", argv[0]);
            return 1;
        }
        state->topic = iroh_topic_id_from_string(argv[2]);
        if (!state->topic)
        {
            check_error("parse topic");
            return 1;
        }
        state->bootstrap_peer = argv[3];
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
    state->endpoint = (IrohEndpoint *)handle; /* Temporary storage */

    printf("Starting gossip chat...\n");
    event_base_dispatch(base);

    /* Cleanup */
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
        /* Not ready yet, re-add event */
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
    state->gossip = (IrohGossip *)online_handle;
}

void on_endpoint_online(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->gossip;

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
        /* Not ready yet, re-add event */
        int online_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, online_fd, EV_READ, on_endpoint_online, state);
        event_add(ev, NULL);
        return;
    }

    iroh_endpoint_wait_online_result(handle);
    iroh_async_free(handle);

    printf("Online! Creating gossip protocol with router...\n");

    /* Create gossip with router (required for incoming connections) */
    IrohAsyncHandle *gossip_handle = iroh_gossip_create_with_router(state->endpoint);
    if (!gossip_handle)
    {
        check_error("gossip create with router");
        event_base_loopbreak(state->base);
        return;
    }

    int gossip_fd = iroh_async_get_fd(gossip_handle);
    struct event *ev = event_new(state->base, gossip_fd, EV_READ, on_gossip_created, state);
    event_add(ev, NULL);

    /* Store handle temporarily */
    state->gossip = (IrohGossip *)gossip_handle;
}

void on_gossip_created(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->gossip;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Gossip creation failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        /* Not ready yet, re-add event */
        int gossip_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, gossip_fd, EV_READ, on_gossip_created, state);
        event_add(ev, NULL);
        return;
    }

    /* Get gossip first, then router */
    state->gossip = iroh_gossip_from_router_result(handle);
    state->router = iroh_router_from_gossip_result(handle);
    iroh_async_free(handle);

    if (!state->gossip)
    {
        fprintf(stderr, "Failed to get gossip\n");
        event_base_loopbreak(state->base);
        return;
    }

    if (!state->router)
    {
        fprintf(stderr, "Failed to get router\n");
        event_base_loopbreak(state->base);
        return;
    }

    printf("Gossip protocol and router created.\n");

    /* Get topic string */
    char *topic_str = iroh_topic_id_to_string(state->topic);
    printf("Topic ID: %s\n", topic_str);
    iroh_string_free(topic_str);

    /* Subscribe to topic */
    const IrohPublicKey *bootstrap_peers[1] = {NULL};
    size_t bootstrap_count = 0;

    IrohPublicKey *bootstrap_peer = NULL;
    if (state->bootstrap_peer)
    {
        bootstrap_peer = iroh_public_key_from_string(state->bootstrap_peer);
        if (bootstrap_peer)
        {
            bootstrap_peers[0] = bootstrap_peer;
            bootstrap_count = 1;
            printf("Bootstrapping from peer: %s\n", state->bootstrap_peer);
        }
    }

    IrohAsyncHandle *sub_handle = iroh_gossip_subscribe(
        state->gossip,
        state->topic,
        bootstrap_count > 0 ? bootstrap_peers : NULL,
        bootstrap_count);

    if (bootstrap_peer)
    {
        iroh_public_key_free(bootstrap_peer);
    }

    if (!sub_handle)
    {
        check_error("gossip subscribe");
        event_base_loopbreak(state->base);
        return;
    }

    int sub_fd = iroh_async_get_fd(sub_handle);
    struct event *ev = event_new(state->base, sub_fd, EV_READ, on_subscribed, state);
    event_add(ev, NULL);

    /* Store handle temporarily */
    state->topic_handle = (IrohGossipTopic *)sub_handle;
}

void on_subscribed(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;
    IrohAsyncHandle *handle = (IrohAsyncHandle *)state->topic_handle;

    IrohAsyncState poll_state = iroh_async_poll(handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(handle);
        fprintf(stderr, "Subscribe failed: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(handle);
        event_base_loopbreak(state->base);
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        /* Not ready yet, re-add event */
        int sub_fd = iroh_async_get_fd(handle);
        struct event *ev = event_new(state->base, sub_fd, EV_READ, on_subscribed, state);
        event_add(ev, NULL);
        return;
    }

    state->topic_handle = iroh_gossip_subscribe_result(handle);
    iroh_async_free(handle);

    if (!state->topic_handle)
    {
        fprintf(stderr, "Failed to get topic handle\n");
        event_base_loopbreak(state->base);
        return;
    }

    printf("Subscribed to topic! Type messages and press Enter to send.\n");
    printf("-----------------------------------------------------------\n");

    /* Start receiving gossip events */
    start_recv_loop(state);

    /* Listen for stdin */
    struct event *stdin_ev = event_new(state->base, STDIN_FILENO, EV_READ | EV_PERSIST, on_stdin, state);
    event_add(stdin_ev, NULL);
}

void start_recv_loop(struct app_state *state)
{
    IrohAsyncHandle *recv_handle = iroh_gossip_recv(state->topic_handle);
    if (!recv_handle)
    {
        fprintf(stderr, "Failed to start recv\n");
        return;
    }
    
    /* Store handle for use in callback */
    state->recv_handle = recv_handle;
    
    int recv_fd = iroh_async_get_fd(recv_handle);
    struct event *recv_ev = event_new(state->base, recv_fd, EV_READ, on_gossip_event, state);
    event_add(recv_ev, NULL);
}

void on_gossip_event(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;

    /* Use the stored recv handle */
    IrohAsyncHandle *recv_handle = state->recv_handle;
    if (!recv_handle)
    {
        fprintf(stderr, "No recv handle\n");
        return;
    }

    IrohAsyncState poll_state = iroh_async_poll(recv_handle);
    if (poll_state == IROH_ASYNC_ERROR)
    {
        char *err = iroh_async_get_error(recv_handle);
        fprintf(stderr, "Recv error: %s\n", err);
        iroh_string_free(err);
        iroh_async_free(recv_handle);
        state->recv_handle = NULL;
        return;
    }
    
    if (poll_state != IROH_ASYNC_READY)
    {
        /* Re-add event for next time */
        int recv_fd = iroh_async_get_fd(recv_handle);
        struct event *recv_ev = event_new(state->base, recv_fd, EV_READ, on_gossip_event, state);
        event_add(recv_ev, NULL);
        return;
    }

    IrohGossipEvent event = iroh_gossip_recv_result(recv_handle);
    iroh_async_free(recv_handle);
    state->recv_handle = NULL;

    switch (event.event_type)
    {
    case IROH_GOSSIP_EVENT_RECEIVED:
        if (event.data && event.data_len > 0)
        {
            /* Print message (null-terminate it) */
            char *msg = malloc(event.data_len + 1);
            memcpy(msg, event.data, event.data_len);
            msg[event.data_len] = '\0';
            printf("%s\n", msg);
            fflush(stdout);
            free(msg);
        }
        break;

    case IROH_GOSSIP_EVENT_NEIGHBORUP:
        if (event.peer)
        {
            char *peer_str = iroh_public_key_to_string(event.peer);
            printf("*** Peer joined: %s\n", peer_str);
            fflush(stdout);
            iroh_string_free(peer_str);
        }
        break;

    case IROH_GOSSIP_EVENT_NEIGHBORDOWN:
        if (event.peer)
        {
            char *peer_str = iroh_public_key_to_string(event.peer);
            printf("*** Peer left: %s\n", peer_str);
            fflush(stdout);
            iroh_string_free(peer_str);
        }
        break;

    default:
        break;
    }

    iroh_gossip_event_free(&event);

    /* Start next recv */
    start_recv_loop(state);
}

void on_stdin(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    struct app_state *state = (struct app_state *)arg;

    char buf[1024];
    if (fgets(buf, sizeof(buf), stdin) == NULL)
    {
        event_base_loopbreak(state->base);
        return;
    }

    /* Remove newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
    {
        buf[len - 1] = '\0';
        len--;
    }

    if (len == 0)
        return;

    /* Format message with name */
    char msg[1200];
    snprintf(msg, sizeof(msg), "<%s> %s", state->my_name, buf);

    /* Broadcast message */
    IrohAsyncHandle *handle = iroh_gossip_broadcast(state->topic_handle, (uint8_t *)msg, strlen(msg));
    if (handle)
    {
        /* For simplicity, we won't wait for completion */
        iroh_async_free(handle);
    }
}

