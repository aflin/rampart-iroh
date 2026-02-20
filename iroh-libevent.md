# iroh-libevent

A C API for [iroh](https://github.com/n0-computer/iroh) with [libevent2](https://libevent.org/) integration.

This library allows you to use iroh's peer-to-peer networking capabilities from C applications that use libevent2 for their event loop.

## Features

- **Full async integration with libevent2**: All iroh operations are non-blocking and integrate seamlessly with libevent2's event loop via file descriptor notifications
- **Complete endpoint management**: Create, configure, and manage iroh endpoints
- **Connection handling**: Connect to peers and accept incoming connections
- **Stream I/O**: Open and accept bidirectional and unidirectional QUIC streams
- **Datagram support**: Send and receive QUIC datagrams
- **Key management**: Generate, parse, and serialize secret/public keys
- **Gossip protocol**: Publish-subscribe messaging over broadcast trees (iroh-gossip)
- **Blob transfer**: Content-addressed file transfer using BLAKE3 hashes (iroh-blobs)
- **Document sync**: Distributed key-value store with automatic synchronization (iroh-docs)

## Requirements

- Rust 1.75+ (for building the library)
- libevent2 development files
- A C compiler (gcc or clang)
- pkg-config (optional, for finding libevent)

### Installing Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get install libevent-dev build-essential pkg-config
```

**macOS (Homebrew):**
```bash
brew install libevent
```

**Fedora/RHEL:**
```bash
sudo dnf install libevent-devel gcc make
```

## Building

```bash
# Build the library and examples
make

# Or build just the library
make lib

# Install to /usr/local (or specify PREFIX)
sudo make install
# or
make install PREFIX=$HOME/.local
```

## Usage

### Basic Pattern

1. Initialize the iroh runtime with `iroh_init()`
2. Start async operations (they return `IrohAsyncHandle*`)
3. Get the file descriptor with `iroh_async_get_fd()`
4. Add the fd to your libevent2 event base with `EV_READ`
5. When the event fires, call `iroh_async_poll()` to check status
6. If ready, call the corresponding `*_result()` function to get the result
7. Free resources when done

### Using the Helper Macros

The header includes helper macros and inline functions for easier libevent integration:

```c
#include "iroh_libevent.h"

void on_endpoint_created(IrohAsyncHandle *handle, void *user_data) {
    struct event_base *base = (struct event_base *)user_data;
    
    // IROH_CHECK_READY handles pending/error states automatically
    IROH_CHECK_READY(handle, base, on_endpoint_created, user_data);
    
    // If we get here, handle is ready
    IrohEndpoint *endpoint = iroh_endpoint_create_result(handle);
    iroh_async_free(handle);
    // Use the endpoint...
}

int main() {
    iroh_init();
    
    struct event_base *base = event_base_new();
    
    // Use the helper macro to add async operations
    IROH_EVENT_ADD(base, iroh_endpoint_create(NULL), on_endpoint_created, base);
    
    event_base_dispatch(base);
    
    event_base_free(base);
    return 0;
}
```

#### Available Helper Macros

**Core Helpers:**

```c
// Add an async operation to libevent
IROH_EVENT_ADD(base, async_call, callback, user_data);

// Check if ready, re-add if pending, print error and return if failed
IROH_CHECK_READY(handle, base, callback, user_data);

// Same as above but with custom error handling
IROH_CHECK_READY_OR(handle, base, callback, user_data, {
    fprintf(stderr, "Custom error: %s\n", err_msg);
    event_base_loopbreak(base);
});
```

**Gossip Helpers:**

```c
// Create topic from a human-readable name
IrohTopicId *topic = iroh_topic_from_name("my-chat-room");

// Broadcast a string message
IrohAsyncHandle *h = iroh_gossip_broadcast_string(topic_handle, "Hello!");

// Handle gossip events with automatic re-subscription
IROH_GOSSIP_HANDLE_EVENT(topic, base, on_event, user_data, event,
    { /* on_received: has 'data', 'data_len', 'sender' */
        printf("Message: %.*s\n", (int)data_len, data);
    },
    { /* on_neighbor_up: has 'sender' */
        printf("Peer joined\n");
    },
    { /* on_neighbor_down: has 'sender' */
        printf("Peer left\n");
    }
);
```

**Blob Helpers:**

```c
// Add a string as a blob
IrohAsyncHandle *h = iroh_blobs_add_string(store, "Hello, World!");

// Get hash and ticket after adding a blob
IROH_BLOB_GET_TICKET(handle, endpoint, hash, ticket_str);
if (ticket_str) {
    printf("Share this ticket: %s\n", ticket_str);
    iroh_string_free(ticket_str);
}
```

**Docs Helpers:**

```c
// Set a string value
IrohAsyncHandle *h = iroh_docs_set_string(docs, ns, author, "key", "value");

// Get a string value (allocates null-terminated string)
IROH_DOCS_GET_STRING(handle, value_str);
if (value_str) {
    printf("Value: %s\n", value_str);
    free(value_str);
}

// Create document and start ticket creation
IROH_DOCS_CREATE_AND_SHARE(docs, endpoint, ns_handle, namespace_id, ticket_handle);
if (ticket_handle) {
    IROH_EVENT_ADD(base, ticket_handle, on_ticket_created, user_data);
}
```

### Manual Integration

If you need more control, you can integrate manually:

```c
#include <event2/event.h>
#include "iroh_libevent.h"

typedef struct {
    struct event *ev;
    IrohAsyncHandle *handle;
} Context;

void on_event(evutil_socket_t fd, short what, void *arg) {
    Context *ctx = (Context *)arg;
    
    IrohAsyncState state = iroh_async_poll(ctx->handle);
    
    switch (state) {
        case IROH_ASYNC_STATE_PENDING:
            // Not ready yet, re-add event
            event_add(ctx->ev, NULL);
            break;
        case IROH_ASYNC_STATE_READY:
            // Get result and handle it
            IrohEndpoint *ep = iroh_endpoint_create_result(ctx->handle);
            // ...
            break;
        case IROH_ASYNC_STATE_ERROR:
            char *err = iroh_async_get_error(ctx->handle);
            fprintf(stderr, "Error: %s\n", err);
            iroh_string_free(err);
            break;
    }
    
    iroh_async_free(ctx->handle);
    event_free(ctx->ev);
    free(ctx);
}

int main() {
    iroh_init();
    
    struct event_base *base = event_base_new();
    
    IrohAsyncHandle *handle = iroh_endpoint_create(NULL);
    int fd = iroh_async_get_fd(handle);
    
    Context *ctx = malloc(sizeof(Context));
    ctx->handle = handle;
    ctx->ev = event_new(base, fd, EV_READ, on_event, ctx);
    event_add(ctx->ev, NULL);
    
    event_base_dispatch(base);
    
    event_base_free(base);
    return 0;
}
```

## API Reference

### Initialization

```c
// Initialize the iroh runtime (must be called first)
IrohError iroh_init(void);

// Initialize with a specific number of worker threads
IrohError iroh_init_with_threads(int num_threads);
```

### Error Handling

```c
// Get the last error message (must free with iroh_string_free)
char *iroh_last_error(void);

// Free a string returned by iroh functions
void iroh_string_free(char *s);
```

### Async Handle Operations

```c
// Get the file descriptor for libevent integration
int iroh_async_get_fd(const IrohAsyncHandle *handle);

// Poll the async operation state
IrohAsyncState iroh_async_poll(const IrohAsyncHandle *handle);

// Get error message from failed operation
char *iroh_async_get_error(const IrohAsyncHandle *handle);

// Cancel an async operation
void iroh_async_cancel(IrohAsyncHandle *handle);

// Free an async handle
void iroh_async_free(IrohAsyncHandle *handle);
```

### Key Management

```c
// Generate a new secret key
IrohSecretKey *iroh_secret_key_generate(void);

// Parse from/to string
IrohSecretKey *iroh_secret_key_from_string(const char *s);
char *iroh_secret_key_to_string(const IrohSecretKey *key);

// Get public key from secret key
IrohPublicKey *iroh_secret_key_public(const IrohSecretKey *key);

// Public key operations
IrohPublicKey *iroh_public_key_from_string(const char *s);
char *iroh_public_key_to_string(const IrohPublicKey *key);

// Free keys
void iroh_secret_key_free(IrohSecretKey *key);
void iroh_public_key_free(IrohPublicKey *key);
```

### Endpoint Configuration

```c
// Create default configuration (returns opaque pointer)
IrohEndpointConfig *iroh_endpoint_config_default(void);

// Free configuration (only if not passed to iroh_endpoint_create)
void iroh_endpoint_config_free(IrohEndpointConfig *config);

// --- Identity ---
void iroh_endpoint_config_secret_key(IrohEndpointConfig *config, const IrohSecretKey *key);
void iroh_endpoint_config_secret_key_bytes(IrohEndpointConfig *config, const uint8_t *key_bytes, size_t len);

// --- ALPNs ---
void iroh_endpoint_config_add_alpn(IrohEndpointConfig *config, const char *alpn);
void iroh_endpoint_config_set_alpns(IrohEndpointConfig *config, const char *const *alpns, size_t count);
void iroh_endpoint_config_clear_alpns(IrohEndpointConfig *config);

// --- Discovery ---
void iroh_endpoint_config_discovery_n0(IrohEndpointConfig *config, bool enabled);      // Default: true
void iroh_endpoint_config_discovery_dns(IrohEndpointConfig *config, bool enabled);     // Default: true
void iroh_endpoint_config_discovery_pkarr_publish(IrohEndpointConfig *config, bool enabled);  // Default: true

// --- Relay ---
typedef enum { 
    IROH_RELAY_MODE_DISABLED = 0,
    IROH_RELAY_MODE_DEFAULT = 1,
    IROH_RELAY_MODE_CUSTOM = 2
} IrohRelayMode;

void iroh_endpoint_config_relay_mode(IrohEndpointConfig *config, IrohRelayMode mode);
void iroh_endpoint_config_relay_url(IrohEndpointConfig *config, const char *url);  // Also sets mode to CUSTOM

// --- Network ---
void iroh_endpoint_config_bind_port(IrohEndpointConfig *config, uint16_t port);
void iroh_endpoint_config_bind_addr_v4(IrohEndpointConfig *config, const char *addr);
void iroh_endpoint_config_bind_addr_v6(IrohEndpointConfig *config, const char *addr);

// --- Transport ---
void iroh_endpoint_config_max_idle_timeout(IrohEndpointConfig *config, uint64_t timeout_ms);
void iroh_endpoint_config_keep_alive_interval(IrohEndpointConfig *config, uint64_t interval_ms);
```

**Example: Custom endpoint configuration**

```c
// Create and configure
IrohEndpointConfig *config = iroh_endpoint_config_default();

// Set a specific secret key (optional - generates one if not set)
IrohSecretKey *key = iroh_secret_key_generate();
iroh_endpoint_config_secret_key(config, key);
iroh_secret_key_free(key);

// Add ALPN protocols
iroh_endpoint_config_add_alpn(config, "my-app/v1");

// Configure relay
iroh_endpoint_config_relay_url(config, "https://my-relay.example.com");

// Configure network
iroh_endpoint_config_bind_port(config, 12345);

// Configure transport
iroh_endpoint_config_max_idle_timeout(config, 30000);     // 30 seconds
iroh_endpoint_config_keep_alive_interval(config, 5000);   // 5 seconds

// Create endpoint - config is consumed, do not free!
IrohAsyncHandle *handle = iroh_endpoint_create(config);
```

### Endpoint Operations

```c
// Create endpoint (async) - config is consumed if provided, pass NULL for defaults
IrohAsyncHandle *iroh_endpoint_create(IrohEndpointConfig *config);
IrohEndpoint *iroh_endpoint_create_result(IrohAsyncHandle *handle);

// Get endpoint info
IrohPublicKey *iroh_endpoint_id(const IrohEndpoint *endpoint);
IrohEndpointAddr *iroh_endpoint_addr(const IrohEndpoint *endpoint);

// Wait for online status (async)
IrohAsyncHandle *iroh_endpoint_wait_online(const IrohEndpoint *endpoint);

// Close endpoint (async)
IrohAsyncHandle *iroh_endpoint_close(IrohEndpoint *endpoint);

// Free endpoint
void iroh_endpoint_free(IrohEndpoint *endpoint);
```

### Connection Operations

```c
// Connect to peer (async)
IrohAsyncHandle *iroh_endpoint_connect(
    const IrohEndpoint *endpoint,
    const IrohEndpointAddr *addr,
    const char *alpn
);
IrohConnection *iroh_connection_connect_result(IrohAsyncHandle *handle);

// Accept connection (async)
IrohAsyncHandle *iroh_endpoint_accept(const IrohEndpoint *endpoint);
IrohConnection *iroh_connection_accept_result(IrohAsyncHandle *handle);

// Connection info
IrohPublicKey *iroh_connection_remote_id(const IrohConnection *conn);
char *iroh_connection_alpn(const IrohConnection *conn);

// Close connection
void iroh_connection_close(IrohConnection *conn, uint32_t code, const char *reason);
void iroh_connection_free(IrohConnection *conn);
```

### Stream Operations

```c
// Open bidirectional stream (async)
IrohAsyncHandle *iroh_connection_open_bi(const IrohConnection *conn);
IrohSendStream *iroh_stream_open_bi_send_result(IrohAsyncHandle *handle);
IrohRecvStream *iroh_stream_open_bi_recv_result(IrohAsyncHandle *handle);

// Accept bidirectional stream (async)
IrohAsyncHandle *iroh_connection_accept_bi(const IrohConnection *conn);

// Open/accept unidirectional streams (async)
IrohAsyncHandle *iroh_connection_open_uni(const IrohConnection *conn);
IrohAsyncHandle *iroh_connection_accept_uni(const IrohConnection *conn);

// Write to stream (async)
IrohAsyncHandle *iroh_send_stream_write(
    IrohSendStream *stream,
    const uint8_t *data,
    size_t len
);
ssize_t iroh_send_stream_write_result(IrohAsyncHandle *handle);

// Finish send stream
IrohError iroh_send_stream_finish(IrohSendStream *stream);

// Read from stream (async)
IrohAsyncHandle *iroh_recv_stream_read(IrohRecvStream *stream, size_t max_len);
IrohAsyncHandle *iroh_recv_stream_read_to_end(IrohRecvStream *stream, size_t max_len);
IrohReadResult iroh_recv_stream_read_result(IrohAsyncHandle *handle);

// Free streams
void iroh_send_stream_free(IrohSendStream *stream);
void iroh_recv_stream_free(IrohRecvStream *stream);

// Free read data
void iroh_bytes_free(uint8_t *data, size_t len);
```

### Datagram Operations

```c
// Send datagram (synchronous)
IrohError iroh_connection_send_datagram(
    const IrohConnection *conn,
    const uint8_t *data,
    size_t len
);

// Receive datagram (async)
IrohAsyncHandle *iroh_connection_recv_datagram(const IrohConnection *conn);
uint8_t *iroh_connection_recv_datagram_result(
    IrohAsyncHandle *handle,
    size_t *out_len
);
```

### Gossip Protocol (iroh-gossip)

The gossip protocol provides publish-subscribe messaging over efficient broadcast trees. Peers can join topics and exchange messages with all other peers subscribed to the same topic.

#### Gossip API

```c
// Create gossip with router (required for accepting incoming connections)
IrohAsyncHandle *iroh_gossip_create_with_router(const IrohEndpoint *endpoint);
IrohGossip *iroh_gossip_from_router_result(IrohAsyncHandle *handle);
IrohRouter *iroh_router_from_gossip_result(IrohAsyncHandle *handle);

// Topic management
IrohTopicId *iroh_topic_id_from_bytes(const uint8_t *bytes, size_t len);
IrohTopicId *iroh_topic_id_from_string(const char *s);
char *iroh_topic_id_to_string(const IrohTopicId *topic);
void iroh_topic_id_free(IrohTopicId *topic);

// Subscribe to a topic (async)
IrohAsyncHandle *iroh_gossip_subscribe(
    const IrohGossip *gossip,
    const IrohTopicId *topic,
    const IrohPublicKey **peers,  // Array of bootstrap peers
    size_t num_peers
);
IrohGossipTopic *iroh_gossip_subscribe_result(IrohAsyncHandle *handle);

// Broadcast a message to all peers on the topic (async)
IrohAsyncHandle *iroh_gossip_broadcast(
    const IrohGossipTopic *topic,
    const uint8_t *data,
    size_t len
);
IrohError iroh_gossip_broadcast_result(IrohAsyncHandle *handle);

// Receive messages from the topic (async)
IrohAsyncHandle *iroh_gossip_recv(const IrohGossipTopic *topic);
IrohGossipEvent *iroh_gossip_recv_result(IrohAsyncHandle *handle);

// Gossip event handling
IrohGossipEventType iroh_gossip_event_type(const IrohGossipEvent *event);
uint8_t *iroh_gossip_event_data(const IrohGossipEvent *event, size_t *out_len);
IrohPublicKey *iroh_gossip_event_sender(const IrohGossipEvent *event);
void iroh_gossip_event_free(IrohGossipEvent *event);

// Cleanup
void iroh_gossip_topic_free(IrohGossipTopic *topic);
void iroh_gossip_free(IrohGossip *gossip);
```

#### Gossip Event Types

```c
typedef enum {
    IROH_GOSSIP_EVENT_RECEIVED,    // Message received from a peer
    IROH_GOSSIP_EVENT_NEIGHBOR_UP, // A new peer joined the topic
    IROH_GOSSIP_EVENT_NEIGHBOR_DOWN, // A peer left the topic
    IROH_GOSSIP_EVENT_LAGGED,      // Fell behind, some messages missed
} IrohGossipEventType;
```

#### Gossip Example

See `examples/gossip_chat.c` for a complete chat application. Quick usage:

```bash
# Terminal 1: Create a new topic
./gossip_chat create
# Output: Topic ID: <topic_id>
#         Peer ID: <peer_id>

# Terminal 2: Join the topic
./gossip_chat join <topic_id> <peer_id>

# Now both terminals can exchange messages!
```

### Blob Transfer (iroh-blobs)

The blob protocol provides content-addressed file transfer using BLAKE3 hashes. Files are identified by their hash and can be fetched from any peer that has them.

#### Storage Options

By default, blobs are stored in memory and lost when the process exits. For persistent storage, use the `_persistent` variants:

```c
// In-memory storage (default) - data lost on exit
IrohAsyncHandle *iroh_blobs_create_with_router(const IrohEndpoint *endpoint);

// Persistent filesystem storage - data survives restart
IrohAsyncHandle *iroh_blobs_create_with_router_persistent(
    const IrohEndpoint *endpoint,
    const char *path  // Directory for blob storage
);
```

#### Blob API

```c
// Create blobs protocol with router (required for serving blobs)
// Use iroh_blobs_create_with_router() for memory storage
// Use iroh_blobs_create_with_router_persistent() for disk storage
IrohBlobStore *iroh_blobs_store_from_router_result(IrohAsyncHandle *handle);
IrohBlobsProtocol *iroh_blobs_protocol_from_router_result(IrohAsyncHandle *handle);
IrohRouter *iroh_router_from_blobs_result(IrohAsyncHandle *handle);

// Hash management
IrohBlobHash *iroh_blob_hash_from_string(const char *s);
char *iroh_blob_hash_to_string(const IrohBlobHash *hash);
uint8_t *iroh_blob_hash_to_bytes(const IrohBlobHash *hash, size_t *out_len);
void iroh_blob_hash_free(IrohBlobHash *hash);

// Add data to the blob store (async)
IrohAsyncHandle *iroh_blobs_add_bytes(
    const IrohBlobStore *store,
    const uint8_t *data,
    size_t len
);
IrohAsyncHandle *iroh_blobs_add_file(
    const IrohBlobStore *store,
    const char *path
);
IrohBlobHash *iroh_blobs_add_result(IrohAsyncHandle *handle);

// Create a ticket for sharing
IrohBlobTicket *iroh_blobs_create_ticket(
    const IrohEndpoint *endpoint,
    const IrohBlobHash *hash
);
char *iroh_blob_ticket_to_string(const IrohBlobTicket *ticket);
IrohBlobTicket *iroh_blob_ticket_from_string(const char *s);
IrohBlobHash *iroh_blob_ticket_hash(const IrohBlobTicket *ticket);
void iroh_blob_ticket_free(IrohBlobTicket *ticket);

// Download a blob from a remote peer (async)
IrohAsyncHandle *iroh_blobs_download(
    const IrohBlobsProtocol *blobs,
    const IrohEndpoint *endpoint,
    const IrohBlobTicket *ticket
);
IrohBlobHash *iroh_blobs_download_result(IrohAsyncHandle *handle);

// Read blob data (async)
IrohAsyncHandle *iroh_blobs_read(
    const IrohBlobStore *store,
    const IrohBlobHash *hash
);
uint8_t *iroh_blobs_read_result(IrohAsyncHandle *handle, size_t *out_len);

// Cleanup
void iroh_blobs_store_free(IrohBlobStore *store);
void iroh_blobs_protocol_free(IrohBlobsProtocol *blobs);
```

#### Blob Example

See `examples/blob_transfer.c` for a complete file transfer application. Quick usage:

```bash
# Terminal 1: Share a file
./blob_transfer send myfile.txt
# Output: Share this ticket to download the file:
#         blob....<long_ticket_string>

# Terminal 2: Download the file
./blob_transfer receive <ticket_string>
# Output: Downloaded blob: <hash>
#         Received 1234 bytes
#         Content: <file contents>
```

### Document Sync (iroh-docs)

The docs protocol provides a distributed key-value store with automatic synchronization between peers. Documents (namespaces) contain entries identified by key, author, and namespace. All entries are signed and can be synced efficiently using range-based set reconciliation.

#### Storage Options

By default, documents are stored in memory and lost when the process exits. For persistent storage, use the `_persistent` variant:

```c
// In-memory storage (default) - data lost on exit
IrohAsyncHandle *iroh_docs_create_with_router(const IrohEndpoint *endpoint);

// Persistent filesystem storage - data survives restart
IrohAsyncHandle *iroh_docs_create_with_router_persistent(
    const IrohEndpoint *endpoint,
    const char *path  // Base directory for storage
);
```

The persistent storage creates the following directory structure:
```
<path>/
├── blobs/     # Blob content (the actual values)
└── docs/      # Document metadata (redb database)
```

#### Docs API

```c
// Create docs protocol with all dependencies (blobs, gossip, router)
// Docs is a "meta protocol" that requires both blobs and gossip
// Use iroh_docs_create_with_router() for memory storage
// Use iroh_docs_create_with_router_persistent() for disk storage
IrohDocs *iroh_docs_from_router_result(IrohAsyncHandle *handle);
IrohBlobStore *iroh_blobs_store_from_docs_result(IrohAsyncHandle *handle);
IrohRouter *iroh_router_from_docs_result(IrohAsyncHandle *handle);

// Author management (authors sign entries)
IrohAsyncHandle *iroh_docs_create_author(const IrohDocs *docs);
IrohAuthorId *iroh_docs_create_author_result(IrohAsyncHandle *handle);
char *iroh_author_id_to_string(const IrohAuthorId *author);
IrohAuthorId *iroh_author_id_from_string(const char *s);
void iroh_author_id_free(IrohAuthorId *author);

// Namespace (document) management
char *iroh_namespace_id_to_string(const IrohNamespaceId *namespace_id);
IrohNamespaceId *iroh_namespace_id_from_string(const char *s);
void iroh_namespace_id_free(IrohNamespaceId *namespace_id);

// Create a new document (async)
IrohAsyncHandle *iroh_docs_create_doc(const IrohDocs *docs);
IrohNamespaceId *iroh_docs_create_doc_result(IrohAsyncHandle *handle);

// Set an entry (key-value pair) in a document (async)
IrohAsyncHandle *iroh_docs_set(
    const IrohDocs *docs,
    const IrohNamespaceId *namespace_id,
    const IrohAuthorId *author,
    const char *key,
    const uint8_t *value,
    size_t value_len
);
IrohError iroh_docs_set_result(IrohAsyncHandle *handle);

// Get an entry from a document (async)
IrohAsyncHandle *iroh_docs_get(
    const IrohDocs *docs,
    const IrohNamespaceId *namespace_id,
    const IrohAuthorId *author,
    const char *key
);
uint8_t *iroh_docs_get_result(IrohAsyncHandle *handle, size_t *out_len);

// Delete an entry from a document (async)
IrohAsyncHandle *iroh_docs_delete(
    const IrohDocs *docs,
    const IrohNamespaceId *namespace_id,
    const IrohAuthorId *author,
    const char *key
);
IrohError iroh_docs_delete_result(IrohAsyncHandle *handle);

// Create a ticket for sharing a document (async)
IrohAsyncHandle *iroh_docs_create_ticket(
    const IrohDocs *docs,
    const IrohNamespaceId *namespace_id,
    const IrohEndpoint *endpoint
);
IrohDocTicket *iroh_docs_create_ticket_result(IrohAsyncHandle *handle);

// Ticket management
char *iroh_doc_ticket_to_string(const IrohDocTicket *ticket);
IrohDocTicket *iroh_doc_ticket_from_string(const char *s);
IrohNamespaceId *iroh_doc_ticket_namespace(const IrohDocTicket *ticket);
void iroh_doc_ticket_free(IrohDocTicket *ticket);

// Join a document using a ticket (async)
IrohAsyncHandle *iroh_docs_join(const IrohDocs *docs, const IrohDocTicket *ticket);
IrohNamespaceId *iroh_docs_join_result(IrohAsyncHandle *handle);

// Cleanup
void iroh_docs_free(IrohDocs *docs);
```

#### Docs Example

See `examples/docs_kv.c` for a complete synchronized key-value store. Quick usage:

```bash
# Terminal 1: Create a new document
./docs_kv create
# Output: Document created! Use this ticket to join:
#         doc....<long_ticket_string>

# Terminal 2: Join and set a value
./docs_kv set <ticket_string> mykey "Hello, World!"
# Output: Successfully set 'mykey' = 'Hello, World!'

# Terminal 3: Get the value (after sync)
./docs_kv get <ticket_string> mykey
# Output: 'mykey' = 'Hello, World!'
```

## Examples

See the `examples/` directory for complete working examples:

- `echo_server.c` - A server that echoes back received data
- `echo_client.c` - A client that connects and sends a message
- `gossip_chat.c` - A peer-to-peer chat application using gossip
- `blob_transfer.c` - File transfer using content-addressed blobs
- `docs_kv.c` - Distributed key-value store with automatic sync

Run the examples:

```bash
# Echo example
make run-server                    # Terminal 1
make run-client ADDR="<addr>"      # Terminal 2

# Gossip chat example
make run-gossip-create             # Terminal 1
make run-gossip-join TOPIC="<topic>" PEER="<peer>"  # Terminal 2

# Blob transfer example
make run-blob-send FILE="myfile.txt"      # Terminal 1
make run-blob-receive TICKET="<ticket>"   # Terminal 2

# Docs key-value example
make run-docs-create                                      # Terminal 1
make run-docs-set TICKET="<ticket>" KEY="foo" VALUE="bar" # Terminal 2
make run-docs-get TICKET="<ticket>" KEY="foo"             # Terminal 3
```

## Thread Safety

- The iroh runtime is thread-safe and can handle operations from multiple threads
- Individual handles (endpoints, connections, streams) should generally be used from a single thread
- The async handle mechanism uses pipe-based notification which is thread-safe

## Memory Management

- All `*_free()` functions are safe to call with NULL pointers
- Strings returned by the API (via `*_to_string()` functions) must be freed with `iroh_string_free()`
- Byte buffers from read operations must be freed with `iroh_bytes_free()`
- Always free async handles after getting their results

## License

This project is licensed under either of:

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT license ([LICENSE-MIT](LICENSE-MIT))

at your option.

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.
