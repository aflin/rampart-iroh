# rampart-iroh

Peer-to-peer networking for Rampart using [iroh](https://iroh.computer).

The `rampart-iroh` module provides QUIC-based encrypted connections,
pub/sub messaging (Gossip), content-addressed blob transfer (Blobs),
and replicated key-value documents (Docs) -- all over iroh's peer-to-peer
network with NAT traversal via relay servers.

## Loading the Module

```javascript
var iroh = require("rampart-iroh");
```

## Endpoint

An Endpoint is the foundation of all iroh networking. It creates a QUIC
endpoint with a unique cryptographic identity (node ID), connects to the
relay network for NAT traversal, and manages connections to other nodes.

### Constructor

```javascript
var ep = new iroh.Endpoint([options]);
```

**Options** (all optional):

| Property    | Type                    | Description |
|-------------|-------------------------|-------------|
| `alpn`      | String or Array         | Application protocol identifier(s) for accepting connections. |
| `secretKey` | String or Buffer        | Secret key for a persistent identity. Pass a hex string (64 chars) or a 32-byte binary buffer. If omitted, a random identity is generated each time. |
| `discovery` | Boolean                 | Enable DNS and n0 discovery services (default: true). |
| `relay`     | String or false         | Custom relay server URL, or `false` to disable relaying. |
| `port`      | Number                  | UDP port to bind (default: random). |
| `timeout`   | Number                  | Max idle timeout in milliseconds for connections. |

### Properties

| Property       | Type    | Description |
|----------------|---------|-------------|
| `nodeId`       | String  | The endpoint's public key (hex). Set after `"online"` fires. |
| `address`      | String  | Full address string: `<nodeId>?relay=<url>`. Set after `"online"` fires. |
| `secretKey`    | Buffer  | The endpoint's 32-byte secret key (binary). Set after `"online"` fires. |
| `secretKeyHex` | String  | The endpoint's secret key as a 64-character lowercase hex string. Set after `"online"` fires. |
| `online`       | Boolean | True once the endpoint is connected to a relay server. |
| `closed`       | Boolean | True after `close()` completes. |

### Methods

#### ep.connect(address, alpn, callback)

Connect to a remote endpoint.

- **address** (String) -- The remote endpoint's address (node ID with optional
  `?relay=<url>` suffix).
- **alpn** (String) -- The application protocol to use.
- **callback** (Function) -- Called with a `Connection` object on success.

```javascript
ep.connect(remoteAddress, "my-app/1", function(conn) {
    console.log("Connected to:", conn.remoteId);
});
```

#### ep.close()

Gracefully shut down the endpoint and all its connections.

```javascript
ep.close();
```

### Events

| Event          | Callback Arguments | Description |
|----------------|--------------------|-------------|
| `"online"`     | *(none)*           | Endpoint has connected to a relay and is ready. `this.nodeId`, `this.address`, `this.secretKey`, and `this.secretKeyHex` are available. |
| `"connection"` | `conn`             | A new incoming connection was accepted. Registering this event starts the accept loop. |
| `"close"`      | *(none)*           | Endpoint has been closed. |
| `"error"`      | `errorMessage`     | An error occurred. |

```javascript
ep.on("online", function() {
    console.log("Node ID:", this.nodeId);
    console.log("Address:", this.address);
});

ep.on("connection", function(conn) {
    console.log("Incoming connection from:", conn.remoteId);
});
```

### Persistent Identity

By default, each Endpoint gets a new random identity. To keep the same
node ID across restarts, save the secret key and pass it back on the next
run:

```javascript
rampart.globalize(rampart.utils);
var iroh = require("rampart-iroh");

var keyFile = process.scriptPath + "/my-node.key";
var opts = {};

// Load saved key if it exists
try {
    var hex = trim(readFile(keyFile));
    if (hex.length === 64) opts.secretKey = hex;
} catch(e) {}

var ep = new iroh.Endpoint(opts);

ep.on("online", function() {
    // Save key on first run
    fprintf(keyFile, "%s", this.secretKeyHex);
    console.log("Node ID:", this.nodeId);  // same every time
});
```

Both `secretKey` (binary buffer) and `secretKeyHex` (hex string) are set
on the endpoint when `"online"` fires. Either form can be passed back via
the `secretKey` constructor option -- strings are treated as hex, buffers
as raw 32-byte keys.

## Connection

A Connection represents an encrypted QUIC connection to a remote endpoint.
Connections are obtained either from the `"connection"` event on an Endpoint
(server side) or from `endpoint.connect()` (client side).

### Properties

| Property   | Type    | Description |
|------------|---------|-------------|
| `remoteId` | String  | The remote endpoint's public key (hex). |
| `closed`   | Boolean | True after the connection is closed. |

### Methods

#### conn.openBi(callback)

Open a bidirectional QUIC stream.

- **callback** (Function) -- Called with a `Stream` object.

```javascript
conn.openBi(function(stream) {
    stream.write("hello");
    stream.finish();
});
```

#### conn.close([code, reason])

Close the connection.

- **code** (Number, optional) -- Application error code (default: 0).
- **reason** (String, optional) -- Reason string (default: `"closed"`).

#### conn.sendDatagram(data)

Send an unreliable datagram over the connection.

- **data** (String or Buffer) -- The data to send.

### Events

| Event         | Callback Arguments | Description |
|---------------|--------------------|-------------|
| `"stream"`    | `stream`           | A new incoming stream was accepted. Registering this event starts the stream accept loop. |
| `"close"`     | *(none)*           | Connection was closed. |
| `"error"`     | `errorMessage`     | An error occurred. |

## Stream

A Stream is a bidirectional QUIC stream within a connection. Streams are
obtained either from the `"stream"` event on a Connection (for incoming
streams) or from `connection.openBi()` (for outgoing streams).

### Properties

| Property   | Type    | Description |
|------------|---------|-------------|
| `finished` | Boolean | True after `finish()` is called. |
| `closed`   | Boolean | True after the stream is fully closed. |

### Methods

#### stream.write(data)

Write data to the stream.

- **data** (String or Buffer) -- The data to write.

Returns `this` for chaining. It is safe to call `finish()` immediately after
`write()` in the same tick -- the finish is automatically deferred until
the write completes.

#### stream.finish()

Signal that no more data will be written. The remote side will receive an
`"end"` event.

#### stream.destroy()

Immediately tear down the stream without a graceful shutdown.

### Events

| Event     | Callback Arguments | Description |
|-----------|--------------------|-------------|
| `"data"`  | `data`             | Data received (String). Registering this event starts the read loop. |
| `"end"`   | *(none)*           | The remote side has finished writing. |
| `"close"` | *(none)*           | Stream is fully closed. |
| `"error"` | `errorMessage`     | A stream error occurred. |

### Echo Server Example

```javascript
var iroh = require("rampart-iroh");

var server = new iroh.Endpoint({ alpn: "echo/1" });

server.on("online", function() {
    console.log("Echo server online at:", this.address);
});

server.on("connection", function(conn) {
    conn.on("stream", function(stream) {
        stream.on("data", function(data) {
            stream.write(data);  // echo back
        });
        stream.on("end", function() {
            stream.finish();
        });
    });
});
```

### Echo Client Example

```javascript
var iroh = require("rampart-iroh");

var client = new iroh.Endpoint();

client.on("online", function() {
    this.connect(serverAddress, "echo/1", function(conn) {
        conn.openBi(function(stream) {
            stream.on("data", function(data) {
                console.log("Echo:", data);
            });
            stream.write("Hello!");
            stream.finish();
        });
    });
});
```

## Convenience Functions

### iroh.createServer([options,] callback)

Create an Endpoint and automatically register the callback for incoming
connections.

- **options** (Object, optional) -- Endpoint options (see Endpoint constructor).
- **callback** (Function) -- Called for each incoming connection with a
  `Connection` argument.

Returns the Endpoint.

```javascript
var server = iroh.createServer({ alpn: "echo/1" }, function(conn) {
    conn.on("stream", function(stream) {
        stream.on("data", function(data) { stream.write(data); });
        stream.on("end", function() { stream.finish(); });
    });
});
```

### iroh.connect(address, alpn, callback)

Create an ephemeral Endpoint and connect to a remote address. The callback
fires once the connection is established.

- **address** (String) -- Remote endpoint address.
- **alpn** (String) -- Application protocol.
- **callback** (Function) -- Called with the `Connection` object.

Returns the Endpoint.

```javascript
iroh.connect(serverAddress, "echo/1", function(conn) {
    conn.openBi(function(stream) {
        stream.write("hello");
        stream.finish();
    });
});
```

## Gossip

Gossip provides pub/sub messaging over iroh's gossip protocol. Messages
are broadcast to all peers subscribed to the same topic.

### Constructor

```javascript
var gossip = new iroh.Gossip(endpoint);
```

- **endpoint** (Endpoint) -- A ready Endpoint (must be online).

### Events

| Event     | Callback Arguments | Description |
|-----------|--------------------|-------------|
| `"ready"` | *(none)*           | Gossip protocol is initialized and ready. |
| `"error"` | `errorMessage`     | An error occurred. |

### Methods

#### gossip.subscribe(topicName [, peers])

Subscribe to a named topic. Returns a `GossipTopic` object.

- **topicName** (String) -- The topic name (hashed internally to a topic ID).
- **peers** (Array of Strings, optional) -- Node IDs of peers already on
  the topic. The first node omits this (or passes `[]`) to create the
  topic. Subsequent nodes must include at least one existing peer's node
  ID to bootstrap into the topic.

```javascript
gossip.on("ready", function() {
    // First node: create the topic (no peers needed)
    var topic = gossip.subscribe("my-channel");

    // Later nodes: join by providing an existing peer's node ID
    // var topic = gossip.subscribe("my-channel", [firstNodeId]);
});
```

#### gossip.close()

Shut down the gossip protocol.

## GossipTopic

A GossipTopic represents a subscription to a gossip topic. It is returned
by `gossip.subscribe()`.

### Methods

#### topic.broadcast(data)

Broadcast a message to all peers on this topic.

- **data** (String or Buffer) -- The message to send.

```javascript
topic.broadcast("Hello everyone!");
```

#### topic.close()

Unsubscribe from the topic.

### Events

| Event          | Callback Arguments | Description |
|----------------|--------------------|-------------|
| `"ready"`      | *(none)*           | Subscription is active. |
| `"message"`    | `msg`              | A message was received. `msg.data` is the message content (String), `msg.sender` is the sender's node ID. |
| `"peerJoin"`   | `peerId`           | A peer joined the topic. |
| `"peerLeave"`  | `peerId`           | A peer left the topic. |
| `"error"`      | `errorMessage`     | An error occurred. |

### Gossip Chat Example

```javascript
var iroh = require("rampart-iroh");

// Node 1: create topic
var ep1 = new iroh.Endpoint();
ep1.on("online", function() {
    var gossip1 = new iroh.Gossip(ep1);
    gossip1.on("ready", function() {
        var topic = gossip1.subscribe("chat-room");

        topic.on("message", function(msg) {
            console.log(msg.sender + ":", msg.data);
        });

        topic.on("peerJoin", function(peerId) {
            topic.broadcast("Welcome!");
        });
    });
});

// Node 2: join topic with Node 1 as bootstrap peer
var ep2 = new iroh.Endpoint();
ep2.on("online", function() {
    var gossip2 = new iroh.Gossip(ep2);
    gossip2.on("ready", function() {
        var topic = gossip2.subscribe("chat-room", [node1Id]);

        topic.on("message", function(msg) {
            console.log(msg.sender + ":", msg.data);
        });

        topic.on("ready", function() {
            topic.broadcast("Hello from Node 2!");
        });
    });
});
```

## Blobs

Blobs provides content-addressed storage and transfer. Data is identified
by its BLAKE3 hash and can be transferred between nodes using tickets.

### Constructor

```javascript
var blobs = new iroh.Blobs(endpoint [, options]);
```

- **endpoint** (Endpoint) -- A ready Endpoint.
- **options** (Object, optional):

| Property | Type   | Description |
|----------|--------|-------------|
| `path`   | String | Directory path for persistent blob storage. If omitted, blobs are stored in memory only. |

### Events

| Event     | Callback Arguments | Description |
|-----------|--------------------|-------------|
| `"ready"` | *(none)*           | Blob store is initialized and ready. |
| `"error"` | `errorMessage`     | An error occurred. |

### Methods

#### blobs.addBytes(data, callback)

Store bytes in the blob store.

- **data** (String or Buffer) -- The data to store.
- **callback** (Function) -- Called with a result object: `{ hash, ticket }`.
  - `hash` (String) -- The BLAKE3 hash of the stored data.
  - `ticket` (String) -- A transfer ticket that other nodes can use to
    download this blob.

```javascript
blobs.addBytes("Hello, world!", function(result) {
    console.log("Hash:", result.hash);
    console.log("Ticket:", result.ticket);
});
```

#### blobs.addFile(path, callback)

Store a file's contents in the blob store.

- **path** (String) -- Path to the file.
- **callback** (Function) -- Called with `{ hash, ticket }` (same as `addBytes`).

```javascript
blobs.addFile("/path/to/file.txt", function(result) {
    console.log("Hash:", result.hash);
});
```

#### blobs.read(hash, callback)

Read a blob's content from the local store.

- **hash** (String) -- The BLAKE3 hash of the blob.
- **callback** (Function) -- Called with the data as a String.

```javascript
blobs.read(hash, function(data) {
    console.log("Content:", data);
});
```

#### blobs.download(ticket, callback)

Download a blob from a remote node using a transfer ticket.

- **ticket** (String) -- A blob transfer ticket (obtained from `addBytes`
  or `addFile` on the remote node).
- **callback** (Function) -- Called with the hash (String) of the downloaded
  blob on success.

```javascript
blobs.download(ticket, function(hash) {
    console.log("Downloaded:", hash);
    blobs.read(hash, function(data) {
        console.log("Content:", data);
    });
});
```

#### blobs.createTicket(hash)

Create a transfer ticket for a blob in the local store. This is a
synchronous call.

- **hash** (String) -- The BLAKE3 hash of the blob.

Returns the ticket string, or `null` if the blob is not found.

```javascript
var ticket = blobs.createTicket(hash);
```

#### blobs.close()

Shut down the blob store.

### Blob Transfer Example

```javascript
var iroh = require("rampart-iroh");

// Node 1: store and share a blob
var ep1 = new iroh.Endpoint();
ep1.on("online", function() {
    var blobs1 = new iroh.Blobs(ep1);
    blobs1.on("ready", function() {
        blobs1.addBytes("The quick brown fox.", function(result) {
            console.log("Ticket:", result.ticket);
            // Give this ticket to Node 2
        });
    });
});

// Node 2: download the blob
var ep2 = new iroh.Endpoint();
ep2.on("online", function() {
    var blobs2 = new iroh.Blobs(ep2);
    blobs2.on("ready", function() {
        blobs2.download(ticket, function(hash) {
            blobs2.read(hash, function(data) {
                console.log("Downloaded:", data);
            });
        });
    });
});
```

## Docs

Docs provides replicated key-value documents that sync automatically
between peers. Each document has a namespace, and entries are written
by authors (each with their own cryptographic identity). Docs syncs
over the iroh-docs protocol and uses iroh-blobs for content transfer.

### Constructor

```javascript
var docs = new iroh.Docs(endpoint [, options]);
```

- **endpoint** (Endpoint) -- A ready Endpoint.
- **options** (Object, optional):

| Property | Type   | Description |
|----------|--------|-------------|
| `path`   | String | Directory path for persistent document storage. If omitted, documents are stored in memory only. |

### Events

| Event     | Callback Arguments | Description |
|-----------|--------------------|-------------|
| `"ready"` | *(none)*           | Document store is initialized and ready. |
| `"error"` | `errorMessage`     | An error occurred. |

### Methods

#### docs.createAuthor(callback)

Create a new author identity.

- **callback** (Function) -- Called with the author ID (String).

```javascript
docs.createAuthor(function(authorId) {
    console.log("Author:", authorId);
});
```

#### docs.createDoc(callback)

Create a new document (namespace).

- **callback** (Function) -- Called with the namespace ID (String).

```javascript
docs.createDoc(function(nsId) {
    console.log("Document:", nsId);
});
```

#### docs.set(nsId, authorId, key, value [, callback])

Set a key-value pair in a document.

- **nsId** (String) -- The namespace ID.
- **authorId** (String) -- The author ID.
- **key** (String) -- The key.
- **value** (String or Buffer) -- The value.
- **callback** (Function, optional) -- Called with no arguments on success.

```javascript
docs.set(nsId, authorId, "greeting", "Hello!", function() {
    console.log("Value set.");
});
```

#### docs.get(nsId, authorId, key, callback)

Get a value by namespace, author, and key.

- **nsId** (String) -- The namespace ID.
- **authorId** (String) -- The author ID.
- **key** (String) -- The key.
- **callback** (Function) -- Called with the value (Buffer) or `null`
  if not found.

```javascript
docs.get(nsId, authorId, "greeting", function(buf) {
    // buf is a plain buffer; convert to string if needed
    var str = bufToStr(buf);
    console.log("Value:", str);
});
```

#### docs.getLatest(nsId, key, callback)

Get the latest value for a key across all authors.

- **nsId** (String) -- The namespace ID.
- **key** (String) -- The key.
- **callback** (Function) -- Called with the value (Buffer) or `null`
  if not found.

```javascript
docs.getLatest(nsId, "greeting", function(buf) {
    var str = bufToStr(buf);
    console.log("Latest value:", str);
});
```

#### docs.delete(nsId, authorId, key [, callback])

Delete a key from a document.

- **nsId** (String) -- The namespace ID.
- **authorId** (String) -- The author ID.
- **key** (String) -- The key to delete.
- **callback** (Function, optional) -- Called with no arguments on success.

```javascript
docs.delete(nsId, authorId, "old-key", function() {
    console.log("Deleted.");
});
```

#### docs.createTicket(nsId, callback)

Create a sharing ticket for a document.

- **nsId** (String) -- The namespace ID.
- **callback** (Function) -- Called with the ticket string.

```javascript
docs.createTicket(nsId, function(ticket) {
    console.log("Share this ticket:", ticket);
});
```

#### docs.join(ticket, callback)

Join a document using a sharing ticket from another node. The document's
contents are synced automatically.

- **ticket** (String) -- A document sharing ticket.
- **callback** (Function) -- Called with the namespace ID (String) once
  sync is complete.

```javascript
docs.join(ticket, function(nsId) {
    console.log("Joined document:", nsId);
    docs.getLatest(nsId, "greeting", function(buf) {
        console.log("Synced value:", bufToStr(buf));
    });
});
```

#### docs.close()

Shut down the document store.

### Buffer to String Helper

The `get()` and `getLatest()` methods return plain buffers. To convert
to a string:

```javascript
function bufToStr(buf) {
    if (typeof buf === 'string') return buf;
    if (buf === null || buf === undefined) return null;
    var u8 = new Uint8Array(buf);
    var s = '';
    for (var i = 0; i < u8.length; i++) {
        s += String.fromCharCode(u8[i]);
    }
    return s;
}
```

### Distributed Key-Value Store Example

```javascript
var iroh = require("rampart-iroh");

// Node 1: create document and set values
var ep1 = new iroh.Endpoint();
ep1.on("online", function() {
    var docs1 = new iroh.Docs(ep1);
    docs1.on("ready", function() {
        docs1.createAuthor(function(authorId) {
            docs1.createDoc(function(nsId) {
                docs1.set(nsId, authorId, "name", "Alice", function() {
                    docs1.set(nsId, authorId, "status", "online", function() {
                        // Create ticket for sharing
                        docs1.createTicket(nsId, function(ticket) {
                            console.log("Share ticket:", ticket);
                        });
                    });
                });
            });
        });
    });
});

// Node 2: join and read synced data
var ep2 = new iroh.Endpoint();
ep2.on("online", function() {
    var docs2 = new iroh.Docs(ep2);
    docs2.on("ready", function() {
        docs2.join(ticket, function(nsId) {
            docs2.getLatest(nsId, "name", function(buf) {
                console.log("Name:", bufToStr(buf));  // "Alice"
            });
        });
    });
});
```

## Event Emitter Methods

All iroh objects (Endpoint, Connection, Stream, Gossip, GossipTopic,
Blobs, Docs) share the same event emitter interface:

| Method                        | Description |
|-------------------------------|-------------|
| `obj.on(event, callback)`    | Register a persistent event listener. |
| `obj.once(event, callback)`  | Register a one-time event listener (removed after first call). |
| `obj.off(event, callback)`   | Remove a specific event listener. |
| `obj.trigger(event [, args])` | Manually fire an event. |

If an `"error"` event fires and no handler is registered, the error is
thrown as an uncaught exception.

### Auto-Start Behavior

Some events trigger automatic behavior when a listener is first registered:

- Registering `"connection"` on an Endpoint starts the connection accept loop.
- Registering `"stream"` on a Connection starts the stream accept loop.
- Registering `"data"` on a Stream starts the read loop.
