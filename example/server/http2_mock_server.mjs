import http2 from 'node:http2';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const port = Number(process.env.PORT ?? 8443);
const certPath = process.env.CERT_PATH ?? path.join(__dirname, 'cert.pem');
const keyPath = process.env.KEY_PATH ?? path.join(__dirname, 'key.pem');

const payloadCache = new Map();

function getJsonPayloadBuffer(sizeKb) {
  const clampedKb = Math.max(1, Math.min(2048, sizeKb));
  if (payloadCache.has(clampedKb)) {
    return payloadCache.get(clampedKb);
  }

  const targetBytes = clampedKb * 1024;
  const items = [];
  let index = 0;

  while (true) {
    items.push({
      id: index,
      uuid: `item-${index}-${'x'.repeat(24)}`,
      title: `Benchmark Payload Record #${index}`,
      timestamp: '2026-10-25T00:00:00.000Z',
      metrics: {
        score: (index * 17) % 100,
        active: index % 2 === 0,
        tags: ['http2', 'zero-copy', 'isolate', 'flutter_turbo_fetch'],
      },
      description:
        'High-concurrency HTTP/2 batch reception and zero-copy Isolate JSON decoding benchmark item.',
    });
    index++;
    if (index % 10 === 0) {
      const current = Buffer.byteLength(
        JSON.stringify({ sizeKb: clampedKb, count: items.length, items }),
      );
      if (current >= targetBytes) {
        break;
      }
    }
  }

  const buffer = Buffer.from(
    JSON.stringify({
      sizeKb: clampedKb,
      count: items.length,
      items,
    }),
    'utf8',
  );
  payloadCache.set(clampedKb, buffer);
  return buffer;
}

function handleRequest(req, res) {
  const reqUrl = new URL(req.url ?? '/', `https://localhost:${port}`);
  if (reqUrl.pathname === '/health') {
    res.writeHead(200, { 'content-type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({ status: 'ok', httpVersion: req.httpVersion }));
    return;
  }

  if (reqUrl.pathname === '/redirect') {
    const target = reqUrl.searchParams.get('to') ?? '/health';
    res.writeHead(302, { location: target });
    res.end();
    return;
  }

  const status = Number(reqUrl.searchParams.get('status') ?? 200);
  const sizeKb = Number(reqUrl.searchParams.get('sizeKb') ?? 100);
  const delayMs = Number(reqUrl.searchParams.get('delayMs') ?? 0);
  const omitContentLength = reqUrl.searchParams.get('omitContentLength') === '1';
  const payload = getJsonPayloadBuffer(Number.isFinite(sizeKb) ? sizeKb : 100);

  const send = () => {
    const headers = {
      'content-type': 'application/json; charset=utf-8',
      'x-http-version': req.httpVersion,
    };
    if (!omitContentLength) {
      headers['content-length'] = String(payload.byteLength);
    }
    res.writeHead(Number.isFinite(status) ? status : 200, headers);
    if (omitContentLength && payload.byteLength > 16384) {
      const mid = Math.floor(payload.byteLength / 2);
      res.write(payload.subarray(0, mid));
      res.end(payload.subarray(mid));
    } else {
      res.end(payload);
    }
  };

  if (delayMs > 0) {
    setTimeout(send, delayMs);
  } else {
    send();
  }
}

let server;
if (fs.existsSync(certPath) && fs.existsSync(keyPath)) {
  server = http2.createSecureServer(
    {
      key: fs.readFileSync(keyPath),
      cert: fs.readFileSync(certPath),
      allowHTTP1: true,
    },
    handleRequest,
  );
  console.log(`[HTTP/2 TLS Server] Listening on https://localhost:${port}`);
} else {
  console.warn(
    `[Warning] TLS cert/key not found at ${certPath} / ${keyPath}. Starting cleartext h2c server on http://localhost:${port}`,
  );
  server = http2.createServer(handleRequest);
}

server.listen(port, '0.0.0.0', () => {
  console.log(`Endpoint example: /json?sizeKb=100&delayMs=20`);
  console.log(`Android Emulator URL: https://10.0.2.2:${port}/json?sizeKb=100`);
  console.log(`iOS Simulator URL:    https://localhost:${port}/json?sizeKb=100`);
});
