# Настройка брандмауэра Windows для событий кнопок MAKCU по UDP.
#
# Запускать setup_firewall.bat (двойной клик) или:
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup_firewall.ps1 [параметры]
#
#   без параметров            правило для всей локальной подсети, IP платы знать не нужно
#   -RemoteAddress 1.2.3.4    правило только для этого адреса (уже, но ломается при смене IP)
#   -InterfaceAlias Ethernet  правило только для этого сетевого адаптера
#   -Check                    только показать состояние, ничего не менять, права не нужны
#   -Remove                   удалить правило
#   -WhatIf                   показать, что было бы сделано, ничего не менять
#
# Зачем: плата шлёт события 0x53 с порта, равного порту программы, а не с 8080. В сети Public
# брандмауэр такие пакеты молча режет, движение при этом работает, а кнопки держатся на запасном
# опросе. Правило одно на все программы и все локальные порты, потому что порт программы случаен.
# Изменение прав требует, скрипт сам запросит UAC. Повторный запуск заменяет правило, а не
# добавляет второе.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Check,
    [switch]$Remove,
    [string]$RemoteAddress = 'LocalSubnet',
    [string]$InterfaceAlias = ''
)

$ErrorActionPreference = 'Stop'
$RuleName = 'MAKCU UDP events'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-MakcuRules {
    return @(Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue)
}

# Напечатать правила и сети, вернуть $true, если события кнопок пройдут.
function Show-State {
    $good = $false
    $byAddress = $false
    $rules = Get-MakcuRules
    foreach ($rule in $rules) {
        $address = ($rule | Get-NetFirewallAddressFilter).RemoteAddress -join ','
        $protocol = ($rule | Get-NetFirewallPortFilter).Protocol
        $iface = ($rule | Get-NetFirewallInterfaceFilter).InterfaceAlias -join ','
        Write-Host ("правило: вкл. {0}, {1}, {2}, {3}, от {4}, адаптер {5}, профиль {6}" -f
            $rule.Enabled, $rule.Direction, $rule.Action, $protocol, $address, $iface, $rule.Profile)
        if ("$($rule.Enabled)" -eq 'True' -and "$($rule.Direction)" -eq 'Inbound' -and "$($rule.Action)" -eq 'Allow' -and
            ($protocol -eq 'UDP' -or $protocol -eq 'Any')) {
            $good = $true
            if ($address -notin @('Any', 'LocalSubnet')) { $byAddress = $true }
        }
    }
    foreach ($net in @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)) {
        Write-Host ("сеть: {0} = {1}" -f $net.InterfaceAlias, $net.NetworkCategory)
    }
    if ($good) {
        Write-Host '[+] правило есть, события кнопок разрешены'
        if ($byAddress) { Write-Host '[!] правило привязано к IP: если адрес платы сменится, запустите скрипт снова' }
    } else {
        Write-Host '[-] рабочего правила нет: в сети Public события кнопок будут отрезаны'
    }
    return $good
}

if ($Check) {
    if (Show-State) { exit 0 }
    exit 1
}

# Без прав: перезапуститься через UAC и показать результат здесь, окно с правами закроется сразу.
if (-not (Test-Admin) -and -not $WhatIfPreference) {
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($Remove) { $argLine += ' -Remove' }
    if ($PSBoundParameters.ContainsKey('RemoteAddress')) { $argLine += " -RemoteAddress `"$RemoteAddress`"" }
    if ($InterfaceAlias) { $argLine += " -InterfaceAlias `"$InterfaceAlias`"" }
    try {
        $child = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argLine -Wait -PassThru -WindowStyle Hidden
    } catch {
        Write-Host '[-] права администратора не получены (UAC), ничего не изменено'
        exit 1
    }
    if ($child.ExitCode -ne 0) { Write-Host "[-] настройка завершилась с кодом $($child.ExitCode)" }
    if ($Remove) {
        if (Get-MakcuRules) { exit 1 }
        Write-Host '[+] правило удалено'
        exit 0
    }
    if (Show-State) { exit 0 }
    exit 1
}

$old = Get-MakcuRules
if ($Remove) {
    foreach ($rule in $old) { Remove-NetFirewallRule -Name $rule.Name }
    exit 0
}

# Сначала новое, потом удаление старого: если параметры неверны, прежнее правило останется.
$params = @{
    DisplayName   = $RuleName
    Description   = 'MAKCU: button events 0x53 arrive from the port of the program, not from 8080'
    Direction     = 'Inbound'
    Protocol      = 'UDP'
    Action        = 'Allow'
    Profile       = 'Any'
    RemoteAddress = $RemoteAddress
}
if ($InterfaceAlias) { $params.InterfaceAlias = $InterfaceAlias }
New-NetFirewallRule @params | Out-Null
foreach ($rule in $old) { Remove-NetFirewallRule -Name $rule.Name }
exit 0
