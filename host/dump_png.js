// Convert a raw RGB565 framebuffer (little-endian uint16, row-major) to a PNG.
// Usage: node dump_png.js <in.rgb565> <out.png> <width> <height>
const fs = require('fs');
const zlib = require('zlib');

const [, , inPath, outPath, wStr, hStr] = process.argv;
const W = parseInt(wStr, 10), H = parseInt(hStr, 10);
const raw = fs.readFileSync(inPath);

// RGB565 -> 8-bit RGB, packed into filtered scanlines (filter byte 0 per row).
const stride = 1 + W * 3;
const rows = Buffer.alloc(stride * H);
for (let y = 0; y < H; y++) {
  rows[y * stride] = 0; // filter: none
  for (let x = 0; x < W; x++) {
    const v = raw.readUInt16LE((y * W + x) * 2);
    const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
    const o = y * stride + 1 + x * 3;
    rows[o]     = (r5 << 3) | (r5 >> 2);
    rows[o + 1] = (g6 << 2) | (g6 >> 4);
    rows[o + 2] = (b5 << 3) | (b5 >> 2);
  }
}

// ---- Minimal PNG writer ----
const crcTable = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return ~c;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, 'ascii');
  const body = Buffer.concat([typeBuf, data]);
  const crc = Buffer.alloc(4); crc.writeInt32BE(crc32(body), 0);
  return Buffer.concat([len, body, crc]);
}
const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; // 8-bit truecolor
const idat = zlib.deflateSync(rows, { level: 9 });
const png = Buffer.concat([sig, chunk('IHDR', ihdr), chunk('IDAT', idat), chunk('IEND', Buffer.alloc(0))]);
fs.writeFileSync(outPath, png);
console.log(`[dump_png] wrote ${outPath} (${W}x${H})`);
