'use strict';
// The receiving end of Core/SGRemoteLogCore.c: a TCP server that turns the phone's records into events.
//
//   !HELLO <TAB> app <TAB> version <TAB> mod <TAB> device <TAB> os <TAB> pid
//   !LOG <TAB> epoch-ms <TAB> message                       (\n \r \\ escaped)
//   !CRASH-BEGIN <TAB> id <TAB> kind <TAB> summary, raw lines, !CRASH-END <TAB> id
//
// A line that starts with anything else is taken as plain log text, so `nc <pc> 9000` works as a test.
const net = require('node:net');
const { EventEmitter } = require('node:events');

const MAX_LINE = 1024 * 1024;
const MAX_CRASH = 256 * 1024;
const MAX_CLIENTS = 20;

function unescape(text) {
  return text.replace(/\\(n|r|\\)/g, (_, c) => (c === 'n' ? '\n' : c === 'r' ? '\r' : '\\'));
}

class LogServer extends EventEmitter {
  constructor({ port = 9000 } = {}) {
    super();
    this.port = port;
    this.clients = new Map();
    this.nextId = 1;
    this.seenCrashes = new Set();
    this.server = null;
  }

  start() {
    return new Promise((resolve, reject) => {
      const server = net.createServer((socket) => this._accept(socket));
      server.once('error', (error) => {
        this.emit('status', { listening: false, port: this.port, error: error.code || String(error) });
        reject(error);
      });
      server.listen(this.port, '0.0.0.0', () => {
        this.server = server;
        this.port = server.address().port;
        server.on('error', (error) => this.emit('status', { listening: false, port: this.port, error: String(error) }));
        this.emit('status', { listening: true, port: this.port });
        resolve(this.port);
      });
    });
  }

  stop() {
    for (const client of this.clients.values()) client.socket.destroy();
    if (this.server) this.server.close();
    this.server = null;
  }

  _accept(socket) {
    if (this.clients.size >= MAX_CLIENTS) { socket.destroy(); return; }
    const client = {
      id: this.nextId++,
      socket,
      remote: socket.remoteAddress ? socket.remoteAddress.replace(/^::ffff:/, '') : 'unknown',
      info: null,
      buffer: '',
      crash: null,
    };
    this.clients.set(client.id, client);
    socket.setEncoding('utf8');
    socket.setKeepAlive(true, 5000);
    this.emit('client', { clientId: client.id, remote: client.remote, connected: true, info: null });

    socket.on('data', (chunk) => {
      client.buffer += chunk;
      let at;
      while ((at = client.buffer.indexOf('\n')) >= 0) {
        const line = client.buffer.slice(0, at).replace(/\r$/, '');
        client.buffer = client.buffer.slice(at + 1);
        this._line(client, line);
      }
      if (client.buffer.length > MAX_LINE) client.buffer = '';
    });
    const done = () => {
      if (!this.clients.has(client.id)) return;
      this.clients.delete(client.id);
      if (client.buffer) this._line(client, client.buffer);
      // A report cut off by the connection is still worth showing.
      if (client.crash) this._finishCrash(client, true);
      this.emit('client', { clientId: client.id, remote: client.remote, connected: false, info: client.info });
    };
    socket.on('close', done);
    socket.on('error', () => socket.destroy());
  }

  _line(client, line) {
    if (client.crash) {
      if (line === `!CRASH-END\t${client.crash.id}`) { this._finishCrash(client, false); return; }
      if (client.crash.size < MAX_CRASH) { client.crash.lines.push(line); client.crash.size += line.length + 1; }
      return;
    }
    if (!line.startsWith('!')) {
      if (line.length) this.emit('log', { clientId: client.id, ts: Date.now(), text: line });
      return;
    }
    const f = line.split('\t');
    switch (f[0]) {
      case '!HELLO':
        client.info = { app: f[1], version: f[2], mod: f[3], device: f[4], os: f[5], pid: f[6] };
        this.emit('client', { clientId: client.id, remote: client.remote, connected: true, info: client.info });
        break;
      case '!LOG':
        this.emit('log', { clientId: client.id, ts: Number(f[1]) || Date.now(), text: unescape(f.slice(2).join('\t')) });
        break;
      case '!CRASH-BEGIN':
        client.crash = { id: f[1] || String(Date.now()), kind: f[2] || 'crash', summary: f.slice(3).join('\t'), lines: [], size: 0 };
        break;
      default:
        this.emit('log', { clientId: client.id, ts: Date.now(), text: line });
    }
  }

  _finishCrash(client, truncated) {
    const crash = client.crash;
    client.crash = null;
    // The same report can arrive twice: live from the handler, then from the file kept for the next launch.
    if (this.seenCrashes.has(crash.id)) return;
    this.seenCrashes.add(crash.id);
    const seconds = Number(String(crash.id).split('-')[0]);
    this.emit('crash', {
      clientId: client.id,
      id: crash.id,
      kind: crash.kind,
      summary: crash.summary,
      body: crash.lines.join('\n') + (truncated ? '\n(the report was cut off: the connection closed)' : ''),
      ts: seconds > 1e9 ? seconds * 1000 : Date.now(),
      truncated,
    });
  }
}

module.exports = { LogServer, unescape };
