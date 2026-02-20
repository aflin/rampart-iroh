// Echo server/client test for rampart-iroh

var iroh = require("rampart-iroh");

var ALPN = "test-echo/1";
var TEST_MESSAGE = "Hello from rampart-iroh!";
var serverAddr = null;
var testPassed = false;

console.log("Creating server endpoint...");
var server = new iroh.Endpoint({ alpn: ALPN });

server.on("error", function(err) {
    console.log("Server error:", err);
});

server.on("online", function() {
    console.log("Server online!");
    console.log("  Address:", this.address);
    serverAddr = this.address;
    startClient();
});

server.on("connection", function(conn) {
    console.log("Server: got connection from", conn.remoteId);

    conn.on("error", function(err) {
        console.log("Server conn error:", err);
    });

    conn.on("stream", function(stream) {
        console.log("Server: got stream");

        stream.on("data", function(buf) {
            console.log("Server received:", buf);
            stream.write(buf);
        });

        stream.on("end", function() {
            console.log("Server: stream ended, finishing");
            stream.finish();
        });
    });
});

function startClient() {
    console.log("\nCreating client endpoint...");
    var client = new iroh.Endpoint();

    client.on("error", function(err) {
        console.log("Client error:", err);
    });

    client.on("online", function() {
        console.log("Client online!");
        console.log("Client: connecting to server...");
        this.connect(serverAddr, ALPN, function(conn) {
            console.log("Client: connected!");

            conn.on("error", function(err) {
                console.log("Client conn error:", err);
            });

            conn.openBi(function(stream) {
                console.log("Client: stream opened");

                stream.on("data", function(buf) {
                    console.log("Client received echo:", buf);

                    if (buf === TEST_MESSAGE) {
                        console.log("\n=== ECHO TEST PASSED ===");
                        testPassed = true;
                    } else {
                        console.log("\n=== ECHO TEST FAILED ===");
                        console.log("Expected:", TEST_MESSAGE);
                        console.log("Got:", buf);
                    }

                    // Clean shutdown
                    server.close();
                    client.close();
                    setTimeout(function() {
                        process.exit(testPassed ? 0 : 1);
                    }, 1000);
                });

                console.log("Client: sending:", TEST_MESSAGE);
                stream.write(TEST_MESSAGE);
                stream.finish();
            });
        });
    });
}

setTimeout(function() {
    if (!testPassed) {
        console.log("\n=== TEST TIMED OUT ===");
        process.exit(1);
    }
}, 30000);
