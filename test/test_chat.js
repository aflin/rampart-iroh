// Gossip chat test for rampart-iroh
//
// Creates two endpoints with gossip, subscribes them to the same topic,
// and verifies messages are exchanged.

var iroh = require("rampart-iroh");

var TOPIC_NAME = "test-chat-topic";
var TEST_MSG_1 = "Hello from Node 1!";
var TEST_MSG_2 = "Hello from Node 2!";

var node1Ready = false;
var node2Ready = false;
var node1ReceivedMsg = false;
var node2ReceivedMsg = false;
var testPassed = false;

// ---- Node 1: Creates the topic first ----

console.log("Creating Node 1 endpoint...");
var ep1 = new iroh.Endpoint();

ep1.on("error", function(err) {
    console.log("Node 1 endpoint error:", err);
});

ep1.on("online", function() {
    console.log("Node 1 online! ID:", this.nodeId);

    console.log("Node 1: Creating gossip...");
    var gossip1 = new iroh.Gossip(ep1);

    gossip1.on("error", function(err) {
        console.log("Node 1 gossip error:", err);
    });

    gossip1.on("ready", function() {
        console.log("Node 1: Gossip ready, subscribing to topic...");
        var topic1 = gossip1.subscribe(TOPIC_NAME);

        topic1.on("error", function(err) {
            console.log("Node 1 topic error:", err);
        });

        topic1.on("ready", function() {
            console.log("Node 1: Subscribed to topic!");
            node1Ready = true;
            // Start Node 2 once Node 1 is subscribed
            startNode2(ep1.nodeId, topic1);
        });

        topic1.on("message", function(msg) {
            console.log("Node 1 received:", msg.data, "from:", msg.sender);
            if (msg.data === TEST_MSG_2) {
                node1ReceivedMsg = true;
                checkDone();
            }
        });

        topic1.on("peerJoin", function(peerId) {
            console.log("Node 1: peer joined:", peerId);
            // Once Node 2 joins, send our message
            console.log("Node 1: broadcasting message...");
            topic1.broadcast(TEST_MSG_1);
        });

        topic1.on("peerLeave", function(peerId) {
            console.log("Node 1: peer left:", peerId);
        });
    });
});

// ---- Node 2: Joins Node 1's topic ----

function startNode2(node1Id, topic1) {
    console.log("\nCreating Node 2 endpoint...");
    var ep2 = new iroh.Endpoint();

    ep2.on("error", function(err) {
        console.log("Node 2 endpoint error:", err);
    });

    ep2.on("online", function() {
        console.log("Node 2 online! ID:", this.nodeId);

        console.log("Node 2: Creating gossip...");
        var gossip2 = new iroh.Gossip(ep2);

        gossip2.on("error", function(err) {
            console.log("Node 2 gossip error:", err);
        });

        gossip2.on("ready", function() {
            console.log("Node 2: Gossip ready, subscribing with peer...");
            // Subscribe with Node 1 as a bootstrap peer
            var topic2 = gossip2.subscribe(TOPIC_NAME, [node1Id]);

            topic2.on("error", function(err) {
                console.log("Node 2 topic error:", err);
            });

            topic2.on("ready", function() {
                console.log("Node 2: Subscribed to topic!");
                node2Ready = true;
            });

            topic2.on("message", function(msg) {
                console.log("Node 2 received:", msg.data, "from:", msg.sender);
                if (msg.data === TEST_MSG_1) {
                    node2ReceivedMsg = true;
                    // Send reply
                    console.log("Node 2: broadcasting reply...");
                    topic2.broadcast(TEST_MSG_2);
                    checkDone();
                }
            });

            topic2.on("peerJoin", function(peerId) {
                console.log("Node 2: peer joined:", peerId);
            });

            topic2.on("peerLeave", function(peerId) {
                console.log("Node 2: peer left:", peerId);
            });
        });
    });
}

function checkDone() {
    if (node1ReceivedMsg && node2ReceivedMsg) {
        console.log("\n=== GOSSIP CHAT TEST PASSED ===");
        console.log("  Node 1 received Node 2's message: YES");
        console.log("  Node 2 received Node 1's message: YES");
        testPassed = true;
        process.exit(0);
    }
}

// Timeout
setTimeout(function() {
    console.log("\n=== GOSSIP CHAT TEST TIMED OUT ===");
    console.log("  Node 1 ready:", node1Ready);
    console.log("  Node 2 ready:", node2Ready);
    console.log("  Node 1 received msg:", node1ReceivedMsg);
    console.log("  Node 2 received msg:", node2ReceivedMsg);
    process.exit(1);
}, 30000);
