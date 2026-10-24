# Taurus Log Console

A Windows XP styled window (Electron: the same app on Windows and Mac) that an **IPLOG** build of Taurus
sends its log lines and crashes to. Crashes show in red as `[CRASH]` with the signal or exception, a
backtrace and the last log lines.

## Get it

- **GitHub**: Actions → *Log Console* → Run workflow. Download `TaurusLogConsole-windows` (one portable
  `.exe`) or `TaurusLogConsole-mac` (`.dmg`, Intel and Apple silicon).
- **From source**: `make console` at the repo root, or `npm install && npm start` here.

Neither is signed. Windows SmartScreen: *More info → Run anyway*. macOS: right-click the app → *Open*
(or `xattr -cr "/Applications/Taurus Log Console.app"`).

## Use it

1. Open it on a computer on the same Wi-Fi as the phone; allow the firewall prompt (private networks).
   It listens on TCP 9000 (`TAURUS_LOG_PORT` changes it) and announces itself as `_taurus-log._tcp`.
2. Build with the flag: `make install IPLOG=1`, or *Build IPA* on GitHub with **iplog** ticked. Add
   `IPLOG_HOST=<this PC's address>` to skip discovery (the window lists its addresses).
3. Open Spotify and allow Local Network once. Lines appear as they are logged.

Every session is also written to `Documents/TaurusLogs/`.
