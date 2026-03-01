"use transpiler"

var iroh = require("rampart-iroh");

/* ---- Promise helpers ---- */

function waitFor(emitter, event) {
    return new Promise(function(resolve, reject) {
        emitter.on(event, function() {
            resolve(this);
        });
        emitter.on("error", reject);
    });
}

function promisify(obj, method) {
    return function() {
        var args = Array.prototype.slice.call(arguments);
        return new Promise(function(resolve, reject) {
            args.push(function(result) {
                resolve(result);
            });
            obj.on("error", reject);
            obj[method].apply(obj, args);
        });
    };
}

/* ---- Test harness ---- */

var testnum = 0;

function testFeature(name, test) {
    testnum++;
    rampart.utils.printf("async test - %3d - %-44s - ", testnum, name);
    if (test)
        rampart.utils.printf("passed\n");
    else {
        rampart.utils.printf(">>>>> FAILED <<<<<\n");
        process.exit(1);
    }
}

/* ---- Main async test ---- */

async function main() {

    // 1. Endpoint comes online
    var ep = new iroh.Endpoint({alpn: "test-async/1"});
    var epInfo = await waitFor(ep, "online");
    testFeature("endpoint comes online", true);
    testFeature("nodeId is a string",
        typeof epInfo.nodeId === 'string' && epInfo.nodeId.length > 0);

    // 2. Docs ready
    var docs = new iroh.Docs(ep);
    await waitFor(docs, "ready");
    testFeature("docs ready", true);

    // Create promisified wrappers
    var createAuthor = promisify(docs, "createAuthor");
    var createDoc = promisify(docs, "createDoc");
    var set = promisify(docs, "set");
    var get = promisify(docs, "get");
    var getAttr = promisify(docs, "getAttr");
    var getAll = promisify(docs, "getAll");
    var del = promisify(docs, "delete");

    // 3. Create author
    var authorId = await createAuthor();
    testFeature("createAuthor returns id",
        typeof authorId === 'string' && authorId.length > 0);

    // 4. Create doc
    var nsId = await createDoc();
    testFeature("createDoc returns namespace",
        typeof nsId === 'string' && nsId.length > 0);

    // 5. Set single key
    await set(nsId, authorId, "greeting", "hello async world");
    testFeature("set single key", true);

    // 6. Set multiple keys via object
    await set(nsId, authorId, {"color": "blue", "count": "42"});
    testFeature("set object (multi-key)", true);

    // 7. Get single key
    var val = await get(nsId, authorId, "greeting");
    val = rampart.utils.bufferToString(val);
    testFeature("get returns correct value", val === "hello async world");

    // 8. getAttr
    var val2 = await getAttr(nsId, "color");
    val2 = rampart.utils.bufferToString(val2);
    testFeature("getAttr returns correct value", val2 === "blue");

    // 9. getAll
    var all = await getAll(nsId);
    testFeature("getAll returns object", typeof all === 'object' && all !== null);
    testFeature("getAll has greeting",
        rampart.utils.bufferToString(all.greeting) === "hello async world");
    testFeature("getAll has color",
        rampart.utils.bufferToString(all.color) === "blue");
    testFeature("getAll has count",
        rampart.utils.bufferToString(all.count) === "42");

    // 10. Delete and verify
    await del(nsId, authorId, "count");
    testFeature("delete key", true);

    var deleted = await getAttr(nsId, "count");
    testFeature("deleted key returns null", deleted === null);

    // 11. Echo server/client
    var ALPN = "async-echo-test/1";
    var MSG = "async echo message";

    var serverEp = new iroh.Endpoint({alpn: ALPN});
    var serverInfo = await waitFor(serverEp, "online");
    var serverAddr = serverInfo.address;
    testFeature("server endpoint online", true);

    // Server echoes back data on incoming streams
    serverEp.on("connection", function(conn) {
        conn.on("stream", function(stream) {
            stream.on("data", function(buf) {
                stream.write(buf);
            });
            stream.on("end", function() {
                stream.finish();
            });
        });
    });

    var clientEp = new iroh.Endpoint();
    await waitFor(clientEp, "online");
    testFeature("client endpoint online", true);

    // Connect and echo
    var echoResult = await new Promise(function(resolve, reject) {
        clientEp.connect(serverAddr, ALPN, function(conn) {
            conn.openBi(function(stream) {
                stream.on("data", function(buf) {
                    resolve(rampart.utils.bufferToString(buf));
                });
                stream.write(MSG);
            });
        });
    });
    testFeature("echo returns correct data", echoResult === MSG);

    // Cleanup
    ep.close();
    serverEp.close();
    clientEp.close();

    rampart.utils.printf("\n=== ALL %d ASYNC TESTS PASSED ===\n", testnum);
    setTimeout(function() { process.exit(0); }, 1500);
}

main().then(null, function(e) {
    console.log("\nAsync test error:", e);
    process.exit(1);
});

// Timeout safety
setTimeout(function() {
    console.log("\n=== ASYNC TEST TIMED OUT ===");
    process.exit(1);
}, 30000);
