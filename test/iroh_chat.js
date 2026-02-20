#!/usr/bin/env rampart

// iroh_chat.js — Interactive P2P chat over iroh gossip
//
// Usage:
//   rampart iroh_chat.js --nick NAME [--topic TOPIC] [--join NODE_ID]
//
// First user (creates the room):
//   rampart iroh_chat.js --nick Alice
//
// Others (join with Node ID printed by first user):
//   rampart iroh_chat.js --nick Bob --join <NODE_ID>
//
// Commands:
//   @name message    Direct message to 'name'
//   /send <path>     Send a file (max ~200KB)
//   /save [name]     Save last received file
//   /who             List connected peers
//   /nick <name>     Change your nickname
//   /help            Show commands
//   /quit            Leave the chat

var iroh = require("rampart-iroh");
rampart.globalize(rampart.utils);

var MAX_FILE_SIZE = 200 * 1024; // 200KB limit for gossip file transfer

// -- Parse arguments --

var args = process.argv.slice(2);
var nick = null;
var topicName = "rampart-chat";
var joinPeer = null;

function usage(msg, code) {
    if (msg) printf("%s\n\n", msg);
    printf("Usage: rampart iroh_chat.js --nick NAME [--topic TOPIC] [--join NODE_ID]\n\n");
    printf("  --nick, -n NAME      Your display name (required)\n");
    printf("  --topic, -t TOPIC    Chat room name (default: rampart-chat)\n");
    printf("  --join, -j NODE_ID   Node ID of an existing peer to join\n");
    process.exit(code || 0);
}

for (var i = 0; i < args.length; i++) {
    switch (args[i]) {
        case '--nick': case '-n':
            nick = args[++i];
            break;
        case '--topic': case '-t':
            topicName = args[++i];
            break;
        case '--join': case '-j':
            joinPeer = args[++i];
            break;
        default:
            usage("Unknown argument: " + args[i], 1);
    }
}

if (!nick) usage("Error: --nick is required", 1);

// -- State --

var myNodeId = null;
var topic = null;
var peers = {};          // nodeId -> nick
var lastFile = null;     // {filename, data} - last received file (Buffer)
var inputStarted = false;

// -- Helpers --

function shortId(id) {
    return id ? id.substring(0, 8) + ".." : "?";
}

function chatPrint(msg) {
    printf("\r\x1b[K%s\n", msg);   // clear line, print, newline
    if (inputStarted) repl.refresh();
}

function makeMsg(obj) {
    obj.from = nick;
    obj.nodeId = myNodeId;
    return JSON.stringify(obj);
}

function showHelp() {
    chatPrint("  Commands:");
    chatPrint("    @name message    Direct message to 'name'");
    chatPrint("    /send <path>     Send a file (max ~200KB)");
    chatPrint("    /save [name]     Save last received file");
    chatPrint("    /who             List connected peers");
    chatPrint("    /nick <name>     Change your nickname");
    chatPrint("    /help            Show this help");
    chatPrint("    /quit            Leave the chat");
}

// -- Message handlers --

function onGossipMessage(msg) {
    var m;
    try {
        m = JSON.parse(msg.data);
    } catch (e) {
        return; // ignore malformed
    }

    // Ignore own messages
    if (m.nodeId === myNodeId) return;

    // Track peers
    if (m.nodeId && m.from) {
        peers[m.nodeId] = m.from;
    }

    switch (m.type) {
        case "chat":
            chatPrint(m.from + ": " + m.text);
            break;

        case "dm":
            if (m.to === nick) {
                chatPrint("\x1b[35m[DM from " + m.from + "]\x1b[0m " + m.text);
            }
            // silently ignore DMs for others
            break;

        case "file":
            chatPrint("\x1b[36m" + m.from + " sent file: " + m.filename +
                      " (" + m.filedata.length + " bytes encoded)\x1b[0m");
            try {
                lastFile = {
                    filename: m.filename,
                    data: Duktape.dec('base64', m.filedata)
                };
                chatPrint("  Type /save to save it, or /save <filename>");
            } catch (e) {
                chatPrint("  (failed to decode file data)");
            }
            break;

        case "join":
            chatPrint("\x1b[32m* " + m.from + " has joined\x1b[0m");
            // Reply with our presence so the new peer knows about us
            if (topic) topic.broadcast(makeMsg({type: "here"}));
            break;

        case "here":
            // Silent peer tracking (already updated above)
            break;

        case "leave":
            chatPrint("\x1b[31m* " + m.from + " has left\x1b[0m");
            if (m.nodeId) delete peers[m.nodeId];
            break;

        case "nick":
            chatPrint("\x1b[33m* " + m.oldNick + " is now known as " + m.from + "\x1b[0m");
            break;
    }
}

function onPeerJoin(peerId) {
    var peerNick = peers[peerId];
    if (peerNick) {
        chatPrint("\x1b[32m* " + peerNick + " (" + shortId(peerId) + ") connected\x1b[0m");
    } else {
        chatPrint("\x1b[32m* Peer " + shortId(peerId) + " connected\x1b[0m");
    }
}

function onPeerLeave(peerId) {
    var peerNick = peers[peerId] || shortId(peerId);
    chatPrint("\x1b[31m* " + peerNick + " disconnected\x1b[0m");
    delete peers[peerId];
}

// -- Command processing --

function processLine(line) {
    if (!line || !line.length) return;
    line = trim(line);
    if (!line.length) return;

    // /quit
    if (line === "/quit" || line === "/exit") {
        if (topic) topic.broadcast(makeMsg({type: "leave"}));
        printf("Goodbye!\n");
        // Give the broadcast a moment to send
        setTimeout(function() { process.exit(0); }, 500);
        return;
    }

    // /help
    if (line === "/help") {
        showHelp();
        return;
    }

    // /who
    if (line === "/who") {
        var names = [];
        for (var id in peers) {
            names.push(peers[id] + " (" + shortId(id) + ")");
        }
        if (names.length === 0) {
            chatPrint("  No other peers connected.");
        } else {
            chatPrint("  Connected peers:");
            for (var i = 0; i < names.length; i++) {
                chatPrint("    " + names[i]);
            }
        }
        return;
    }

    // /nick <name>
    if (line.indexOf("/nick ") === 0) {
        var newNick = trim(line.substring(6));
        if (!newNick.length) {
            chatPrint("  Usage: /nick <name>");
            return;
        }
        var oldNick = nick;
        nick = newNick;
        if (topic) topic.broadcast(makeMsg({type: "nick", oldNick: oldNick}));
        chatPrint("  You are now known as " + nick);
        return;
    }

    // /send <path>
    if (line.indexOf("/send ") === 0) {
        var path = trim(line.substring(6));
        if (!path.length) {
            chatPrint("  Usage: /send <filepath>");
            return;
        }
        var st = stat(path);
        if (!st || !st.isFile) {
            chatPrint("  File not found: " + path);
            return;
        }
        if (st.size > MAX_FILE_SIZE) {
            chatPrint("  File too large (" + st.size + " bytes). Max is " +
                      MAX_FILE_SIZE + " bytes.");
            return;
        }
        try {
            var filedata = readFile(path);
            var b64 = sprintf('%B', filedata);
            var basename = path.replace(/.*\//, '');
            if (topic) {
                topic.broadcast(makeMsg({
                    type: "file",
                    filename: basename,
                    filedata: b64
                }));
            }
            chatPrint("  Sent file: " + basename + " (" + st.size + " bytes)");
        } catch (e) {
            chatPrint("  Error reading file: " + e);
        }
        return;
    }

    // /save [filename]
    if (line === "/save" || line.indexOf("/save ") === 0) {
        if (!lastFile) {
            chatPrint("  No file to save. Wait for someone to /send a file.");
            return;
        }
        var saveName = trim(line.substring(5));
        if (!saveName.length) saveName = lastFile.filename;
        try {
            fwrite(saveName, lastFile.data);
            chatPrint("  Saved: " + saveName + " (" + lastFile.data.length + " bytes)");
        } catch (e) {
            chatPrint("  Error saving file: " + e);
        }
        return;
    }

    // Unknown command
    if (line.charAt(0) === '/' && line.charAt(1) !== '/') {
        chatPrint("  Unknown command. Type /help for a list.");
        return;
    }

    // @name message -> DM
    if (line.charAt(0) === '@') {
        var spaceIdx = line.indexOf(' ');
        if (spaceIdx < 0) {
            chatPrint("  Usage: @name message");
            return;
        }
        var target = line.substring(1, spaceIdx);
        var dmText = line.substring(spaceIdx + 1);
        if (!target.length || !dmText.length) {
            chatPrint("  Usage: @name message");
            return;
        }
        if (topic) topic.broadcast(makeMsg({type: "dm", to: target, text: dmText}));
        chatPrint("\x1b[35m[DM to " + target + "]\x1b[0m " + dmText);
        return;
    }

    // Regular chat message
    if (topic) topic.broadcast(makeMsg({type: "chat", text: line}));
}

// -- Input loop using repl + thread --

function startInput() {
    global.lines = repl(nick + " > ");
    var thr = new rampart.thread();
    inputStarted = true;

    function readstdin() {
        return global.lines.next();
    }

    function writeout(line, err) {
        if (err) {
            fprintf(stderr, "Input error: %s\n", err);
            process.exit(1);
        }
        if (line === null) {
            // EOF (ctrl-d)
            if (topic) topic.broadcast(makeMsg({type: "leave"}));
            printf("\nGoodbye!\n");
            setTimeout(function() { process.exit(0); }, 500);
            return;
        }

        processLine(line);

        // Re-queue next read (setTimeout avoids stack overflow)
        setTimeout(function() {
            thr.exec(readstdin, writeout);
        }, 0);
    }

    thr.exec(readstdin, writeout);
}

// -- Main startup --

printf("Starting iroh chat...\n");
printf("  Nick:  %s\n", nick);
printf("  Topic: %s\n", topicName);
if (joinPeer) printf("  Joining peer: %s\n", shortId(joinPeer));

var ep = new iroh.Endpoint();

ep.on("error", function(err) {
    fprintf(stderr, "Endpoint error: %s\n", err);
});

ep.on("online", function() {
    myNodeId = this.nodeId;
    printf("  Online! Node ID: %s\n", myNodeId);
    if (!joinPeer) {
        printf("\n  Others can join with:\n");
        printf("    rampart iroh_chat.js --nick NAME --join %s\n", myNodeId);
        if (topicName !== "rampart-chat") {
            printf("    (add: --topic %s)\n", topicName);
        }
    }
    printf("\nConnecting to gossip...\n");

    var gossip = new iroh.Gossip(ep);

    gossip.on("error", function(err) {
        fprintf(stderr, "Gossip error: %s\n", err);
    });

    gossip.on("ready", function() {
        printf("Gossip ready. ");
        if (joinPeer) {
            printf("Joining topic '%s' via peer %s...\n", topicName, shortId(joinPeer));
            topic = gossip.subscribe(topicName, [joinPeer]);
        } else {
            printf("Creating topic '%s'...\n", topicName);
            topic = gossip.subscribe(topicName);
        }

        topic.on("error", function(err) {
            chatPrint("Topic error: " + err);
        });

        topic.on("message", onGossipMessage);
        topic.on("peerJoin", onPeerJoin);
        topic.on("peerLeave", onPeerLeave);

        topic.on("ready", function() {
            printf("Connected! Type /help for commands.\n\n");
            // Announce our arrival
            topic.broadcast(makeMsg({type: "join"}));
            // Start interactive input
            startInput();
        });
    });
});
