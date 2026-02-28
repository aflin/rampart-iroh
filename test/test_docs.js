// Docs test for rampart-iroh
//
// Node 1 creates a document, sets key-value pairs, creates a ticket.
// Node 2 joins via ticket and reads back the synced data.

var iroh = require("rampart-iroh");

var TEST_KEY = "greeting";
var TEST_VALUE = "Hello from iroh docs!";
var TEST_KEY2 = "counter";
var TEST_VALUE2 = "42";

// Helper: convert plain Duktape buffer to string
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

// ---- Node 1: creates doc and sets data ----

console.log("Creating Node 1 endpoint...");
var ep1 = new iroh.Endpoint();

ep1.on("error", function(err) {
    console.log("Node 1 endpoint error:", err);
});

ep1.on("online", function() {
    console.log("Node 1 online! ID:", this.nodeId);

    console.log("Node 1: Creating docs...");
    var docs1 = new iroh.Docs(ep1);

    docs1.on("error", function(err) {
        console.log("Node 1 docs error:", err);
    });

    docs1.on("ready", function() {
        console.log("Node 1: Docs ready, creating author...");

        docs1.createAuthor(function(authorId) {
            console.log("Node 1: Author created:", authorId);

            docs1.createDoc(function(nsId) {
                console.log("Node 1: Doc created:", nsId);

                // Set first key-value pair
                console.log("Node 1: Setting key '" + TEST_KEY + "'...");
                docs1.set(nsId, authorId, TEST_KEY, TEST_VALUE, function() {
                    console.log("Node 1: Set '" + TEST_KEY + "' OK");

                    // Set second key-value pair
                    docs1.set(nsId, authorId, TEST_KEY2, TEST_VALUE2, function() {
                        console.log("Node 1: Set '" + TEST_KEY2 + "' OK");

                        // Read back with get(ns, author, key)
                        docs1.get(nsId, authorId, TEST_KEY, function(data) {
                            var val = bufToStr(data);
                            console.log("Node 1: get() returned:", val);
                            if (val !== TEST_VALUE) {
                                console.log("ERROR: get() mismatch!");
                                console.log("  Expected:", TEST_VALUE);
                                console.log("  Got:", val);
                                process.exit(1);
                            }
                            console.log("Node 1: get() verified OK");

                            // Read back with getAttr(ns, key)
                            docs1.getAttr(nsId, TEST_KEY, function(data2) {
                                var val2 = bufToStr(data2);
                                console.log("Node 1: getAttr() returned:", val2);
                                if (val2 !== TEST_VALUE) {
                                    console.log("ERROR: getAttr() mismatch!");
                                    console.log("  Expected:", TEST_VALUE);
                                    console.log("  Got:", val2);
                                    process.exit(1);
                                }
                                console.log("Node 1: getAttr() verified OK");

                                // Delete key2 and verify it's gone
                                docs1.delete(nsId, authorId, TEST_KEY2, function() {
                                    console.log("Node 1: Deleted '" + TEST_KEY2 + "'");

                                    docs1.getAttr(nsId, TEST_KEY2, function(data3) {
                                        var val3 = bufToStr(data3);
                                        console.log("Node 1: getAttr('" + TEST_KEY2 + "') after delete:", val3);
                                        if (val3 !== null) {
                                            console.log("ERROR: deleted key still returns data!");
                                            process.exit(1);
                                        }
                                        console.log("Node 1: delete verified OK");

                                        // Create ticket for Node 2
                                        docs1.createTicket(nsId, function(ticket) {
                                            console.log("Node 1: Ticket created:", ticket.substring(0, 40) + "...");

                                            if (!ticket) {
                                                console.log("ERROR: No ticket returned");
                                                process.exit(1);
                                            }

                                            // Start Node 2
                                            startNode2(ticket, docs1);
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
});

// ---- Node 2: joins doc via ticket ----

function startNode2(ticket, docs1) {
    console.log("\nCreating Node 2 endpoint...");
    var ep2 = new iroh.Endpoint();

    ep2.on("error", function(err) {
        console.log("Node 2 endpoint error:", err);
    });

    ep2.on("online", function() {
        console.log("Node 2 online! ID:", this.nodeId);

        console.log("Node 2: Creating docs...");
        var docs2 = new iroh.Docs(ep2);

        docs2.on("error", function(err) {
            console.log("Node 2 docs error:", err);
        });

        docs2.on("ready", function() {
            console.log("Node 2: Docs ready, joining via ticket...");

            docs2.join(ticket, function(nsId) {
                console.log("Node 2: Joined doc:", nsId);

                // Try to read the synced data
                console.log("Node 2: Reading synced data...");
                docs2.getAttr(nsId, TEST_KEY, function(data) {
                    var val = bufToStr(data);
                    console.log("Node 2: getAttr('" + TEST_KEY + "') =", val);

                    if (val === TEST_VALUE) {
                        console.log("\n=== DOCS TEST PASSED ===");
                        console.log("  Single-node set/get/getAttr/delete: OK");
                        console.log("  Two-node join and sync: OK");
                        console.log("  Synced value:", val);
                        process.exit(0);
                    } else {
                        console.log("\n=== DOCS TEST FAILED ===");
                        console.log("  Expected:", TEST_VALUE);
                        console.log("  Got:", val);
                        process.exit(1);
                    }
                });
            });
        });
    });
}

// Timeout
setTimeout(function() {
    console.log("\n=== DOCS TEST TIMED OUT ===");
    process.exit(1);
}, 30000);
