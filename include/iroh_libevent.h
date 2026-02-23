/**
 * iroh-libevent: C API for iroh with libevent2 integration
 *
 * Mostly coded by Claude Opus 4.5 - Feb 2026.
 * Copyright (C) 2026 Aaron Flin - All Rights Reserved
 * You may use, distribute or alter this code under the
 * terms of the MIT license
 * see https://opensource.org/licenses/MIT
 *
 * Supports: iroh core, iroh-blobs, iroh-gossip, iroh-docs
 *
 * Usage pattern:
 * 1. Call iroh_init() to initialize the runtime
 * 2. Create an endpoint with iroh_endpoint_create()
 * 3. Use protocols (gossip, blobs, docs) as needed
 * 4. Integrate with libevent2 using the helper macros or iroh_async_get_fd()
 */

#ifndef IROH_LIBEVENT_H
#define IROH_LIBEVENT_H

#include <event2/event.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ========================================================================
     * Error Handling
     * ======================================================================== */

    typedef enum
    {
        IROH_ERROR_OK = 0,
        IROH_ERROR_INVALID_ARGUMENT = 1,
        IROH_ERROR_OUT_OF_MEMORY = 2,
        IROH_ERROR_TIMEOUT = 3,
        IROH_ERROR_CONNECTION_FAILED = 4,
        IROH_ERROR_STREAM_ERROR = 5,
        IROH_ERROR_ENDPOINT_CLOSED = 6,
        IROH_ERROR_PENDING = 7,
        IROH_ERROR_INTERNAL = 8,
        IROH_ERROR_PARSE_ERROR = 9,
        IROH_ERROR_NOT_INITIALIZED = 10,
        IROH_ERROR_ALREADY_INITIALIZED = 11,
        IROH_ERROR_BLOB_NOT_FOUND = 12,
        IROH_ERROR_TOPIC_ERROR = 13,
        IROH_ERROR_DOC_ERROR = 14,
    } IrohError;

    /** Get the last error message. Returns NULL if no error. Free with iroh_string_free(). */
    char *iroh_last_error(void);

    /** Free a string returned by iroh functions. */
    void iroh_string_free(char *s);

    /* ========================================================================
     * Async Handle (for libevent2 integration)
     * ======================================================================== */

    typedef enum
    {
        IROH_ASYNC_PENDING = 0,
        IROH_ASYNC_READY = 1,
        IROH_ASYNC_ERROR = 2,
        IROH_ASYNC_CANCELLED = 3,
    } IrohAsyncState;

    /** Opaque handle for async operations */
    typedef struct IrohAsyncHandle IrohAsyncHandle;

    /** Get the file descriptor for this async handle. Add to libevent with EV_READ. */
    int iroh_async_get_fd(const IrohAsyncHandle *handle);

    /** Poll the async operation state. */
    IrohAsyncState iroh_async_poll(const IrohAsyncHandle *handle);

    /** Get the error message from a failed async operation. Free with iroh_string_free(). */
    char *iroh_async_get_error(const IrohAsyncHandle *handle);

    /** Cancel an async operation. */
    void iroh_async_cancel(IrohAsyncHandle *handle);

    /** Free an async handle. */
    void iroh_async_free(IrohAsyncHandle *handle);

    /* ========================================================================
     * Initialization
     * ======================================================================== */

    /** Initialize the iroh runtime. Must be called before any other iroh functions. */
    IrohError iroh_init(void);

    /** Initialize with a custom number of worker threads. */
    IrohError iroh_init_with_threads(int num_threads);

    /* ========================================================================
     * Key Types
     * ======================================================================== */

    typedef struct IrohSecretKey IrohSecretKey;
    typedef struct IrohPublicKey IrohPublicKey;

    /** Generate a new random secret key. */
    IrohSecretKey *iroh_secret_key_generate(void);

    /** Parse a secret key from a base32 string. */
    IrohSecretKey *iroh_secret_key_from_string(const char *s);

    /** Convert a secret key to bytes (32 bytes). Free with iroh_bytes_free(). */
    uint8_t *iroh_secret_key_to_bytes(const IrohSecretKey *key, size_t *out_len);

    /** Get the public key from a secret key. */
    IrohPublicKey *iroh_secret_key_public(const IrohSecretKey *key);

    /** Free a secret key. */
    void iroh_secret_key_free(IrohSecretKey *key);

    /** Parse a public key from a base32 string. */
    IrohPublicKey *iroh_public_key_from_string(const char *s);

    /** Convert a public key to a string. Free with iroh_string_free(). */
    char *iroh_public_key_to_string(const IrohPublicKey *key);

    /** Free a public key. */
    void iroh_public_key_free(IrohPublicKey *key);

    /* ========================================================================
     * Endpoint Address
     * ======================================================================== */

    typedef struct IrohEndpointAddr IrohEndpointAddr;

    /** Create an endpoint address from just a public key. */
    IrohEndpointAddr *iroh_endpoint_addr_from_public_key(const IrohPublicKey *key);

    /** Create an endpoint address with a relay URL. */
    IrohEndpointAddr *iroh_endpoint_addr_with_relay(const IrohPublicKey *key, const char *relay_url);

    /** Parse an endpoint address from a string. Format: "<public_key>" or "<public_key>?relay=<url>" */
    IrohEndpointAddr *iroh_endpoint_addr_from_string(const char *s);

    /** Get the public key from an endpoint address. */
    IrohPublicKey *iroh_endpoint_addr_public_key(const IrohEndpointAddr *addr);

    /** Convert an endpoint address to a string. Free with iroh_string_free(). */
    char *iroh_endpoint_addr_to_string(const IrohEndpointAddr *addr);

    /** Free an endpoint address. */
    void iroh_endpoint_addr_free(IrohEndpointAddr *addr);

    /* ========================================================================
     * Endpoint Configuration
     * ======================================================================== */

    /** Opaque endpoint configuration (use builder methods to configure) */
    typedef struct IrohEndpointConfig IrohEndpointConfig;

    /** Relay mode for endpoint configuration */
    typedef enum
    {
        IROH_RELAY_MODE_DISABLED = 0, /** Disable relay servers completely */
        IROH_RELAY_MODE_DEFAULT = 1,  /** Use default n0 relay servers */
        IROH_RELAY_MODE_CUSTOM = 2,   /** Use custom relay URL */
    } IrohRelayMode;

    /** Create a default endpoint configuration.
     *  Returns an opaque config that should be configured with builder methods
     *  and then passed to iroh_endpoint_create().
     *  Free with iroh_endpoint_config_free() if not passed to iroh_endpoint_create(). */
    IrohEndpointConfig *iroh_endpoint_config_default(void);

    /** Free an endpoint configuration.
     *  Note: After passing config to iroh_endpoint_create(), it is consumed
     *  and should NOT be freed separately. */
    void iroh_endpoint_config_free(IrohEndpointConfig *config);

    /* --- Identity --- */

    /** Set the secret key for the endpoint.
     *  Pass NULL to generate a new random key.
     *  @param config The endpoint configuration
     *  @param key    The secret key (ownership NOT transferred) */
    void iroh_endpoint_config_secret_key(IrohEndpointConfig *config, const IrohSecretKey *key);

    /** Set the secret key from raw bytes (must be exactly 32 bytes).
     *  @param config    The endpoint configuration
     *  @param key_bytes Pointer to 32 bytes of key data
     *  @param len       Length of key data (must be 32) */
    void iroh_endpoint_config_secret_key_bytes(IrohEndpointConfig *config, const uint8_t *key_bytes, size_t len);

    /* --- ALPNs --- */

    /** Add an ALPN protocol string.
     *  @param config The endpoint configuration
     *  @param alpn   The ALPN protocol string (e.g., "iroh-example/echo/1") */
    void iroh_endpoint_config_add_alpn(IrohEndpointConfig *config, const char *alpn);

    /** Set multiple ALPN protocols at once (replaces any existing ALPNs).
     *  @param config The endpoint configuration
     *  @param alpns  Array of ALPN protocol strings
     *  @param count  Number of ALPN strings in the array */
    void iroh_endpoint_config_set_alpns(IrohEndpointConfig *config, const char *const *alpns, size_t count);

    /** Clear all ALPN protocols. */
    void iroh_endpoint_config_clear_alpns(IrohEndpointConfig *config);

    /* --- Discovery --- */

    /** Enable or disable n0 (number zero) discovery service.
     *  This uses Iroh's public discovery infrastructure.
     *  Default: enabled.
     *  @param config  The endpoint configuration
     *  @param enabled true to enable, false to disable */
    void iroh_endpoint_config_discovery_n0(IrohEndpointConfig *config, bool enabled);

    /** Enable or disable DNS discovery.
     *  Default: enabled.
     *  @param config  The endpoint configuration
     *  @param enabled true to enable, false to disable */
    void iroh_endpoint_config_discovery_dns(IrohEndpointConfig *config, bool enabled);

    /** Enable or disable publishing to pkarr (public key addressable resource records).
     *  Default: enabled.
     *  @param config  The endpoint configuration
     *  @param enabled true to enable, false to disable */
    void iroh_endpoint_config_discovery_pkarr_publish(IrohEndpointConfig *config, bool enabled);

    /* --- Relay --- */

    /** Set the relay mode.
     *  - IROH_RELAY_MODE_DISABLED: No relay servers
     *  - IROH_RELAY_MODE_DEFAULT: Use default n0 relay servers
     *  - IROH_RELAY_MODE_CUSTOM: Use custom relay URL (set via iroh_endpoint_config_relay_url)
     *  @param config The endpoint configuration
     *  @param mode   The relay mode */
    void iroh_endpoint_config_relay_mode(IrohEndpointConfig *config, IrohRelayMode mode);

    /** Set a custom relay URL. Also sets relay mode to IROH_RELAY_MODE_CUSTOM.
     *  @param config The endpoint configuration
     *  @param url    The relay URL (e.g., "https://my-relay.example.com") */
    void iroh_endpoint_config_relay_url(IrohEndpointConfig *config, const char *url);

    /* --- Network --- */

    /** Set the UDP port to bind to.
     *  Pass 0 for automatic port selection (default behavior).
     *  @param config The endpoint configuration
     *  @param port   The UDP port number (0 for automatic) */
    void iroh_endpoint_config_bind_port(IrohEndpointConfig *config, uint16_t port);

    /** Set the IPv4 address to bind to.
     *  Pass NULL to bind to all interfaces (default).
     *  @param config The endpoint configuration
     *  @param addr   IPv4 address string (e.g., "192.168.1.100") */
    void iroh_endpoint_config_bind_addr_v4(IrohEndpointConfig *config, const char *addr);

    /** Set the IPv6 address to bind to.
     *  Pass NULL to bind to all interfaces (default).
     *  @param config The endpoint configuration
     *  @param addr   IPv6 address string (e.g., "::1") */
    void iroh_endpoint_config_bind_addr_v6(IrohEndpointConfig *config, const char *addr);

    /* --- Transport --- */

    /** Set the maximum idle timeout in milliseconds.
     *  Connections idle for longer than this will be closed.
     *  Pass 0 to use the default timeout.
     *  @param config     The endpoint configuration
     *  @param timeout_ms Maximum idle timeout in milliseconds (0 for default) */
    void iroh_endpoint_config_max_idle_timeout(IrohEndpointConfig *config, uint64_t timeout_ms);

    /** Set the keep-alive interval in milliseconds.
     *  Keep-alive packets are sent to prevent the connection from going idle.
     *  Pass 0 to disable keep-alives.
     *  @param config      The endpoint configuration
     *  @param interval_ms Keep-alive interval in milliseconds (0 to disable) */
    void iroh_endpoint_config_keep_alive_interval(IrohEndpointConfig *config, uint64_t interval_ms);

    /* ========================================================================
     * Endpoint
     * ======================================================================== */

    typedef struct IrohEndpoint IrohEndpoint;

    /** Create an endpoint asynchronously using the provided configuration.
     *  The configuration is consumed and should not be used or freed after this call.
     *  Pass NULL for config to use default settings.
     *  @param config The endpoint configuration (consumed, do not free) */
    IrohAsyncHandle *iroh_endpoint_create(IrohEndpointConfig *config);

    /** Get the endpoint from a completed create operation. */
    IrohEndpoint *iroh_endpoint_create_result(IrohAsyncHandle *handle);

    /** Get the endpoint's public key (endpoint ID). */
    IrohPublicKey *iroh_endpoint_id(const IrohEndpoint *endpoint);

    /** Get the endpoint's secret key as 32 raw bytes. Free with iroh_bytes_free(). */
    uint8_t *iroh_endpoint_secret_key(const IrohEndpoint *endpoint, size_t *out_len);

    /** Get the endpoint's address. */
    IrohEndpointAddr *iroh_endpoint_addr(const IrohEndpoint *endpoint);

    /** Set the ALPN protocols for the endpoint. */
    void iroh_endpoint_set_alpns(const IrohEndpoint *endpoint, const char *const *alpns, size_t count);

    /** Close the endpoint asynchronously. */
    IrohAsyncHandle *iroh_endpoint_close(IrohEndpoint *endpoint);

    /** Free an endpoint without closing it gracefully. */
    void iroh_endpoint_free(IrohEndpoint *endpoint);

    /** Wait for the endpoint to be online (connected to relay). */
    IrohAsyncHandle *iroh_endpoint_wait_online(const IrohEndpoint *endpoint);

    /** Check if wait_online completed. */
    IrohError iroh_endpoint_wait_online_result(IrohAsyncHandle *handle);

    /* ========================================================================
     * Connection
     * ======================================================================== */

    typedef struct IrohConnection IrohConnection;

    /** Connect to a remote endpoint asynchronously. */
    IrohAsyncHandle *iroh_endpoint_connect(const IrohEndpoint *endpoint, const IrohEndpointAddr *addr, const char *alpn);

    /** Get the connection from a completed connect operation. */
    IrohConnection *iroh_connection_connect_result(IrohAsyncHandle *handle);

    /** Accept an incoming connection asynchronously. */
    IrohAsyncHandle *iroh_endpoint_accept(const IrohEndpoint *endpoint);

    /** Get the connection from a completed accept operation. */
    IrohConnection *iroh_connection_accept_result(IrohAsyncHandle *handle);

    /** Get the remote endpoint ID of a connection. */
    IrohPublicKey *iroh_connection_remote_id(const IrohConnection *conn);

    /** Get the ALPN protocol used by the connection. Free with iroh_string_free(). */
    char *iroh_connection_alpn(const IrohConnection *conn);

    /** Close a connection with an error code and reason. */
    void iroh_connection_close(IrohConnection *conn, uint32_t code, const char *reason);

    /** Free a connection. */
    void iroh_connection_free(IrohConnection *conn);

    /* ========================================================================
     * Streams
     * ======================================================================== */

    typedef struct IrohSendStream IrohSendStream;
    typedef struct IrohRecvStream IrohRecvStream;

    /** Open a bidirectional stream asynchronously. */
    IrohAsyncHandle *iroh_connection_open_bi(const IrohConnection *conn);

    /** Get the send stream from a completed open_bi operation. */
    IrohSendStream *iroh_stream_open_bi_send_result(IrohAsyncHandle *handle);

    /** Get the recv stream from a completed open_bi operation (call after getting send). */
    IrohRecvStream *iroh_stream_open_bi_recv_result(IrohAsyncHandle *handle);

    /** Accept a bidirectional stream asynchronously. */
    IrohAsyncHandle *iroh_connection_accept_bi(const IrohConnection *conn);

    /** Open a unidirectional send stream asynchronously. */
    IrohAsyncHandle *iroh_connection_open_uni(const IrohConnection *conn);

    /** Get the send stream from a completed open_uni operation. */
    IrohSendStream *iroh_stream_open_uni_result(IrohAsyncHandle *handle);

    /** Accept a unidirectional receive stream asynchronously. */
    IrohAsyncHandle *iroh_connection_accept_uni(const IrohConnection *conn);

    /** Get the recv stream from a completed accept_uni operation. */
    IrohRecvStream *iroh_stream_accept_uni_result(IrohAsyncHandle *handle);

    /* ========================================================================
     * Stream I/O
     * ======================================================================== */

    typedef struct
    {
        uint8_t *data; /* Owned, free with iroh_bytes_free() */
        size_t len;
        bool finished; /* True if stream has ended */
    } IrohReadResult;

    /** Write data to a send stream asynchronously. Data is copied. */
    IrohAsyncHandle *iroh_send_stream_write(IrohSendStream *stream, const uint8_t *data, size_t len);

    /** Get the number of bytes written. Returns -1 on error. */
    ssize_t iroh_send_stream_write_result(IrohAsyncHandle *handle);

    /** Finish the send stream (signal end of data). */
    IrohError iroh_send_stream_finish(IrohSendStream *stream);

    /** Free a send stream. */
    void iroh_send_stream_free(IrohSendStream *stream);

    /** Read data from a receive stream asynchronously. */
    IrohAsyncHandle *iroh_recv_stream_read(IrohRecvStream *stream, size_t max_len);

    /** Read all data until end of stream. */
    IrohAsyncHandle *iroh_recv_stream_read_to_end(IrohRecvStream *stream, size_t max_len);

    /** Get the read result. Free data with iroh_bytes_free(). */
    IrohReadResult iroh_recv_stream_read_result(IrohAsyncHandle *handle);

    /** Free bytes returned by read operations. */
    void iroh_bytes_free(uint8_t *data, size_t len);

    /** Free a receive stream. */
    void iroh_recv_stream_free(IrohRecvStream *stream);

    /* ========================================================================
     * Datagrams
     * ======================================================================== */

    /** Send a datagram (synchronous, data is copied). */
    IrohError iroh_connection_send_datagram(const IrohConnection *conn, const uint8_t *data, size_t len);

    /** Receive a datagram asynchronously. */
    IrohAsyncHandle *iroh_connection_recv_datagram(const IrohConnection *conn);

    /** Get the datagram data. Free with iroh_bytes_free(). */
    uint8_t *iroh_connection_recv_datagram_result(IrohAsyncHandle *handle, size_t *out_len);

    /* ========================================================================
     * IROH-BLOBS PROTOCOL
     * ======================================================================== */

    typedef struct IrohBlobHash IrohBlobHash;
    typedef struct IrohBlobStore IrohBlobStore;

    /** Parse a blob hash from a hex or base32 string. */
    IrohBlobHash *iroh_blob_hash_from_string(const char *s);

    /** Convert a blob hash to a string. Free with iroh_string_free(). */
    char *iroh_blob_hash_to_string(const IrohBlobHash *hash);

    /** Get the raw bytes of a blob hash (32 bytes). Free with iroh_bytes_free(). */
    uint8_t *iroh_blob_hash_to_bytes(const IrohBlobHash *hash, size_t *out_len);

    /** Free a blob hash. */
    void iroh_blob_hash_free(IrohBlobHash *hash);

    /** Create an in-memory blob store. */
    IrohBlobStore *iroh_blob_store_memory(void);

    /** Create a filesystem blob store at the given path (async). */
    IrohAsyncHandle *iroh_blob_store_filesystem(const char *path);

    /** Get the blob store from a completed filesystem store creation. */
    IrohBlobStore *iroh_blob_store_filesystem_result(IrohAsyncHandle *handle);

    /** Free a blob store. */
    void iroh_blob_store_free(IrohBlobStore *store);

    /* ========================================================================
     * IROH-GOSSIP PROTOCOL
     * ======================================================================== */

    typedef struct IrohTopicId IrohTopicId;
    typedef struct IrohGossip IrohGossip;
    typedef struct IrohGossipTopic IrohGossipTopic;

    /** Create a topic ID from 32 bytes. */
    IrohTopicId *iroh_topic_id_from_bytes(const uint8_t *bytes);

    /** Create a topic ID from a name string by hashing it with BLAKE3.
     *  This is useful for creating human-readable topic names. */
    IrohTopicId *iroh_topic_id_from_name(const char *name);

    /** Create a topic ID from a string (hex or base32). */
    IrohTopicId *iroh_topic_id_from_string(const char *s);

    /** Convert a topic ID to string. Free with iroh_string_free(). */
    char *iroh_topic_id_to_string(const IrohTopicId *topic);

    /** Get the raw bytes of a topic ID (32 bytes). Free with iroh_bytes_free(). */
    uint8_t *iroh_topic_id_to_bytes(const IrohTopicId *topic, size_t *out_len);

    /** Free a topic ID. */
    void iroh_topic_id_free(IrohTopicId *topic);

    /** Create a gossip protocol instance (async). */
    IrohAsyncHandle *iroh_gossip_create(const IrohEndpoint *endpoint);

    /** Get the gossip instance from a completed create operation. */
    IrohGossip *iroh_gossip_create_result(IrohAsyncHandle *handle);

    /** Get the ALPN for gossip protocol. Free with iroh_string_free(). */
    char *iroh_gossip_alpn(void);

    /**
     * Subscribe to a gossip topic.
     * bootstrap_peers: array of IrohPublicKey pointers (can be NULL)
     * bootstrap_count: number of bootstrap peers
     */
    IrohAsyncHandle *iroh_gossip_subscribe(
        const IrohGossip *gossip,
        const IrohTopicId *topic,
        const IrohPublicKey *const *bootstrap_peers,
        size_t bootstrap_count);

    /** Get the gossip topic from a completed subscribe operation. */
    IrohGossipTopic *iroh_gossip_subscribe_result(IrohAsyncHandle *handle);

    /** Broadcast a message to all peers in the topic. */
    IrohAsyncHandle *iroh_gossip_broadcast(const IrohGossipTopic *topic, const uint8_t *data, size_t len);

    /** Check if broadcast completed successfully. */
    IrohError iroh_gossip_broadcast_result(IrohAsyncHandle *handle);

    /** Gossip event types */
    typedef enum
    {
        IROH_GOSSIP_EVENT_RECEIVED = 0,    /* Received a message */
        IROH_GOSSIP_EVENT_NEIGHBORUP = 1,  /* A peer joined */
        IROH_GOSSIP_EVENT_NEIGHBORDOWN = 2,/* A peer left */
        IROH_GOSSIP_EVENT_JOINED = 3,      /* We joined (deprecated, use NeighborUp) */
        IROH_GOSSIP_EVENT_ERROR = 4,       /* Error or unknown */
    } IrohGossipEventType;

    /** Result of receiving a gossip event */
    typedef struct
    {
        IrohGossipEventType event_type;
        uint8_t *data;       /* For Received: message data (free with iroh_bytes_free) */
        size_t data_len;
        IrohPublicKey *peer; /* For NeighborUp/NeighborDown: the peer (free with iroh_public_key_free) */
    } IrohGossipEvent;

    /** Receive the next gossip event (async). */
    IrohAsyncHandle *iroh_gossip_recv(IrohGossipTopic *topic);

    /** Get the gossip event from a completed recv operation. */
    IrohGossipEvent iroh_gossip_recv_result(IrohAsyncHandle *handle);

    /** Free gossip event data. */
    void iroh_gossip_event_free(IrohGossipEvent *event);

    /** Free gossip topic. */
    void iroh_gossip_topic_free(IrohGossipTopic *topic);

    /** Free gossip protocol. */
    void iroh_gossip_free(IrohGossip *gossip);

    /* ========================================================================
     * ROUTER (for multi-protocol support)
     * ======================================================================== */

    typedef struct IrohRouter IrohRouter;

    /**
     * Create gossip protocol with router (RECOMMENDED for gossip to work properly).
     * This sets up the router to accept incoming gossip connections.
     */
    IrohAsyncHandle *iroh_gossip_create_with_router(const IrohEndpoint *endpoint);

    /** Get the gossip instance from a completed create_with_router operation. */
    IrohGossip *iroh_gossip_from_router_result(IrohAsyncHandle *handle);

    /** Get the router from a completed create_with_router operation (call after getting gossip). */
    IrohRouter *iroh_router_from_gossip_result(IrohAsyncHandle *handle);

    /** Create a router for the endpoint (without any protocols). */
    IrohAsyncHandle *iroh_router_create(const IrohEndpoint *endpoint);

    /** Get router from completed creation. */
    IrohRouter *iroh_router_create_result(IrohAsyncHandle *handle);

    /** Shutdown the router gracefully. */
    IrohAsyncHandle *iroh_router_shutdown(IrohRouter *router);

    /** Check if shutdown completed. */
    IrohError iroh_router_shutdown_result(IrohAsyncHandle *handle);

    /** Free router (does not shutdown gracefully). */
    void iroh_router_free(IrohRouter *router);

    /* ========================================================================
     * IROH-BLOBS PROTOCOL
     * ======================================================================== */

    typedef struct IrohBlobHash IrohBlobHash;
    typedef struct IrohBlobStore IrohBlobStore;
    typedef struct IrohBlobsProtocol IrohBlobsProtocol;
    typedef struct IrohBlobTicket IrohBlobTicket;

    /** Create a blob hash from a string (hex or base32). */
    IrohBlobHash *iroh_blob_hash_from_string(const char *s);

    /** Convert a blob hash to string. Free with iroh_string_free(). */
    char *iroh_blob_hash_to_string(const IrohBlobHash *hash);

    /** Get the raw bytes of a blob hash (32 bytes). Free with iroh_bytes_free(). */
    uint8_t *iroh_blob_hash_to_bytes(const IrohBlobHash *hash, size_t *out_len);

    /** Free a blob hash. */
    void iroh_blob_hash_free(IrohBlobHash *hash);

    /**
     * Create blobs protocol with router (required for serving blobs).
     * This sets up an in-memory store and router to accept blob requests.
     * Data is stored in memory and lost when the process exits.
     */
    IrohAsyncHandle *iroh_blobs_create_with_router(const IrohEndpoint *endpoint);

    /**
     * Create blobs protocol with router using persistent filesystem storage.
     * Data is stored on disk at the specified path and persists across restarts.
     * The directory will be created if it does not exist.
     *
     * @param endpoint The endpoint to use for the blobs protocol
     * @param path     Directory path where blob data will be stored
     */
    IrohAsyncHandle *iroh_blobs_create_with_router_persistent(const IrohEndpoint *endpoint, const char *path);

    /** Get the blob store from a completed create_with_router operation. */
    IrohBlobStore *iroh_blobs_store_from_router_result(IrohAsyncHandle *handle);

    /** Get the blobs protocol from a completed create_with_router operation (call after getting store). */
    IrohBlobsProtocol *iroh_blobs_protocol_from_router_result(IrohAsyncHandle *handle);

    /** Get the router from a completed blobs create_with_router operation (call after getting blobs). */
    IrohRouter *iroh_router_from_blobs_result(IrohAsyncHandle *handle);

    /** Add data to the blob store and get its hash. */
    IrohAsyncHandle *iroh_blobs_add_bytes(const IrohBlobStore *store, const uint8_t *data, size_t len);

    /** Get the blob hash from a completed add operation. */
    IrohBlobHash *iroh_blobs_add_result(IrohAsyncHandle *handle);

    /** Add a file to the blob store. */
    IrohAsyncHandle *iroh_blobs_add_file(const IrohBlobStore *store, const char *path);

    /** Create a blob ticket for sharing. */
    IrohBlobTicket *iroh_blobs_create_ticket(const IrohEndpoint *endpoint, const IrohBlobHash *hash);

    /** Parse a blob ticket from string. */
    IrohBlobTicket *iroh_blob_ticket_from_string(const char *s);

    /** Convert a blob ticket to string. Free with iroh_string_free(). */
    char *iroh_blob_ticket_to_string(const IrohBlobTicket *ticket);

    /** Get the hash from a blob ticket. */
    IrohBlobHash *iroh_blob_ticket_hash(const IrohBlobTicket *ticket);

    /** Get the endpoint address from a blob ticket. */
    IrohEndpointAddr *iroh_blob_ticket_addr(const IrohBlobTicket *ticket);

    /** Free a blob ticket. */
    void iroh_blob_ticket_free(IrohBlobTicket *ticket);

    /** Read blob data by hash. */
    IrohAsyncHandle *iroh_blobs_read(const IrohBlobStore *store, const IrohBlobHash *hash);

    /** Get the blob data from a completed read operation. Free with iroh_bytes_free(). */
    uint8_t *iroh_blobs_read_result(IrohAsyncHandle *handle, size_t *out_len);

    /** Download a blob from a remote peer using a ticket. */
    IrohAsyncHandle *iroh_blobs_download(
        const IrohBlobsProtocol *blobs,
        const IrohEndpoint *endpoint,
        const IrohBlobTicket *ticket);

    /** Get the blob hash from a completed download operation. */
    IrohBlobHash *iroh_blobs_download_result(IrohAsyncHandle *handle);

    /** Free blob store. */
    void iroh_blobs_store_free(IrohBlobStore *store);

    /** Free blobs protocol. */
    void iroh_blobs_protocol_free(IrohBlobsProtocol *blobs);

    /** Get the ALPN for blobs protocol. Free with iroh_string_free(). */
    char *iroh_blobs_alpn(void);

    /* ========================================================================
     * IROH-DOCS PROTOCOL
     * ======================================================================== */

    typedef struct IrohDocs IrohDocs;
    typedef struct IrohAuthorId IrohAuthorId;
    typedef struct IrohNamespaceId IrohNamespaceId;
    typedef struct IrohDocTicket IrohDocTicket;

    /**
     * Create docs protocol with all dependencies (blobs, gossip, router).
     * Docs requires both blobs and gossip protocols to function.
     * Uses in-memory storage - data is lost when the process exits.
     */
    IrohAsyncHandle *iroh_docs_create_with_router(const IrohEndpoint *endpoint);

    /**
     * Create docs protocol with persistent filesystem storage.
     * Both document metadata and blob content are stored on disk at the specified path.
     * The directory structure will be created if it does not exist.
     *
     * Directory layout:
     *   <path>/blobs/  - Blob content storage
     *   <path>/docs/   - Document metadata (redb database)
     *
     * @param endpoint The endpoint to use
     * @param path     Base directory path where data will be stored
     */
    IrohAsyncHandle *iroh_docs_create_with_router_persistent(const IrohEndpoint *endpoint, const char *path);

    /** Get the docs protocol from a completed create_with_router operation. */
    IrohDocs *iroh_docs_from_router_result(IrohAsyncHandle *handle);

    /** Get the blob store from a completed docs create_with_router operation (call after getting docs). */
    IrohBlobStore *iroh_blobs_store_from_docs_result(IrohAsyncHandle *handle);

    /** Get the router from a completed docs create_with_router operation (call last). */
    IrohRouter *iroh_router_from_docs_result(IrohAsyncHandle *handle);

    /** Create a new author for signing document entries. */
    IrohAsyncHandle *iroh_docs_create_author(const IrohDocs *docs);

    /** Get the author ID from a completed create_author operation. */
    IrohAuthorId *iroh_docs_create_author_result(IrohAsyncHandle *handle);

    /** Convert an author ID to string. Free with iroh_string_free(). */
    char *iroh_author_id_to_string(const IrohAuthorId *author);

    /** Parse an author ID from string. */
    IrohAuthorId *iroh_author_id_from_string(const char *s);

    /** Free an author ID. */
    void iroh_author_id_free(IrohAuthorId *author);

    /** Convert a namespace ID to string. Free with iroh_string_free(). */
    char *iroh_namespace_id_to_string(const IrohNamespaceId *namespace_id);

    /** Parse a namespace ID from string. */
    IrohNamespaceId *iroh_namespace_id_from_string(const char *s);

    /** Free a namespace ID. */
    void iroh_namespace_id_free(IrohNamespaceId *namespace_id);

    /** Create a new document (namespace). */
    IrohAsyncHandle *iroh_docs_create_doc(const IrohDocs *docs);

    /** Get the namespace ID from a completed create_doc operation. */
    IrohNamespaceId *iroh_docs_create_doc_result(IrohAsyncHandle *handle);

    /** Set an entry in a document (key-value pair). */
    IrohAsyncHandle *iroh_docs_set(
        const IrohDocs *docs,
        const IrohNamespaceId *namespace_id,
        const IrohAuthorId *author,
        const char *key,
        const uint8_t *value,
        size_t value_len);

    /** Check if a set operation completed successfully. */
    IrohError iroh_docs_set_result(IrohAsyncHandle *handle);

    /**
     * Set an entry and wait for peers to have a chance to sync.
     * This writes the entry and then waits the specified time for peers to sync.
     * 
     * The wait allows connected peers to:
     * 1. Receive the gossip announcement about the new entry
     * 2. Sync the entry metadata  
     * 3. Download the blob content
     *
     * @param timeout_secs Time to wait after writing (recommended: 3-5 seconds for LAN, 10+ for WAN)
     *                     If 0, returns immediately after writing (same as iroh_docs_set).
     */
    IrohAsyncHandle *iroh_docs_set_and_sync(
        const IrohDocs *docs,
        const IrohNamespaceId *namespace_id,
        const IrohAuthorId *author,
        const char *key,
        const uint8_t *value,
        size_t value_len,
        uint32_t timeout_secs);

    /** Check if a set_and_sync operation completed successfully. */
    IrohError iroh_docs_set_and_sync_result(IrohAsyncHandle *handle);

    /** Get an entry from a document by key (requires specific author). */
    IrohAsyncHandle *iroh_docs_get(
        const IrohDocs *docs,
        const IrohNamespaceId *namespace_id,
        const IrohAuthorId *author,
        const char *key);

    /** Get the value from a completed get operation. Free with iroh_bytes_free(). */
    uint8_t *iroh_docs_get_result(IrohAsyncHandle *handle, size_t *out_len);

    /** 
     * Get the latest entry for a key from any author.
     * This is the recommended function for key-value store usage where
     * you don't care which author wrote the value.
     */
    IrohAsyncHandle *iroh_docs_get_latest(
        const IrohDocs *docs,
        const IrohNamespaceId *namespace_id,
        const char *key);

    /** Get the value from a completed get_latest operation. Free with iroh_bytes_free(). */
    uint8_t *iroh_docs_get_latest_result(IrohAsyncHandle *handle, size_t *out_len);

    /** Delete an entry from a document. */
    IrohAsyncHandle *iroh_docs_delete(
        const IrohDocs *docs,
        const IrohNamespaceId *namespace_id,
        const IrohAuthorId *author,
        const char *key);

    /** Check if a delete operation completed successfully. */
    IrohError iroh_docs_delete_result(IrohAsyncHandle *handle);

    /** Create a ticket for sharing a document. */
    IrohAsyncHandle *iroh_docs_create_ticket(
        const IrohDocs *docs,
        const IrohNamespaceId *namespace_id,
        const IrohEndpoint *endpoint);

    /** Get the ticket from a completed create_ticket operation. */
    IrohDocTicket *iroh_docs_create_ticket_result(IrohAsyncHandle *handle);

    /** Convert a doc ticket to string. Free with iroh_string_free(). */
    char *iroh_doc_ticket_to_string(const IrohDocTicket *ticket);

    /** Parse a doc ticket from string. */
    IrohDocTicket *iroh_doc_ticket_from_string(const char *s);

    /** Get the namespace ID from a doc ticket. */
    IrohNamespaceId *iroh_doc_ticket_namespace(const IrohDocTicket *ticket);

    /** Free a doc ticket. */
    void iroh_doc_ticket_free(IrohDocTicket *ticket);

    /** Join a document using a ticket (imports and syncs). */
    IrohAsyncHandle *iroh_docs_join(const IrohDocs *docs, const IrohDocTicket *ticket);

    /** Get the namespace ID from a completed join operation. */
    IrohNamespaceId *iroh_docs_join_result(IrohAsyncHandle *handle);

    /** Free docs protocol. */
    void iroh_docs_free(IrohDocs *docs);

    /** Get the ALPN for docs protocol. Free with iroh_string_free(). */
    char *iroh_docs_alpn(void);

    /* ========================================================================
     * LIBEVENT2 INTEGRATION HELPERS
     * ======================================================================== */

    /**
     * Context structure for iroh async operations with libevent.
     */
    typedef struct iroh_event_ctx
    {
        struct event *ev;
        IrohAsyncHandle *handle;
        void (*callback)(IrohAsyncHandle *handle, void *user_data);
        void *user_data;
    } iroh_event_ctx_t;

    /**
     * Internal callback that bridges libevent to the user callback.
     */
    static inline void
    iroh_event_callback(evutil_socket_t fd, short what, void *arg)
    {
        iroh_event_ctx_t *ctx = (iroh_event_ctx_t *)arg;
        (void)fd;
        (void)what;

        IrohAsyncState state = iroh_async_poll(ctx->handle);

        if (state == IROH_ASYNC_PENDING)
        {
            /* Not ready yet, re-add the event */
            event_add(ctx->ev, NULL);
            return;
        }

        /* Operation complete, call user callback */
        if (ctx->callback)
        {
            ctx->callback(ctx->handle, ctx->user_data);
        }

        /* Clean up */
        event_free(ctx->ev);
        free(ctx);
    }

    /**
     * Add an iroh async operation to a libevent event base.
     *
     * @param base      The libevent event_base
     * @param handle    The IrohAsyncHandle from an iroh async operation
     * @param callback  Function to call when the operation completes
     * @param user_data User data passed to the callback
     * @return 0 on success, -1 on error
     */
    static inline int
    iroh_event_add(struct event_base *base, IrohAsyncHandle *handle,
                   void (*callback)(IrohAsyncHandle *handle, void *user_data), void *user_data)
    {
        if (!base || !handle)
            return -1;

        int fd = iroh_async_get_fd(handle);
        if (fd < 0)
            return -1;

        iroh_event_ctx_t *ctx = (iroh_event_ctx_t *)malloc(sizeof(iroh_event_ctx_t));
        if (!ctx)
            return -1;

        ctx->handle = handle;
        ctx->callback = callback;
        ctx->user_data = user_data;
        ctx->ev = event_new(base, fd, EV_READ, iroh_event_callback, ctx);

        if (!ctx->ev)
        {
            free(ctx);
            return -1;
        }

        if (event_add(ctx->ev, NULL) != 0)
        {
            event_free(ctx->ev);
            free(ctx);
            return -1;
        }

        return 0;
    }

    /**
     * Add an iroh async operation with a timeout.
     *
     * @param base      The libevent event_base
     * @param handle    The IrohAsyncHandle from an iroh async operation
     * @param callback  Function to call when the operation completes or times out
     * @param user_data User data passed to the callback
     * @param timeout   Timeout in seconds (use NULL for no timeout)
     * @return 0 on success, -1 on error
     */
    static inline int
    iroh_event_add_timeout(struct event_base *base, IrohAsyncHandle *handle,
                           void (*callback)(IrohAsyncHandle *handle, void *user_data),
                           void *user_data, struct timeval *timeout)
    {
        if (!base || !handle)
            return -1;

        int fd = iroh_async_get_fd(handle);
        if (fd < 0)
            return -1;

        iroh_event_ctx_t *ctx = (iroh_event_ctx_t *)malloc(sizeof(iroh_event_ctx_t));
        if (!ctx)
            return -1;

        ctx->handle = handle;
        ctx->callback = callback;
        ctx->user_data = user_data;
        ctx->ev = event_new(base, fd, EV_READ, iroh_event_callback, ctx);

        if (!ctx->ev)
        {
            free(ctx);
            return -1;
        }

        if (event_add(ctx->ev, timeout) != 0)
        {
            event_free(ctx->ev);
            free(ctx);
            return -1;
        }

        return 0;
    }

/**
 * Macro to add an iroh async operation to libevent.
 *
 * Example:
 *   IROH_EVENT_ADD(base, iroh_endpoint_create(NULL), on_endpoint_created, ctx);
 */
#define IROH_EVENT_ADD(base, async_call, callback, user_data)                                          \
    do                                                                                                 \
    {                                                                                                  \
        IrohAsyncHandle *_h = (async_call);                                                            \
        if (_h)                                                                                        \
            iroh_event_add((base), _h, (callback), (user_data));                                       \
    } while (0)

/**
 * Check async result and handle error/pending states.
 * Use in callbacks to simplify result handling.
 *
 * @param handle    The async handle to check
 * @param base      Event base (for re-adding if pending)
 * @param callback  Callback function (for re-adding if pending)
 * @param user_data User data (for re-adding if pending)
 *
 * Example:
 *   void on_result(IrohAsyncHandle *handle, void *arg) {
 *       IROH_CHECK_READY(handle, base, on_result, arg);
 *       // handle is ready, get result...
 *   }
 */
#define IROH_CHECK_READY(handle, base, callback, user_data)                                            \
    do                                                                                                 \
    {                                                                                                  \
        IrohAsyncState _state = iroh_async_poll(handle);                                               \
        if (_state == IROH_ASYNC_PENDING)                                                              \
        {                                                                                              \
            iroh_event_add((base), (handle), (callback), (user_data));                                 \
            return;                                                                                    \
        }                                                                                              \
        if (_state == IROH_ASYNC_ERROR)                                                                \
        {                                                                                              \
            char *_err = iroh_async_get_error(handle);                                                 \
            fprintf(stderr, "Error: %s\n", _err ? _err : "unknown");                                   \
            if (_err)                                                                                  \
                iroh_string_free(_err);                                                                \
            iroh_async_free(handle);                                                                   \
            return;                                                                                    \
        }                                                                                              \
    } while (0)

/**
 * Check async result with custom error handler.
 *
 * @param handle      The async handle to check
 * @param base        Event base (for re-adding if pending)
 * @param callback    Callback function (for re-adding if pending)
 * @param user_data   User data (for re-adding if pending)
 * @param on_error    Code block to execute on error (has access to 'err_msg')
 */
#define IROH_CHECK_READY_OR(handle, base, callback, user_data, on_error)                               \
    do                                                                                                 \
    {                                                                                                  \
        IrohAsyncState _state = iroh_async_poll(handle);                                               \
        if (_state == IROH_ASYNC_PENDING)                                                              \
        {                                                                                              \
            iroh_event_add((base), (handle), (callback), (user_data));                                 \
            return;                                                                                    \
        }                                                                                              \
        if (_state == IROH_ASYNC_ERROR)                                                                \
        {                                                                                              \
            char *err_msg = iroh_async_get_error(handle);                                              \
            on_error;                                                                                  \
            if (err_msg)                                                                               \
                iroh_string_free(err_msg);                                                             \
            iroh_async_free(handle);                                                                   \
            return;                                                                                    \
        }                                                                                              \
    } while (0)

    /* ========================================================================
     * GOSSIP PROTOCOL HELPERS
     * ======================================================================== */

/**
 * Create a topic ID from a string (hashes the string to create the topic).
 * Useful for creating human-readable topic names.
 *
 * @param topic_name  A human-readable topic name
 * @return Topic ID (must be freed with iroh_topic_id_free)
 */
static inline IrohTopicId *
iroh_topic_from_name(const char *topic_name)
{
    return iroh_topic_id_from_name(topic_name);
}

/**
 * Broadcast a string message to a gossip topic.
 *
 * @param topic   The gossip topic handle
 * @param message The string message to broadcast
 * @return Async handle for the broadcast operation
 */
static inline IrohAsyncHandle *
iroh_gossip_broadcast_string(const IrohGossipTopic *topic, const char *message)
{
    return iroh_gossip_broadcast(topic, (const uint8_t *)message, strlen(message));
}

/**
 * Macro to handle gossip events in a receive loop.
 * Automatically re-subscribes for the next event after processing.
 *
 * @param topic       The gossip topic handle
 * @param base        Event base
 * @param callback    Callback for next event
 * @param user_data   User data for callback
 * @param event_var   Variable name to store the event
 * @param on_received Code block for RECEIVED events (has 'data', 'data_len', 'sender')
 * @param on_neighbor_up   Code block for NEIGHBOR_UP events (has 'sender')
 * @param on_neighbor_down Code block for NEIGHBOR_DOWN events (has 'sender')
 */
#define IROH_GOSSIP_HANDLE_EVENT(topic, base, callback, user_data, event_var, on_received,             \
                                 on_neighbor_up, on_neighbor_down)                                     \
    do                                                                                                 \
    {                                                                                                  \
        IrohGossipEventType _type = iroh_gossip_event_type(event_var);                                 \
        IrohPublicKey *sender = iroh_gossip_event_sender(event_var);                                   \
        if (_type == IROH_GOSSIP_EVENT_RECEIVED)                                                       \
        {                                                                                              \
            size_t data_len = 0;                                                                       \
            uint8_t *data = iroh_gossip_event_data(event_var, &data_len);                              \
            on_received;                                                                               \
            if (data)                                                                                  \
                iroh_bytes_free(data, data_len);                                                       \
        }                                                                                              \
        else if (_type == IROH_GOSSIP_EVENT_NEIGHBOR_UP)                                               \
        {                                                                                              \
            on_neighbor_up;                                                                            \
        }                                                                                              \
        else if (_type == IROH_GOSSIP_EVENT_NEIGHBOR_DOWN)                                             \
        {                                                                                              \
            on_neighbor_down;                                                                          \
        }                                                                                              \
        if (sender)                                                                                    \
            iroh_public_key_free(sender);                                                              \
        iroh_gossip_event_free(event_var);                                                             \
        IROH_EVENT_ADD(base, iroh_gossip_recv(topic), callback, user_data);                            \
    } while (0)

    /* ========================================================================
     * BLOB PROTOCOL HELPERS
     * ======================================================================== */

/**
 * Add a string as a blob.
 *
 * @param store   The blob store
 * @param str     The string to add
 * @return Async handle for the add operation
 */
static inline IrohAsyncHandle *
iroh_blobs_add_string(const IrohBlobStore *store, const char *str)
{
    return iroh_blobs_add_bytes(store, (const uint8_t *)str, strlen(str));
}

/**
 * Macro to create and share a blob in one step.
 * After the async operation completes, creates a ticket for sharing.
 *
 * @param handle    Completed add operation handle
 * @param endpoint  The endpoint for creating the ticket
 * @param hash_var  Variable name to store the hash
 * @param ticket_var Variable name to store the ticket string
 */
#define IROH_BLOB_GET_TICKET(handle, endpoint, hash_var, ticket_var)                                   \
    IrohBlobHash *hash_var = iroh_blobs_add_result(handle);                                            \
    char *ticket_var = NULL;                                                                           \
    if (hash_var)                                                                                      \
    {                                                                                                  \
        IrohBlobTicket *_ticket = iroh_blobs_create_ticket(endpoint, hash_var);                        \
        if (_ticket)                                                                                   \
        {                                                                                              \
            ticket_var = iroh_blob_ticket_to_string(_ticket);                                          \
            iroh_blob_ticket_free(_ticket);                                                            \
        }                                                                                              \
    }

    /* ========================================================================
     * DOCS PROTOCOL HELPERS
     * ======================================================================== */

/**
 * Set a string value in a document.
 *
 * @param docs      The docs protocol handle
 * @param ns        The namespace ID
 * @param author    The author ID
 * @param key       The key string
 * @param value     The value string
 * @return Async handle for the set operation
 */
static inline IrohAsyncHandle *
iroh_docs_set_string(const IrohDocs *docs, const IrohNamespaceId *ns, const IrohAuthorId *author,
                     const char *key, const char *value)
{
    return iroh_docs_set(docs, ns, author, key, (const uint8_t *)value, strlen(value));
}

/**
 * Set a string value in a document and wait for sync confirmation.
 * This ensures the content is replicated to at least one peer before returning.
 *
 * @param docs         The docs protocol handle
 * @param ns           The namespace ID
 * @param author       The author ID
 * @param key          The key string
 * @param value        The value string
 * @param timeout_secs Maximum time to wait for sync (0 = wait forever)
 * @return Async handle for the set operation
 */
static inline IrohAsyncHandle *
iroh_docs_set_string_and_sync(const IrohDocs *docs, const IrohNamespaceId *ns, const IrohAuthorId *author,
                              const char *key, const char *value, uint32_t timeout_secs)
{
    return iroh_docs_set_and_sync(docs, ns, author, key, (const uint8_t *)value, strlen(value), timeout_secs);
}

/**
 * Macro to get a string value from a completed docs_get operation.
 * Allocates a null-terminated string that must be freed with free().
 *
 * @param handle   Completed get operation handle
 * @param str_var  Variable name to store the string (char*)
 */
#define IROH_DOCS_GET_STRING(handle, str_var)                                                          \
    char *str_var = NULL;                                                                              \
    do                                                                                                 \
    {                                                                                                  \
        size_t _len = 0;                                                                               \
        uint8_t *_data = iroh_docs_get_result(handle, &_len);                                          \
        if (_data && _len > 0)                                                                         \
        {                                                                                              \
            str_var = (char *)malloc(_len + 1);                                                        \
            if (str_var)                                                                               \
            {                                                                                          \
                memcpy(str_var, _data, _len);                                                          \
                str_var[_len] = '\0';                                                                  \
            }                                                                                          \
            iroh_bytes_free(_data, _len);                                                              \
        }                                                                                              \
    } while (0)

/**
 * Macro to get a string value from a completed docs_get_latest operation.
 * This is the recommended macro for key-value store usage.
 * Allocates a null-terminated string that must be freed with free().
 *
 * @param handle   Completed get_latest operation handle
 * @param str_var  Variable name to store the string (char*)
 */
#define IROH_DOCS_GET_LATEST_STRING(handle, str_var)                                                   \
    char *str_var = NULL;                                                                              \
    do                                                                                                 \
    {                                                                                                  \
        size_t _len = 0;                                                                               \
        uint8_t *_data = iroh_docs_get_latest_result(handle, &_len);                                   \
        if (_data && _len > 0)                                                                         \
        {                                                                                              \
            str_var = (char *)malloc(_len + 1);                                                        \
            if (str_var)                                                                               \
            {                                                                                          \
                memcpy(str_var, _data, _len);                                                          \
                str_var[_len] = '\0';                                                                  \
            }                                                                                          \
            iroh_bytes_free(_data, _len);                                                              \
        }                                                                                              \
    } while (0)

/**
 * Macro to create a document and get its ticket for sharing.
 * Use after iroh_docs_create_doc completes.
 *
 * @param docs       The docs protocol handle
 * @param endpoint   The endpoint for the ticket
 * @param ns_handle  Completed create_doc async handle
 * @param ns_var     Variable name for namespace ID
 * @param ticket_handle_var Variable name for ticket async handle
 */
#define IROH_DOCS_CREATE_AND_SHARE(docs, endpoint, ns_handle, ns_var, ticket_handle_var)               \
    IrohNamespaceId *ns_var = iroh_docs_create_doc_result(ns_handle);                                  \
    IrohAsyncHandle *ticket_handle_var = NULL;                                                         \
    if (ns_var)                                                                                        \
    {                                                                                                  \
        ticket_handle_var = iroh_docs_create_ticket(docs, ns_var, endpoint);                           \
    }

#ifdef __cplusplus
}
#endif

#endif /* IROH_LIBEVENT_H */
