/* rampart-iroh test suite */
rampart.globalize(rampart.utils);

var iroh = require("rampart-iroh");

var testnum = 0;

function testFeature(name, test) {
    var error = false;
    testnum++;
    if (typeof test == 'function') {
        try {
            test = test();
        } catch(e) {
            error = e;
            test = false;
        }
    }
    printf("testing iroh - %3d - %-47s - ", testnum, name);
    fflush(stdout);
    if (test)
        printf("passed\n");
    else {
        printf(">>>>> FAILED <<<<<\n");
        if (error) console.log(error);
        process.exit(1);
    }
    if (error) console.log(error);
}

/* ================================================================
   Test 1: Module load and constructors
   ================================================================ */

testFeature("module loads", typeof iroh === 'object' && iroh !== null);
testFeature("Endpoint constructor exists", typeof iroh.Endpoint === 'function');
testFeature("Gossip constructor exists", typeof iroh.Gossip === 'function');
testFeature("Blobs constructor exists", typeof iroh.Blobs === 'function');
testFeature("Docs constructor exists", typeof iroh.Docs === 'function');
testFeature("createServer exists", typeof iroh.createServer === 'function');
testFeature("connect exists", typeof iroh.connect === 'function');

/* ================================================================
   Test 2: Endpoint lifecycle (online + close)
   ================================================================ */

var closeTestDone = false;

function runCloseTest(callback) {
    var ep = new iroh.Endpoint({alpn: "test/1"});
    var gotOnline = false;

    ep.on("error", function(err) {
        testFeature("close - no errors", false);
    });

    ep.on("online", function() {
        gotOnline = true;
        testFeature("endpoint comes online", true);
        testFeature("nodeId is a string", typeof this.nodeId === 'string' && this.nodeId.length > 0);
        testFeature("address is a string", typeof this.address === 'string' && this.address.length > 0);
        testFeature("secretKey is a buffer", typeof this.secretKey === 'object' && this.secretKey.byteLength === 32);
        testFeature("secretKeyHex is a hex string", typeof this.secretKeyHex === 'string' && this.secretKeyHex.length === 64);

        ep.close();

        setTimeout(function() {
            testFeature("endpoint close without crash", true);
            closeTestDone = true;
            callback();
        }, 1500);
    });

    setTimeout(function() {
        if (!closeTestDone) {
            testFeature("endpoint online (timeout)", false);
        }
    }, 15000);
}

/* ================================================================
   Test 3: Secret key persistence (hex and buffer round-trip)
   ================================================================ */

function runSecretKeyTest(callback) {
    var skDone = false;
    var ep1 = new iroh.Endpoint();

    ep1.on("error", function(err) {
        testFeature("secretkey - no errors", false);
    });

    ep1.on("online", function() {
        var savedHex = this.secretKeyHex;
        var savedBuf = this.secretKey;
        var savedNodeId = this.nodeId;
        ep1.close();

        setTimeout(function() {
            /* Restore from hex string */
            var ep2 = new iroh.Endpoint({ secretKey: savedHex });
            ep2.on("error", function(err) {
                testFeature("secretkey - hex restore no errors", false);
            });
            ep2.on("online", function() {
                testFeature("secretkey - hex restores same nodeId",
                    this.nodeId === savedNodeId);
                ep2.close();

                setTimeout(function() {
                    /* Restore from binary buffer */
                    var ep3 = new iroh.Endpoint({ secretKey: savedBuf });
                    ep3.on("error", function(err) {
                        testFeature("secretkey - buf restore no errors", false);
                    });
                    ep3.on("online", function() {
                        testFeature("secretkey - buffer restores same nodeId",
                            this.nodeId === savedNodeId);
                        ep3.close();
                        skDone = true;
                        setTimeout(callback, 1000);
                    });
                }, 1000);
            });
        }, 1000);
    });

    setTimeout(function() {
        if (!skDone) {
            testFeature("secretkey test (timeout)", false);
        }
    }, 20000);
}

/* ================================================================
   Test 4: Echo server / client with streams
   ================================================================ */

var ALPN = "iroh-test-echo/1";
var ECHO_MSG = "Hello from rampart-iroh test!";

function runEchoTest(callback) {
    var serverGotConnection = false;
    var serverGotStream = false;
    var serverGotData = false;
    var clientConnected = false;
    var clientGotEcho = false;
    var echoMatch = false;
    var echoDone = false;
    var clientData = "";

    var server = new iroh.Endpoint({alpn: ALPN});

    server.on("error", function(err) {
        testFeature("echo - server no errors", false);
    });

    server.on("online", function() {
        var serverAddr = this.address;

        server.on("connection", function(conn) {
            serverGotConnection = true;

            conn.on("stream", function(stream) {
                serverGotStream = true;

                stream.on("data", function(buf) {
                    serverGotData = true;
                    stream.write(buf);
                });

                stream.on("end", function() {
                    stream.finish();
                });
            });
        });

        /* Start client */
        var client = new iroh.Endpoint();

        client.on("error", function(err) {
            testFeature("echo - client no errors", false);
        });

        client.on("online", function() {
            this.connect(serverAddr, ALPN, function(conn) {
                clientConnected = true;

                conn.openBi(function(stream) {

                    stream.on("data", function(buf) {
                        clientGotEcho = true;
                        clientData += sprintf("%s", buf);
                    });

                    stream.on("end", function() {
                        echoMatch = (clientData === ECHO_MSG);
                        echoDone = true;

                        testFeature("echo - server got connection", serverGotConnection);
                        testFeature("echo - server got stream", serverGotStream);
                        testFeature("echo - server got data", serverGotData);
                        testFeature("echo - client connected", clientConnected);
                        testFeature("echo - client received echo", clientGotEcho);
                        testFeature("echo - echo data matches", echoMatch);

                        server.close();
                        client.close();

                        setTimeout(callback, 1000);
                    });

                    stream.write(ECHO_MSG);
                    stream.finish();
                });
            });
        });
    });

    setTimeout(function() {
        if (!echoDone) {
            testFeature("echo test (timeout)", false);
        }
    }, 20000);
}

/* ================================================================
   Test 4: Gossip (two nodes exchange messages)
   ================================================================ */

var TOPIC_NAME = "iroh-test-topic";
var GOSSIP_MSG_1 = "Message from node 1";
var GOSSIP_MSG_2 = "Message from node 2";

function runGossipTest(callback) {
    var node1ReceivedMsg = false;
    var node2ReceivedMsg = false;

    var ep1 = new iroh.Endpoint();

    ep1.on("error", function(err) {
        testFeature("gossip - node1 no errors", false);
    });

    ep1.on("online", function() {
        var gossip1 = new iroh.Gossip(ep1);

        gossip1.on("error", function(err) {
            testFeature("gossip - gossip1 no errors", false);
        });

        gossip1.on("ready", function() {
            var topic1 = gossip1.subscribe(TOPIC_NAME);

            topic1.on("error", function(err) {
                testFeature("gossip - topic1 no errors", false);
            });

            topic1.on("message", function(msg) {
                if (bufferToString(msg.data) === GOSSIP_MSG_2) {
                    node1ReceivedMsg = true;
                    checkGossipDone();
                }
            });

            topic1.on("peerJoin", function(peerId) {
                topic1.broadcast(GOSSIP_MSG_1);
            });

            // wait a sec, because two connections on the same machine can create
            // a bit of a race, since ready doesn't mean full discovery has happened yet.
            // and peerJoin may not fire.
            // seems to only be an issue on raspberry pi.
            sleep(2);

            topic1.on("ready", function() {
                /* Start node 2 */
                var ep2 = new iroh.Endpoint();

                ep2.on("error", function(err) {
                    testFeature("gossip - node2 no errors", false);
                });

                ep2.on("online", function() {
                    var gossip2 = new iroh.Gossip(ep2);

                    gossip2.on("error", function(err) {
                        testFeature("gossip - gossip2 no errors", false);
                    });

                    gossip2.on("ready", function() {
                        var topic2 = gossip2.subscribe(TOPIC_NAME, [ep1.nodeId]);

                        topic2.on("error", function(err) {
                            testFeature("gossip - topic2 no errors", false);
                        });

                        topic2.on("message", function(msg) {
                            if (bufferToString(msg.data) === GOSSIP_MSG_1) {
                                node2ReceivedMsg = true;
                                topic2.broadcast(GOSSIP_MSG_2);
                                checkGossipDone();
                            }
                        });
                    });
                });
            });
        });
    });

    function checkGossipDone() {
        if (node1ReceivedMsg && node2ReceivedMsg) {
            testFeature("gossip - node2 received msg from node1", true);
            testFeature("gossip - node1 received msg from node2", true);
            callback();
        }
    }

    setTimeout(function() {
        if (!node1ReceivedMsg || !node2ReceivedMsg) {
            testFeature("gossip test (timeout)", false);
        }
    }, 20000);
}

/* ================================================================
   Test 5: Blobs (store, read local, transfer to second node)
   ================================================================ */

var BLOB_DATA = "The quick brown fox jumps over the lazy dog.";

function runBlobsTest(callback) {
    var storedOk = false;
    var localReadOk = false;
    var downloadOk = false;
    var remoteReadOk = false;

    var ep1 = new iroh.Endpoint();

    ep1.on("error", function(err) {
        testFeature("blobs - node1 no errors", false);
    });

    ep1.on("online", function() {
        var blobs1 = new iroh.Blobs(ep1);

        blobs1.on("error", function(err) {
            testFeature("blobs - blobs1 no errors", false);
        });

        blobs1.on("ready", function() {
            blobs1.addBytes(BLOB_DATA, function(result) {
                storedOk = !!(result && result.hash && result.ticket);
                testFeature("blobs - addBytes returns hash+ticket", storedOk);

                blobs1.read(result.hash, function(data) {
                    localReadOk = (bufferToString(data) === BLOB_DATA);
                    testFeature("blobs - local read matches", localReadOk);

                    /* Node 2 downloads */
                    var ep2 = new iroh.Endpoint();

                    ep2.on("error", function(err) {
                        testFeature("blobs - node2 no errors", false);
                    });

                    ep2.on("online", function() {
                        var blobs2 = new iroh.Blobs(ep2);

                        blobs2.on("error", function(err) {
                            testFeature("blobs - blobs2 no errors", false);
                        });

                        blobs2.on("ready", function() {
                            blobs2.download(result.ticket, function(hash) {
                                downloadOk = (typeof hash === 'string' && hash.length > 0);
                                testFeature("blobs - download returns hash", downloadOk);

                                blobs2.read(hash, function(data2) {
                                    remoteReadOk = (bufferToString(data2) === BLOB_DATA);
                                    testFeature("blobs - remote read matches", remoteReadOk);
                                    callback();
                                });
                            });
                        });
                    });
                });
            });
        });
    });

    setTimeout(function() {
        if (!remoteReadOk) {
            testFeature("blobs test (timeout)", false);
        }
    }, 20000);
}

/* ================================================================
   Test 6: Docs (create, set, get, delete, two-node sync)
   ================================================================ */

var DOC_KEY = "greeting";
var DOC_VAL = "Hello from iroh docs!";
var DOC_KEY2 = "counter";
var DOC_VAL2 = "42";

function runDocsTest(callback) {
    var createAuthorOk = false;
    var createDocOk = false;
    var setOk = false;
    var getOk = false;
    var getAttrOk = false;
    var deleteOk = false;
    var syncOk = false;

    var ep1 = new iroh.Endpoint();

    ep1.on("error", function(err) {
        testFeature("docs - node1 no errors", false);
    });

    ep1.on("online", function() {
        var docs1 = new iroh.Docs(ep1);

        docs1.on("error", function(err) {
            testFeature("docs - docs1 no errors", false);
        });

        docs1.on("ready", function() {
            docs1.createAuthor(function(authorId) {
                createAuthorOk = (typeof authorId === 'string' && authorId.length > 0);
                testFeature("docs - createAuthor", createAuthorOk);

                docs1.createDoc(function(nsId) {
                    createDocOk = (typeof nsId === 'string' && nsId.length > 0);
                    testFeature("docs - createDoc", createDocOk);

                    docs1.set(nsId, authorId, DOC_KEY, DOC_VAL, function() {
                        docs1.set(nsId, authorId, DOC_KEY2, DOC_VAL2, function() {
                            setOk = true;
                            testFeature("docs - set key-value pairs", true);

                            docs1.get(nsId, authorId, DOC_KEY, function(data) {
                                getOk = (bufferToString(data) === DOC_VAL);
                                testFeature("docs - get returns correct value", getOk);

                                docs1.getAttr(nsId, DOC_KEY, function(data2) {
                                    getAttrOk = (bufferToString(data2) === DOC_VAL);
                                    testFeature("docs - getAttr returns correct value", getAttrOk);

                                    docs1.delete(nsId, authorId, DOC_KEY2, function() {
                                        docs1.getAttr(nsId, DOC_KEY2, function(data3) {
                                            deleteOk = (data3 === null);
                                            testFeature("docs - delete removes key", deleteOk);

                                            /* Two-node sync */
                                            docs1.createTicket(nsId, function(ticket) {
                                                testFeature("docs - createTicket", typeof ticket === 'string' && ticket.length > 0);
                                                startDocsNode2(ticket, docs1, callback);
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

    setTimeout(function() {
        if (!syncOk) {
            testFeature("docs test (timeout)", false);
        }
    }, 25000);
}

function startDocsNode2(ticket, docs1, callback) {
    var ep2 = new iroh.Endpoint();

    ep2.on("error", function(err) {
        testFeature("docs - node2 no errors", false);
    });

    ep2.on("online", function() {
        var docs2 = new iroh.Docs(ep2);

        docs2.on("error", function(err) {
            testFeature("docs - docs2 no errors", false);
        });

        docs2.on("ready", function() {
            docs2.join(ticket, function(nsId) {
                testFeature("docs - node2 joins via ticket", typeof nsId === 'string' && nsId.length > 0);

                docs2.getAttr(nsId, DOC_KEY, function(data) {
                    var val = bufferToString(data);
                    testFeature("docs - node2 reads synced value", val === DOC_VAL);
                    callback();
                });
            });
        });
    });
}

/* ================================================================
   Test 7: Docs set with object (multi-set)
   ================================================================ */

var MULTI_SET_OBJ = {"name": "Alice", "status": "online", "color": "blue"};

function runDocsMultiSetTest(callback) {
    var multiSetOk = false;
    var allReadOk = false;

    var ep = new iroh.Endpoint();

    ep.on("error", function(err) {
        testFeature("docs multi-set - no errors", false);
    });

    ep.on("online", function() {
        var docs = new iroh.Docs(ep);

        docs.on("error", function(err) {
            testFeature("docs multi-set - docs no errors", false);
        });

        docs.on("ready", function() {
            docs.createAuthor(function(authorId) {
                docs.createDoc(function(nsId) {

                    /* Use object form to set multiple key-value pairs at once */
                    docs.set(nsId, authorId, MULTI_SET_OBJ, function() {
                        multiSetOk = true;
                        testFeature("docs - set with object", true);

                        /* Verify all three keys were set */
                        docs.getAttr(nsId, "name", function(data) {
                            testFeature("docs multi-set - name correct",
                                bufferToString(data) === "Alice");

                            docs.getAttr(nsId, "status", function(data) {
                                testFeature("docs multi-set - status correct",
                                    bufferToString(data) === "online");

                                docs.getAttr(nsId, "color", function(data) {
                                    testFeature("docs multi-set - color correct",
                                        bufferToString(data) === "blue");

                                    /* Also verify single-key set still works alongside */
                                    docs.set(nsId, authorId, "extra", "value", function() {
                                        docs.getAttr(nsId, "extra", function(data) {
                                            testFeature("docs - single set still works after multi-set",
                                                bufferToString(data) === "value");

                                            /* Test getAll — should return all 4 keys as an object */
                                            docs.getAll(nsId, function(obj) {
                                                testFeature("docs - getAll returns object",
                                                    typeof obj === 'object' && obj !== null);
                                                testFeature("docs - getAll has name",
                                                    bufferToString(obj.name) === "Alice");
                                                testFeature("docs - getAll has status",
                                                    bufferToString(obj.status) === "online");
                                                testFeature("docs - getAll has color",
                                                    bufferToString(obj.color) === "blue");
                                                testFeature("docs - getAll has extra",
                                                    bufferToString(obj.extra) === "value");
                                                allReadOk = true;
                                                callback();
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

    setTimeout(function() {
        if (!allReadOk) {
            testFeature("docs multi-set test (timeout)", false);
        }
    }, 25000);
}

/* ================================================================
   Run all tests sequentially
   ================================================================ */

function allDone() {
    printf("\niroh: all %d tests passed\n", testnum);
    process.exit(0);
}

runCloseTest(function() {
    runSecretKeyTest(function() {
    runEchoTest(function() {
        runGossipTest(function() {
            runBlobsTest(function() {
                runDocsTest(function() {
                    runDocsMultiSetTest(function() {
                        allDone();
                    });
                });
            });
        });
    });
    });
});

/* Global safety timeout */
setTimeout(function() {
    printf("\niroh: GLOBAL TIMEOUT - tests did not complete in 120 seconds\n");
    process.exit(1);
}, 120000);
