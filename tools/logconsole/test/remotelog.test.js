'use strict';
// Runs the phone's C client (harness/remotelog) against the real server: logs, the three kinds of
// crash, a report kept on disk while the console is away, and the same report arriving twice.
const test = require('node:test');
const assert = require('node:assert');
const { spawnSync, spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { LogServer } = require('../src/server');

const root = path.join(__dirname, '..', '..', '..');
const build = path.join(root, 'harness', 'remotelog', 'build.sh');
const built = spawnSync('bash', [build], { encoding: 'utf8' });
const bin = built.status === 0 ? built.stdout.trim().split('\n').pop() : null;
const skip = bin ? false : 'no C compiler';

function collect(server) {
  const events = { logs: [], crashes: [], clients: [] };
  server.on('log', (l) => events.logs.push(l));
  server.on('crash', (c) => events.crashes.push(c));
  server.on('client', (c) => events.clients.push(c));
  return events;
}
function run(port, dir, mode) {
  return new Promise((resolve) => {
    const child = spawn(bin, ['127.0.0.1', String(port), path.join(dir, 'crash.txt'), path.join(dir, 'state'), mode], { stdio: 'ignore' });
    child.on('close', (code, signal) => resolve({ code, signal }));
  });
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

test('logs arrive, escaped newlines restored', { skip }, async () => {
  const server = new LogServer({ port: 0 });
  const ev = collect(server);
  await server.start();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'iplog-'));
  const r = await run(server.port, dir, 'exit');
  await wait(200);
  assert.strictEqual(r.code, 0);
  const texts = ev.logs.map((l) => l.text);
  assert.ok(texts.includes('first line'), texts.join('|'));
  assert.ok(texts.includes('multi\nline \\ message'));
  assert.ok(texts.includes('error: something failed'));
  const hello = ev.clients.find((c) => c.info);
  assert.strictEqual(hello.info.device, 'iPhone-test');
  assert.strictEqual(hello.info.mod, '0.0.1-test');
  assert.strictEqual(ev.crashes.length, 0);
  assert.strictEqual(fs.readFileSync(path.join(dir, 'state'), 'utf8'), 'foreground');
  server.stop();
});

for (const [mode, signalName] of [['segv', 'SIGSEGV'], ['abort', 'SIGABRT'], ['trap', 'SIGILL'], ['nullcall', 'SIGSEGV']]) {
  test(`${mode} is reported as a crash and the process still dies of it`, { skip }, async () => {
    const server = new LogServer({ port: 0 });
    const ev = collect(server);
    await server.start();
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'iplog-'));
    const r = await run(server.port, dir, mode);
    await wait(300);
    assert.strictEqual(r.signal, mode === 'trap' ? r.signal : signalName, `process ended with ${JSON.stringify(r)}`);
    assert.strictEqual(ev.crashes.length, 1);
    const crash = ev.crashes[0];
    assert.match(crash.summary, new RegExp(`^SIG`));
    assert.match(crash.body, /backtrace \(\d+ frames\)/);
    assert.match(crash.body, /about to crash/);          // the last log lines ride along
    assert.strictEqual(crash.truncated, false);
    if (mode === 'segv') assert.match(crash.body, /fault address: 0x0000000000000010/);
    assert.strictEqual(fs.readFileSync(path.join(dir, 'state'), 'utf8'), 'crash');
    server.stop();
  });
}

test('a crash while the console is away is delivered at the next connection, once', { skip }, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'iplog-'));
  // Nobody listens on this port yet.
  const probe = new LogServer({ port: 0 });
  await probe.start();
  const port = probe.port;
  probe.stop();
  await wait(100);
  const r = await run(port, dir, 'segv');
  assert.strictEqual(r.signal, 'SIGSEGV');
  const file = path.join(dir, 'crash.txt');
  assert.ok(fs.existsSync(file) && fs.statSync(file).size > 0, 'report kept on disk');

  const server = new LogServer({ port });
  const ev = collect(server);
  await server.start();
  // A later launch connects, which sends the file; it stays quiet itself.
  const again = await run(port, dir, 'exit');
  await wait(300);
  assert.strictEqual(again.code, 0);
  assert.strictEqual(ev.crashes.length, 1);
  assert.ok(!fs.existsSync(file), 'file removed once sent');
  server.stop();
});

test('the same report twice is shown once; a cut-off one is flagged', async () => {
  const server = new LogServer({ port: 0 });
  const ev = collect(server);
  await server.start();
  const net = require('node:net');
  const block = '!CRASH-BEGIN\t170000-1\texception\tNSRangeException: boom\nline a\n!CRASH-END\t170000-1\n';
  await new Promise((resolve) => {
    const s = net.connect(server.port, '127.0.0.1', () => { s.write(block + block + '!CRASH-BEGIN\t170001-1\tsignal\tSIGSEGV (11)\nhalf'); s.end(); });
    s.on('close', resolve);
  });
  await wait(100);
  assert.strictEqual(ev.crashes.length, 2);
  assert.strictEqual(ev.crashes[0].summary, 'NSRangeException: boom');
  assert.strictEqual(ev.crashes[0].body, 'line a');
  assert.strictEqual(ev.crashes[1].truncated, true);
  server.stop();
});

test('a report about the previous run does not mute this run\'s own crash', { skip }, async () => {
  const server = new LogServer({ port: 0 });
  const ev = collect(server);
  await server.start();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'iplog-'));
  const r = await run(server.port, dir, 'report-then-segv');
  await wait(300);
  assert.strictEqual(r.signal, 'SIGSEGV');
  assert.deepStrictEqual(ev.crashes.map((c) => c.kind).sort(), ['abnormal-exit', 'signal']);
  server.stop();
});
