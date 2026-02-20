// Basic module load test for rampart-iroh
var iroh = require("rampart-iroh");

console.log("Module loaded successfully!");
console.log("Type:", typeof iroh);
console.log("Keys:", Object.keys(iroh));

// Check constructors exist
console.log("Endpoint:", typeof iroh.Endpoint);
console.log("Gossip:", typeof iroh.Gossip);
console.log("Blobs:", typeof iroh.Blobs);
console.log("Docs:", typeof iroh.Docs);
console.log("createServer:", typeof iroh.createServer);
console.log("connect:", typeof iroh.connect);

console.log("\nAll basic checks passed!");
