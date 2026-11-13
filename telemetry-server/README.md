# Somniel telemetry server

Receives opt-in, anonymous performance and crash reports from the app. Python standard library + SQLite, no pip.

## Setup (once, on the always-on Windows PC)
1. Copy the `telemetry-server` folder to that PC (anywhere).
2. Double-click `setup.bat` (it asks for administrator rights).
3. Choose the tunnel: **1 = Cloudflare Tunnel** (needs a domain on Cloudflare) or **2 = Tailscale Funnel** (no domain).
4. It prints the dashboard address and login. It also writes the address and key into
   `app\src\main\res\values\telemetry.xml` (if the folder is inside the project; otherwise copy the two values by hand).
5. Rebuild the app. A build with an empty address has no telemetry prompt at all.

Server and tunnel start by themselves at boot (Task Scheduler, SYSTEM). `setup.bat -Uninstall` removes the tasks.

## Data
`data\telemetry.db` (SQLite). Dashboard: `<address>/dashboard` (HTTP login from `config.json`).
Client IPs are never stored or logged. Back up the `data` folder if you care about history.

## Privacy contract (keep the app's consent text in sync with this)
Sent: random install id, app version, device model, SoC/GPU names, Android version, RAM, frame-rate totals,
crash frames as library+offset. Never: names, accounts, IPs, paths, saves, game data.
