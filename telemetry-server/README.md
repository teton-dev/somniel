# Somniel telemetry server

Receives opt-in, anonymous performance and crash reports from the app. Python standard library + SQLite, no pip.

## Privacy contract (keep the app's consent text in sync with this)
Sent: random install id, app version, device model, SoC/GPU names, Android version, RAM, frame-rate totals,
crash frames as library+offset. Never: names, accounts, IPs, paths, saves, game data.
