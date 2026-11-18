# Diagnostics only: reads firewall state, changes nothing, no admin rights needed.
$ErrorActionPreference = 'Continue'
$out = Join-Path $PSScriptRoot 'firewall_diag.txt'
& {
  "=== time: $(Get-Date)"
  "=== PS $($PSVersionTable.PSVersion); OS $([Environment]::OSVersion.VersionString)"
  $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  "=== admin: $($p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"
  "`n=== third-party firewall / antivirus"
  try { Get-CimInstance -Namespace root/SecurityCenter2 -ClassName FirewallProduct -ErrorAction Stop | Select-Object displayName, productState | Format-Table -AutoSize | Out-String -Width 300 } catch { "n/a: $_" }
  try { Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction Stop | Select-Object displayName, productState | Format-Table -AutoSize | Out-String -Width 300 } catch { "n/a: $_" }
  "`n=== profiles (ActiveStore)"
  Get-NetFirewallProfile -PolicyStore ActiveStore | Select-Object Name, Enabled, DefaultInboundAction, AllowLocalFirewallRules, AllowInboundRules, NotifyOnListen | Format-Table -AutoSize | Out-String -Width 300
  "`n=== networks"
  Get-NetConnectionProfile | Select-Object InterfaceAlias, NetworkCategory, IPv4Connectivity | Format-Table -AutoSize | Out-String -Width 300
  Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias, IPAddress, PrefixLength | Format-Table -AutoSize | Out-String -Width 300
  "`n=== MAKCU rules"
  foreach ($r in @(Get-NetFirewallRule -DisplayName 'MAKCU UDP events' -ErrorAction SilentlyContinue)) {
    $a = ($r | Get-NetFirewallAddressFilter); $pf = ($r | Get-NetFirewallPortFilter); $i = ($r | Get-NetFirewallInterfaceFilter)
    "{0} enabled={1} {2} {3} proto={4} lport={5} remote={6} iface={7} profile={8} store={9}" -f $r.Name, $r.Enabled, $r.Direction, $r.Action, $pf.Protocol, ($pf.LocalPort -join ','), ($a.RemoteAddress -join ','), ($i.InterfaceAlias -join ','), $r.Profile, $r.PolicyStoreSourceType
  }
  "`n=== enabled INBOUND BLOCK rules (block wins over allow)"
  foreach ($r in @(Get-NetFirewallRule -PolicyStore ActiveStore -Direction Inbound -Action Block -Enabled True -ErrorAction SilentlyContinue)) {
    $pf = ($r | Get-NetFirewallPortFilter); $app = ($r | Get-NetFirewallApplicationFilter)
    "{0} | proto={1} lport={2} | program={3} | profile={4} | store={5}" -f $r.DisplayName, $pf.Protocol, ($pf.LocalPort -join ','), $app.Program, $r.Profile, $r.PolicyStoreSourceType
  }
  "`n=== UDP listeners (non-svchost)"
  Get-NetUDPEndpoint -ErrorAction SilentlyContinue | ForEach-Object {
    $pr = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
    if ($pr -and $pr.ProcessName -notin @('svchost','System','Idle','lsass')) { "{0}:{1} {2} ({3})" -f $_.LocalAddress, $_.LocalPort, $pr.ProcessName, $pr.Path }
  } | Sort-Object -Unique
} *>&1 | Out-File -FilePath $out -Encoding utf8
Write-Host "saved: $out"
