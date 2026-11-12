# Somniel telemetry server: one-time setup for a Windows PC that stays on.
#   1. installs Python (if missing) and the tunnel client (cloudflared or Tailscale)
#   2. creates config.json (random ingest key + dashboard password)
#   3. publishes the server to the internet through a tunnel (your home IP is never exposed, no router ports)
#   4. registers Windows startup tasks so everything runs at boot, with no login needed
#   5. writes the server address and key into the Android app (app\src\main\res\values\telemetry.xml)
# Run it by double-clicking setup.bat. Safe to run again.
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfgPath = Join-Path $here 'config.json'
$xmlPath = Join-Path $here '..\app\src\main\res\values\telemetry.xml'
$taskServer = 'SomnielTelemetryServer'
$taskTunnel = 'SomnielTelemetryTunnel'

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }

function Find-Exe($name, $globs) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($g in $globs) {
        $f = Get-ChildItem $g -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if ($f) { return $f.FullName }
    }
    return $null
}

function Ensure-Winget($id, $extra) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget is not available. Install 'App Installer' from the Microsoft Store, then run this again."
    }
    winget install -e --id $id --accept-source-agreements --accept-package-agreements @extra
}

function Register-Startup($name, $exe, $argLine) {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    $act = New-ScheduledTaskAction -Execute $exe -Argument $argLine -WorkingDirectory $here
    $trg = New-ScheduledTaskTrigger -AtStartup
    $set = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $pri = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $name -Action $act -Trigger $trg -Settings $set -Principal $pri | Out-Null
    Start-ScheduledTask -TaskName $name
}

if ($Uninstall) {
    Step 'Removing startup tasks'
    foreach ($t in @($taskServer, $taskTunnel)) { Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue }
    Write-Host 'Done. The data folder and config.json were left alone. For a tunnel: delete it in the Cloudflare dashboard (Zero Trust > Networks > Tunnels), or run "tailscale funnel reset".'
    return
}

# ---- 1. Python ----
Step 'Python'
$python = Find-Exe 'python' @('C:\Program Files\Python3*\python.exe')
if ($python -and $python -like '*WindowsApps*') { $python = $null } # the Store stub, not a real install
if (-not $python) {
    $python = Find-Exe 'nothing' @('C:\Program Files\Python3*\python.exe')
}
if (-not $python) {
    Write-Host 'Installing Python 3.12 (machine-wide) with winget...'
    Ensure-Winget 'Python.Python.3.12' @('--scope', 'machine')
    $python = Find-Exe 'nothing' @('C:\Program Files\Python3*\python.exe')
}
if (-not $python) { throw 'Python was installed but not found. Open a new terminal and run setup again.' }
Write-Host "Using $python"

# ---- 2. config ----
Step 'Server config'
function Rand($n) {
    $b = New-Object byte[] $n
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
    return (($b | ForEach-Object { $_.ToString('x2') }) -join '')
}
if (Test-Path $cfgPath) {
    $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
    Write-Host 'Keeping the existing config.json'
} else {
    $cfg = [ordered]@{ port = 8787; ingest_key = (Rand 16); admin_user = 'admin'; admin_password = (Rand 9) }
    $cfg | ConvertTo-Json | Set-Content -Path $cfgPath -Encoding ASCII
    Write-Host 'Created config.json'
}
$port = [int]$cfg.port

# ---- 3. start the server and check it locally ----
Step 'Starting the server'
Register-Startup $taskServer $python "`"$here\server.py`""
$ok = $false
for ($i = 0; $i -lt 15 -and -not $ok; $i++) {
    Start-Sleep -Seconds 1
    try { $ok = (Invoke-WebRequest "http://127.0.0.1:$port/health" -UseBasicParsing -TimeoutSec 2).Content -eq 'ok' } catch {}
}
if (-not $ok) { throw "The server did not answer on port $port. Run it by hand to see the error:  `"$python`" `"$here\server.py`"" }
Write-Host "Server is up on 127.0.0.1:$port"

# ---- 4. tunnel ----
Step 'Tunnel'
Write-Host 'How should the server be reachable from the internet?'
Write-Host '  1) Cloudflare Tunnel  - free, but needs a domain you own that is on Cloudflare (a domain costs ~10 USD/year)'
Write-Host '  2) Tailscale Funnel   - free, no domain needed; you get a fixed https://<pc>.<name>.ts.net address'
$choice = Read-Host 'Choose 1 or 2'
$publicUrl = $null

if ($choice -eq '1') {
    $cf = Find-Exe 'cloudflared' @('C:\Program Files (x86)\cloudflared\cloudflared.exe', 'C:\Program Files\cloudflared\cloudflared.exe')
    if (-not $cf) {
        Ensure-Winget 'Cloudflare.cloudflared' @()
        $cf = Find-Exe 'cloudflared' @('C:\Program Files (x86)\cloudflared\cloudflared.exe', 'C:\Program Files\cloudflared\cloudflared.exe')
    }
    if (-not $cf) { throw 'cloudflared was installed but not found. Open a new terminal and run setup again.' }

    $cfDir = Join-Path $here 'cloudflared'
    New-Item -ItemType Directory -Force $cfDir | Out-Null
    $cert = Join-Path $env:USERPROFILE '.cloudflared\cert.pem'
    if (-not (Test-Path $cert)) {
        Write-Host 'A browser window opens: log in to Cloudflare and pick the domain to use.'
        & $cf tunnel login
    }
    $hostName = Read-Host 'Hostname to publish, e.g. telemetry.yourdomain.com'
    $name = 'somniel-telemetry'
    $list = (& $cf tunnel list --output json) | ConvertFrom-Json
    $t = $list | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if (-not $t) {
        & $cf tunnel create $name
        $list = (& $cf tunnel list --output json) | ConvertFrom-Json
        $t = $list | Where-Object { $_.name -eq $name } | Select-Object -First 1
    }
    $id = $t.id
    $credSrc = Join-Path $env:USERPROFILE ".cloudflared\$id.json"
    if (-not (Test-Path $credSrc)) { throw "Tunnel credentials $credSrc not found (was the tunnel created on another PC? delete it in the Cloudflare dashboard and run again)." }
    Copy-Item $credSrc (Join-Path $cfDir "$id.json") -Force
    & $cf tunnel route dns --overwrite-dns $name $hostName
    @"
tunnel: $id
credentials-file: $cfDir\$id.json
ingress:
  - hostname: $hostName
    service: http://127.0.0.1:$port
  - service: http_status:404
"@ | Set-Content -Path (Join-Path $cfDir 'config.yml') -Encoding ASCII
    Register-Startup $taskTunnel $cf "tunnel --config `"$cfDir\config.yml`" --no-autoupdate run $name"
    $publicUrl = "https://$hostName"
}
elseif ($choice -eq '2') {
    $ts = Find-Exe 'tailscale' @('C:\Program Files\Tailscale\tailscale.exe')
    if (-not $ts) {
        Ensure-Winget 'Tailscale.Tailscale' @()
        $ts = Find-Exe 'tailscale' @('C:\Program Files\Tailscale\tailscale.exe')
    }
    if (-not $ts) { throw 'Tailscale was installed but not found. Open a new terminal and run setup again.' }
    Write-Host 'If a browser window opens, sign in to Tailscale (a free account is fine).'
    & $ts up
    Write-Host ''
    Write-Host 'Funnel must be allowed once in your Tailscale admin console. If the next command prints a link, open it,'
    Write-Host 'enable Funnel (and HTTPS certificates if asked), then run this setup again.'
    & $ts funnel --bg $port
    $status = (& $ts funnel status) -join "`n"
    if ($status -match '(https://[A-Za-z0-9.-]+\.ts\.net)') { $publicUrl = $Matches[1] }
    if (-not $publicUrl) { throw "Could not read the Funnel address. Output was:`n$status" }
}
else {
    throw 'Choose 1 or 2.'
}

# ---- 5. check the public address ----
Step 'Checking the public address'
$ok = $false
for ($i = 0; $i -lt 30 -and -not $ok; $i++) {
    try { $ok = (Invoke-WebRequest "$publicUrl/health" -UseBasicParsing -TimeoutSec 5).Content -eq 'ok' } catch { Start-Sleep -Seconds 2 }
}
if ($ok) { Write-Host "Reachable: $publicUrl" -ForegroundColor Green }
else { Write-Host "Not reachable yet at $publicUrl (DNS can take a few minutes). The rest of the setup is done." -ForegroundColor Yellow }

# ---- 6. app config ----
Step 'App config'
@"
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <!-- Telemetry server. Leave both empty to build without telemetry (no prompt, no toggle, nothing sent).
         telemetry-server\setup.ps1 fills these in. -->
    <string name="telemetry_url" translatable="false">$publicUrl/v1/ingest</string>
    <string name="telemetry_key" translatable="false">$($cfg.ingest_key)</string>
</resources>
"@ | Set-Content -Path $xmlPath -Encoding UTF8
Write-Host "Wrote $((Resolve-Path $xmlPath).Path)"

Write-Host ''
Write-Host 'DONE.' -ForegroundColor Green
Write-Host "  Dashboard : $publicUrl/dashboard"
Write-Host "  Login     : $($cfg.admin_user) / $($cfg.admin_password)   (also in config.json)"
Write-Host '  Next      : rebuild the Android app (scripts\build.ps1) so it contains the new address.'
Write-Host '  Both the server and the tunnel now start by themselves when this PC boots.'
