'use strict';
// Announces the console on the local network (_taurus-log._tcp) so an IPLOG build with no fixed host
// finds it, and lists this machine's addresses for builds that pin IPLOG_HOST.
const os = require('node:os');

function localAddresses() {
  const out = [];
  for (const [name, list] of Object.entries(os.networkInterfaces())) {
    for (const a of list || []) {
      if (a.family === 'IPv4' && !a.internal) out.push({ name, address: a.address });
    }
  }
  // Wi-Fi and Ethernet first, virtual adapters (docker, vmware, vbox, tailscale) last.
  const virtual = /docker|veth|vmnet|vbox|virtual|vethernet|utun|tailscale|zerotier|br-|lo/i;
  return out.sort((x, y) => Number(virtual.test(x.name)) - Number(virtual.test(y.name)));
}

function publish(port) {
  try {
    const { Bonjour } = require('bonjour-service');
    const bonjour = new Bonjour();
    const service = bonjour.publish({ name: `Taurus Log Console (${os.hostname()})`, type: 'taurus-log', protocol: 'tcp', port });
    return { stop: () => { try { service.stop?.(); bonjour.destroy(); } catch { /* shutting down */ } } };
  } catch (error) {
    return { stop() {}, error: String(error) };
  }
}

module.exports = { localAddresses, publish };
