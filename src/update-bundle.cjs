const https = require('node:https');
const HEADER_SIZE = 16;
const DATA_START = 4 * 1024 * 1024;
const MAGIC = Buffer.from('QRAZYUP2');
function headerLength(bytes) {
  if (bytes.length !== HEADER_SIZE || !bytes.subarray(0, 8).equals(MAGIC) || bytes.readUInt32BE(12) !== 0) throw new Error('Invalid update package header');
  const length = bytes.readUInt32BE(8);
  if (length < 1 || length > DATA_START - HEADER_SIZE) throw new Error('Invalid update package index size');
  return length;
}
function rangeRequest(url, start, end, progress = () => {}, timeout = 120000) {
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start < 0 || end < start) throw new Error('Invalid byte range');
  const expected = end - start + 1;
  return new Promise((resolve, reject) => {
    let active, finished = false;
    const timer = setTimeout(() => fail(new Error('Update download timed out')), timeout);
    function fail(error) { if (finished) return; finished = true; clearTimeout(timer); active?.destroy(); reject(error); }
    function visit(value, count) {
      const target = new URL(value);
      if (count > 5 || target.protocol !== 'https:' || target.username || target.password || !['github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com'].includes(target.hostname)) return fail(new Error('Untrusted update download redirect'));
      // GitHub redirects first. Send Range only to its asset server; suffix
      // ranges and ranges on the redirect endpoint are not consistently supported.
      const ranged = target.hostname !== 'github.com';
      active = https.get(target, { headers: { 'User-Agent': 'Qrazy', ...(ranged ? { Range: `bytes=${start}-${end}` } : {}) } }, response => {
        if ([301, 302, 303, 307, 308].includes(response.statusCode)) {
          response.destroy();
          try { visit(new URL(response.headers.location, target).href, count + 1); } catch (error) { fail(error); }
          return;
        }
        if (response.statusCode !== 206) { const error = new Error(`Update server did not support partial downloading (HTTP ${response.statusCode})`); error.status = response.statusCode; response.destroy(); return fail(error); }
        const match = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(response.headers['content-range'] || '');
        const total = match ? Number(match[3]) : 0;
        if (!match || Number(match[1]) !== start || Number(match[2]) !== end || !Number.isSafeInteger(total) || total <= end || (response.headers['content-encoding'] && response.headers['content-encoding'] !== 'identity') || (response.headers['content-length'] && Number(response.headers['content-length']) !== expected)) { response.destroy(); return fail(new Error('Invalid partial download response')); }
        let size = 0; const chunks = [];
        response.on('data', chunk => { size += chunk.length; if (size > expected) { response.destroy(); return fail(new Error('Partial download exceeded its declared size')); } chunks.push(chunk); progress(size); });
        response.on('error', fail);
        response.on('aborted', () => fail(new Error('Partial download was interrupted')));
        response.on('end', () => { if (finished) return; if (size !== expected) return fail(new Error('Partial download was incomplete')); finished = true; clearTimeout(timer); resolve({ bytes: Buffer.concat(chunks), total }); });
      });
      active.on('error', fail);
    }
    try { visit(url, 0); } catch (error) { fail(error); }
  });
}
async function readBundleManifest(url, request = rangeRequest) {
  const header = await request(url, 0, HEADER_SIZE - 1, undefined, 15000);
  const length = headerLength(header.bytes);
  const index = await request(url, HEADER_SIZE, HEADER_SIZE + length - 1, undefined, 15000);
  if (index.total !== header.total || header.total <= DATA_START) throw new Error('Update package changed during download');
  return { envelope: JSON.parse(index.bytes.toString('utf8')), total: header.total };
}
module.exports = { HEADER_SIZE, DATA_START, MAGIC, headerLength, rangeRequest, readBundleManifest };
