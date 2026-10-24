'use strict';
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('taurus', {
  onEvent: (callback) => ipcRenderer.on('event', (_e, event) => callback(event)),
  ready: () => ipcRenderer.invoke('ready'),
  windowAction: (action) => ipcRenderer.send('window', action),
  saveLog: (text) => ipcRenderer.invoke('save-log', text),
  openLogFolder: () => ipcRenderer.send('open-log-folder'),
});
