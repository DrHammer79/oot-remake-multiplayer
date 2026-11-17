// The crash relay's rules (src/relay.js) under Node: node --test tools/crash-relay/tests/relay.test.js (CI: build.yml,
// job linux). No network: Discord is a fake fetch, the Durable Object's storage a Map.
import test from 'node:test';
import assert from 'node:assert/strict';
import {
    checkRequest, looksLikeZip, readCapped, Limiter, handle, forward,
    MAX_BYTES, PER_IP_PER_HOUR, GLOBAL_PER_HOUR, WINDOW_MS,
} from '../src/relay.js';

const WEBHOOK = 'https://discord.com/api/webhooks/123456789/abc_DEF-ghi';
const ENV = { DISCORD_WEBHOOK: WEBHOOK };

// A real one-file stored zip ('a.txt' = 'hi'), as the kits' writers make (PK 3 4 ... PK 1 2 ... PK 5 6).
function zipBytes(comment = '') {
    const name = new TextEncoder().encode('a.txt');
    const data = new TextEncoder().encode('hi');
    const crc = 0xd8932aac;   // (CRC-32 of 'hi')
    const u16 = (v) => [v & 0xff, (v >> 8) & 0xff];
    const u32 = (v) => [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff];
    const local = [...u32(0x04034b50), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0), ...u32(crc),
        ...u32(data.length), ...u32(data.length), ...u16(name.length), ...u16(0), ...name, ...data];
    const central = [...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
        ...u32(crc), ...u32(data.length), ...u32(data.length), ...u16(name.length), ...u16(0), ...u16(0), ...u16(0),
        ...u16(0), ...u32(0), ...u32(0), ...name];
    const c = new TextEncoder().encode(comment);
    const end = [...u32(0x06054b50), ...u16(0), ...u16(0), ...u16(1), ...u16(1), ...u32(central.length),
        ...u32(local.length), ...u16(c.length), ...c];
    return new Uint8Array([...local, ...central, ...end]);
}

function post(body, headers = {}, path = '/report', method = 'POST') {
    const h = { 'Content-Type': 'application/zip', 'X-Squadala-Kind': 'crash', 'X-Squadala-Kit': 'v0.11.4',
        'CF-Connecting-IP': '203.0.113.7', ...headers };
    for (const k of Object.keys(h)) if (h[k] === null) delete h[k];
    return new Request('https://relay.example' + path, { method, headers: h, body: method === 'GET' ? undefined : body });
}

class FakeStorage {
    constructor() { this.map = new Map(); this.alarm = null; }
    async get(k) { const v = this.map.get(k); return v === undefined ? undefined : structuredClone(v); }
    async put(k, v) { this.map.set(k, structuredClone(v)); }
    async setAlarm(t) { this.alarm = t; }
    async deleteAll() { this.map.clear(); }
}

function fakeDiscord(ok = true) {
    const calls = [];
    const fetchFn = async (url, init) => { calls.push({ url, init }); return new Response('', { status: ok ? 200 : 500 }); };
    return { calls, fetchFn };
}

function setup({ ok = true, now } = {}) {
    const storage = new FakeStorage();
    let t = 1_000_000;
    const clock = now || (() => t);
    const limiter = new Limiter(storage, { now: clock });
    const discord = fakeDiscord(ok);
    return { storage, limiter, discord, advance: (ms) => { t += ms; }, deps: { limiter, fetchFn: discord.fetchFn } };
}

test('a zip from a kit is passed on to the webhook with its kind and version, no one mentioned', async () => {
    const { deps, discord } = setup();
    const res = await handle(post(zipBytes()), ENV, deps);
    assert.equal(res.status, 202);
    assert.equal(discord.calls.length, 1);
    assert.equal(discord.calls[0].url, WEBHOOK);
    const form = discord.calls[0].init.body;
    const payload = JSON.parse(form.get('payload_json'));
    assert.equal(payload.content, 'Squadala! crash, kit v0.11.4');
    assert.deepEqual(payload.allowed_mentions, { parse: [] });
    const file = form.get('files[0]');
    assert.match(file.name, /^squadala-crash-\d{4}-\d\d-\d\dT\d\d-\d\d-\d\d\.zip$/);
    assert.deepEqual(new Uint8Array(await file.arrayBuffer()), zipBytes());
});

test('each kind has its own text', async () => {
    for (const [kind, text] of [['unclean', 'game ended without closing'], ['problem', 'problem report']]) {
        const { deps, discord } = setup();
        assert.equal((await handle(post(zipBytes(), { 'X-Squadala-Kind': kind }), ENV, deps)).status, 202);
        assert.equal(JSON.parse(discord.calls[0].init.body.get('payload_json')).content, `Squadala! ${text}, kit v0.11.4`);
    }
});

test('anything else is refused before Discord is called', async () => {
    const cases = [
        [post(zipBytes(), {}, '/'), 404],
        [post(zipBytes(), {}, '/report/x'), 404],
        [post(null, {}, '/report', 'GET'), 405],
        [post(zipBytes(), { 'Content-Type': 'text/plain' }), 415],
        [post(zipBytes(), { 'Content-Type': null }), 415],
        [post(zipBytes(), { 'X-Squadala-Kind': 'other' }), 400],
        [post(zipBytes(), { 'X-Squadala-Kind': null }), 400],
        [post(zipBytes(), { 'X-Squadala-Kit': '@everyone' }), 400],
        [post(zipBytes(), { 'X-Squadala-Kit': null }), 400],
        [post(zipBytes(), { 'Content-Length': String(MAX_BYTES + 1) }), 413],
        [post(new TextEncoder().encode('not a zip at all, but long enough to be one if it were one........')), 400],
        [post(zipBytes().slice(0, 40)), 400],
    ];
    for (const [req, status] of cases) {
        const { deps, discord } = setup();
        const res = await handle(req, ENV, deps);
        assert.equal(res.status, status, `${req.method} ${req.url} ${[...req.headers].join(' ')}`);
        assert.equal(discord.calls.length, 0);
    }
});

test('a version is a version: v0.11.4, 0.12.0-rc.1 pass; text, mentions and links do not', () => {
    const ok = (v) => !checkRequest(post(zipBytes(), { 'X-Squadala-Kit': v })).error;
    assert.ok(ok('v0.11.4'));
    assert.ok(ok('0.12.0-rc.1'));
    assert.ok(ok('v0.11.4+dev.3'));
    for (const v of ['', 'v0.11.4 @here', 'https://x.y', 'v1', '<@123>', 'v0.11.4\nhi']) assert.ok(!ok(v), v);
});

test('a body larger than the cap is refused even without (or with a wrong) Content-Length', async () => {
    const big = new Uint8Array(MAX_BYTES + 10);
    big.set(zipBytes(), 0);
    const { deps, discord } = setup();
    const res = await handle(post(big, { 'Content-Length': null }), ENV, deps);
    assert.equal(res.status, 413);
    assert.equal(discord.calls.length, 0);
    assert.equal(await readCapped(post(new Uint8Array(11)), 10), null);
    assert.equal((await readCapped(post(new Uint8Array(10)), 10)).length, 10);
});

test('zip check: local header first, end record ending the file, at least one entry', () => {
    assert.ok(looksLikeZip(zipBytes()));
    assert.ok(looksLikeZip(zipBytes('a comment')));
    const z = zipBytes();
    assert.ok(!looksLikeZip(new Uint8Array([...z, 0, 0])));                 // (bytes after the end record)
    const empty = new Uint8Array(60); empty.set([0x50, 0x4b, 0x05, 0x06], 0);
    assert.ok(!looksLikeZip(empty));                                          // (an empty zip starts with its end record)
    const gz = new Uint8Array(z); gz[2] = 0x07;
    assert.ok(!looksLikeZip(gz));
    assert.ok(!looksLikeZip(new Uint8Array(10)));
});

test('no webhook secret (or not a Discord webhook): 503, nothing sent', async () => {
    for (const env of [{}, { DISCORD_WEBHOOK: '' }, { DISCORD_WEBHOOK: 'https://evil.example/api/webhooks/1/x' }]) {
        const { deps, discord } = setup();
        assert.equal((await handle(post(zipBytes()), env, deps)).status, 503);
        assert.equal(discord.calls.length, 0);
    }
});

test('Discord failing gives 502', async () => {
    const { deps } = setup({ ok: false });
    assert.equal((await handle(post(zipBytes()), ENV, deps)).status, 502);
    const throwing = { ...deps, fetchFn: async () => { throw new Error('down'); } };
    assert.equal((await handle(post(zipBytes()), ENV, throwing)).status, 502);
});

test(`rate limit: ${PER_IP_PER_HOUR} an hour from one address, then 429 with Retry-After; a new hour allows again`, async () => {
    const { deps, discord, advance } = setup();
    for (let i = 0; i < PER_IP_PER_HOUR; i++) assert.equal((await handle(post(zipBytes()), ENV, deps)).status, 202);
    const limited = await handle(post(zipBytes()), ENV, deps);
    assert.equal(limited.status, 429);
    assert.ok(Number(limited.headers.get('Retry-After')) > 0);
    assert.equal(discord.calls.length, PER_IP_PER_HOUR);
    assert.equal((await handle(post(zipBytes(), { 'CF-Connecting-IP': '198.51.100.9' }), ENV, deps)).status, 202);
    advance(WINDOW_MS);
    assert.equal((await handle(post(zipBytes()), ENV, deps)).status, 202);
});

test(`rate limit: ${GLOBAL_PER_HOUR} an hour in all`, async () => {
    const { deps } = setup();
    for (let i = 0; i < GLOBAL_PER_HOUR; i++) {
        assert.equal((await handle(post(zipBytes(), { 'CF-Connecting-IP': `192.0.2.${i}` }), ENV, deps)).status, 202);
    }
    assert.equal((await handle(post(zipBytes(), { 'CF-Connecting-IP': '192.0.2.250' }), ENV, deps)).status, 429);
});

test('refused requests (wrong path, type, kind) do not use up the limit; a bad zip does', async () => {
    const { deps, storage } = setup();
    await handle(post(zipBytes(), { 'Content-Type': 'text/plain' }), ENV, deps);
    assert.equal(storage.map.size, 0);
    await handle(post(new Uint8Array(100)), ENV, deps);
    assert.equal((await storage.get('window')).total, 1);
});

test('no address is stored: only salted hashes, and a new window has a new salt and none of the old hashes', async () => {
    const { deps, storage, advance } = setup();
    await handle(post(zipBytes()), ENV, deps);
    const first = await storage.get('window');
    const text = JSON.stringify(first);
    assert.ok(!text.includes('203.0.113.7'));
    assert.equal(Object.keys(first.ips).length, 1);
    assert.match(Object.keys(first.ips)[0], /^[0-9a-f]{24}$/);
    assert.equal(storage.alarm, first.start + WINDOW_MS);   // (worker.js's alarm deletes it all at the window's end)
    advance(WINDOW_MS);
    await handle(post(zipBytes(), { 'CF-Connecting-IP': '198.51.100.9' }), ENV, deps);
    const second = await storage.get('window');
    assert.notEqual(second.salt, first.salt);
    assert.deepEqual(Object.keys(second.ips).filter((k) => k in first.ips), []);
    assert.equal(second.total, 1);
});

test('forward builds one multipart message', async () => {
    const { calls, fetchFn } = fakeDiscord();
    assert.ok(await forward(WEBHOOK, zipBytes(), 'problem', 'v0.11.4', { fetchFn, now: () => new Date('2026-11-17T01:02:03Z') }));
    assert.equal(calls[0].init.method, 'POST');
    assert.equal(calls[0].init.body.get('files[0]').name, 'squadala-problem-2026-11-17T01-02-03.zip');
});
