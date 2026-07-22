import { readFile, writeFile } from "node:fs/promises";

const [source, destination] = process.argv.slice(2);
if (!source || !destination) {
    throw new Error("Usage: node make-icon.mjs <source.png> <destination.ico>");
}

const png = await readFile(source);
const header = Buffer.alloc(22);

header.writeUInt16LE(0, 0);
header.writeUInt16LE(1, 2);
header.writeUInt16LE(1, 4);
header.writeUInt8(96, 6);
header.writeUInt8(96, 7);
header.writeUInt8(0, 8);
header.writeUInt8(0, 9);
header.writeUInt16LE(1, 10);
header.writeUInt16LE(32, 12);
header.writeUInt32LE(png.length, 14);
header.writeUInt32LE(header.length, 18);

await writeFile(destination, Buffer.concat([header, png]));
