// Blob transfer test for rampart-iroh
//
// Node 1 stores bytes, produces a ticket.
// Node 2 downloads via ticket and reads back to verify contents match.

var iroh = require("rampart-iroh");

var TEST_DATA = "The quick brown fox jumps over the lazy dog.";
var testPassed = false;

// ---- Node 1: stores a blob ----

console.log("Creating Node 1 endpoint...");
var ep1 = new iroh.Endpoint();

ep1.on("error", function(err) {
    console.log("Node 1 endpoint error:", err);
});

ep1.on("online", function() {
    console.log("Node 1 online! ID:", this.nodeId);

    console.log("Node 1: Creating blobs...");
    var blobs1 = new iroh.Blobs(ep1);

    blobs1.on("error", function(err) {
        console.log("Node 1 blobs error:", err);
    });

    blobs1.on("ready", function() {
        console.log("Node 1: Blobs ready, adding bytes...");

        blobs1.addBytes(TEST_DATA, function(result) {
            console.log("Node 1: Blob stored!");
            console.log("  hash:", result.hash);
            console.log("  ticket:", result.ticket);

            if (!result.ticket) {
                console.log("ERROR: No ticket returned from addBytes");
                process.exit(1);
            }

            // Also verify we can read it back locally
            blobs1.read(result.hash, function(data) {
                console.log("Node 1: Local read back:", data);
                if (data !== TEST_DATA) {
                    console.log("ERROR: Local read mismatch!");
                    console.log("  Expected:", TEST_DATA);
                    console.log("  Got:", data);
                    process.exit(1);
                }
                console.log("Node 1: Local read verified OK");

                // Now start Node 2 to download
                startNode2(result.ticket, blobs1);
            });
        });
    });
});

// ---- Node 2: downloads the blob ----

function startNode2(ticket, blobs1) {
    console.log("\nCreating Node 2 endpoint...");
    var ep2 = new iroh.Endpoint();

    ep2.on("error", function(err) {
        console.log("Node 2 endpoint error:", err);
    });

    ep2.on("online", function() {
        console.log("Node 2 online! ID:", this.nodeId);

        console.log("Node 2: Creating blobs...");
        var blobs2 = new iroh.Blobs(ep2);

        blobs2.on("error", function(err) {
            console.log("Node 2 blobs error:", err);
        });

        blobs2.on("ready", function() {
            console.log("Node 2: Blobs ready, downloading...");

            blobs2.download(ticket, function(hash) {
                console.log("Node 2: Download complete! Hash:", hash);

                // Read back the downloaded blob
                blobs2.read(hash, function(data) {
                    console.log("Node 2: Read data:", data);

                    if (data === TEST_DATA) {
                        console.log("\n=== BLOB TRANSFER TEST PASSED ===");
                        console.log("  Original data:", TEST_DATA);
                        console.log("  Downloaded data:", data);
                        testPassed = true;
                        process.exit(0);
                    } else {
                        console.log("\n=== BLOB TRANSFER TEST FAILED ===");
                        console.log("  Expected:", TEST_DATA);
                        console.log("  Got:", data);
                        process.exit(1);
                    }
                });
            });
        });
    });
}

// Timeout
setTimeout(function() {
    console.log("\n=== BLOB TRANSFER TEST TIMED OUT ===");
    process.exit(1);
}, 30000);
