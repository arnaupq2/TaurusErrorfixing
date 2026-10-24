'use strict';
const { app, BrowserWindow, dialog, ipcMain, shell } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { LogServer } = require('./server');
const { localAddresses, publish } = require('./discovery');

const PORT = Number(process.env.TAURUS_LOG_PORT) || 9000;
let win = null;
let server = null;
let announce = null;
let status = { listening: false, port: PORT };
let sessionFile = null;
const devices = new Map();   // clientId -> { clientId, remote, connected, info }

function logDir() { return path.join(app.getPath('documents'), 'TaurusLogs'); }

function record(line) {
  try {
    if (!sessionFile) {
      fs.mkdirSync(logDir(), { recursive: true });
      const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
      sessionFile = path.join(logDir(), `taurus-${stamp}.log`);
    }
    fs.appendFileSync(sessionFile, line + '\n');
  } catch { /* the console keeps working without a file */ }
}

function send(event) {
  if (win && !win.isDestroyed()) win.webContents.send('event', event);
}

function stamp(ts) { return new Date(ts).toISOString().replace('T', ' ').replace('Z', ''); }

function startServer() {
  server = new LogServer({ port: PORT });
  server.on('status', (s) => { status = s; send({ type: 'status', ...s, addresses: localAddresses() }); });
  server.on('client', (c) => {
    devices.set(c.clientId, c);
    send({ type: 'client', ...c });
    record(`# ${stamp(Date.now())} ${c.connected ? 'connected' : 'disconnected'} ${c.remote} ${c.info ? c.info.device + ' iOS ' + c.info.os : ''}`);
  });
  server.on('log', (l) => { send({ type: 'log', ...l }); record(`${stamp(l.ts)} ${l.text.replace(/\n/g, '\n    ')}`); });
  server.on('crash', (c) => {
    send({ type: 'crash', ...c });
    if (win && !win.isDestroyed() && !win.isFocused()) win.flashFrame(true);
    record(`${stamp(c.ts)} [CRASH] ${c.kind}: ${c.summary}\n${c.body.replace(/^/gm, '    ')}`);
  });
  server.start().then((port) => { announce = publish(port); }).catch(() => { /* status event already sent */ });
}

function createWindow() {
  win = new BrowserWindow({
    width: 980,
    height: 640,
    minWidth: 640,
    minHeight: 420,
    frame: false,
    backgroundColor: '#0831d9',
    title: 'Taurus Log Console',
    icon: path.join(__dirname, '..', 'build', 'icon.png'),
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, nodeIntegration: false, sandbox: true },
  });
  win.setMenuBarVisibility(false);
  win.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', (e) => e.preventDefault());
  const notifyMax = () => send({ type: 'maximized', value: win.isMaximized() });
  win.on('focus', () => win.flashFrame(false));
  win.on('maximize', notifyMax);
  win.on('unmaximize', notifyMax);
}

ipcMain.handle('ready', () => ({ status: { ...status, addresses: localAddresses() }, devices: [...devices.values()], logDir: logDir() }));
ipcMain.on('window', (_e, action) => {
  if (!win) return;
  if (action === 'minimize') win.minimize();
  else if (action === 'maximize') (win.isMaximized() ? win.unmaximize() : win.maximize());
  else if (action === 'close') win.close();
});
ipcMain.handle('save-log', async (_e, text) => {
  const result = await dialog.showSaveDialog(win, { defaultPath: path.join(logDir(), 'taurus-export.log'), filters: [{ name: 'Log', extensions: ['log', 'txt'] }] });
  if (result.canceled || !result.filePath) return false;
  fs.writeFileSync(result.filePath, text);
  return true;
});
ipcMain.on('open-log-folder', () => { fs.mkdirSync(logDir(), { recursive: true }); shell.openPath(logDir()); });

const lock = app.requestSingleInstanceLock();
if (!lock) {
  app.quit();
} else {
  app.on('second-instance', () => { if (win) { if (win.isMinimized()) win.restore(); win.focus(); } });
  app.whenReady().then(() => { startServer(); createWindow(); });
  app.on('window-all-closed', () => app.quit());
  app.on('before-quit', () => { if (announce) announce.stop(); if (server) server.stop(); });
}
