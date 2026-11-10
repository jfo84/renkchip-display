# setup_ops.ps1 — one-shot elevated setup: firewall rule + hidden serve_ops.ps1.
# Expects serve_ops.ps1 (and ops.mp4) in %USERPROFILE%\ops. Run once via an
# elevated Task Scheduler job; re-running after a reboot is safe.
$fw = netsh advfirewall firewall show rule name="opsserve" 2>$null
if (-not $fw) { netsh advfirewall firewall add rule name="opsserve" dir=in action=allow protocol=tcp localport=8000 | Out-Null }
$running = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like "*serve_ops.ps1*" }
if (-not $running) { Start-Process powershell -WindowStyle Hidden -ArgumentList "-ExecutionPolicy","Bypass","-File","$env:USERPROFILE\ops\serve_ops.ps1" }
Write-Output "setup done"
