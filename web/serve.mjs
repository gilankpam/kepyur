// Static server for the built site (default: build/web/site) on http://localhost:8080 with the cross-origin
// isolation headers the WebAssembly threads need (on GitHub Pages coi-serviceworker.js adds them instead).
// localhost counts as a secure context, which WebUSB requires.
//   node web/serve.mjs [port] [directory]
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const port = Number(process.argv[2] ?? 8080);
const root = process.argv[3] ? resolve(process.argv[3]) : join(fileURLToPath(new URL('..', import.meta.url)), 'build', 'web', 'site');
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css', '.svg': 'image/svg+xml', '.png': 'image/png' };

createServer(async (req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^(\.\.[/\\])+/, '');
  const file = join(root, path.endsWith('/') ? path + 'index.html' : path);
  if (!file.startsWith(root)) return res.writeHead(403).end();
  try {
    const body = await readFile(file);
    res.writeHead(200, {
      'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(body);
  } catch {
    res.writeHead(404).end('not found');
  }
}).listen(port, '127.0.0.1', () => console.log(`http://localhost:${port}/`));
