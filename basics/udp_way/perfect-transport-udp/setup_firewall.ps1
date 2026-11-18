# Настройка брандмауэра Windows для событий кнопок MAKCU по UDP.
#
# Запускать setup_firewall.bat (двойной клик) или:
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup_firewall.ps1 [параметры]
#
#   без параметров            правило для всей локальной подсети, IP платы знать не нужно
#   -RemoteAddress 1.2.3.4    правило только для этого адреса (уже, но ломается при смене IP)
#   -RemoteAddress Any        правило для любых адресов (если плата в другой подсети)
#   -InterfaceAlias Ethernet  правило только для этого сетевого адаптера
#   -Check                    только показать состояние, ничего не менять
#   -Remove                   удалить правило
#   -WhatIf                   показать, что было бы сделано, ничего не менять
#
# Зачем: плата шлёт события 0x53 с порта, равного порту программы, а не с 8080. Без разрешающего
# правила брандмауэр такие пакеты молча режет (в любом профиле сети), движение при этом работает,
# а кнопки держатся на запасном опросе. Правило одно на все программы и все локальные порты,
# потому что порт программы случаен. Повторный запуск заменяет правило (и дубликаты) одним.
#
# Всё, включая -Check, выполняется в окне с правами администратора: на части систем без прав
# Get-NetFirewallRule правил не видит и возвращает пустой список. Скрипт сам запросит UAC,
# а вывод окна с правами покажет здесь.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Check,
    [switch]$Remove,
    [string]$RemoteAddress = 'LocalSubnet',
    [string]$InterfaceAlias = '',
    [string]$LogPath = ''   # служебный: куда окно с правами пишет вывод
)

$ErrorActionPreference = 'Stop'
$RuleName = 'MAKCU UDP events'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

function Write-Log([string]$text) {
    Write-Host $text
    if ($LogPath) { try { $text | Out-File -LiteralPath $LogPath -Encoding utf8 -Append } catch { } }
}

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Ошибки чтения не глушим: «нет правила» и «не смог прочитать» это разные вещи.
function Get-MakcuRules {
    try {
        return @(Get-NetFirewallRule -DisplayName $RuleName -ErrorAction Stop)
    } catch {
        if ($_.CategoryInfo.Category -eq 'ObjectNotFound') { return @() }
        throw
    }
}

# Напечатать правила и сети, вернуть $true, если события кнопок пройдут.
function Show-State {
    try {
        $rules = Get-MakcuRules
    } catch {
        Write-Log "[-] не удалось прочитать правила брандмауэра: $($_.Exception.Message)"
        return $false
    }
    $good = $false
    $byAddress = $false
    foreach ($rule in $rules) {
        $address = ($rule | Get-NetFirewallAddressFilter).RemoteAddress -join ','
        $protocol = ($rule | Get-NetFirewallPortFilter).Protocol
        $iface = ($rule | Get-NetFirewallInterfaceFilter).InterfaceAlias -join ','
        Write-Log ("правило: вкл. {0}, {1}, {2}, {3}, от {4}, адаптер {5}, профиль {6}" -f
            $rule.Enabled, $rule.Direction, $rule.Action, $protocol, $address, $iface, $rule.Profile)
        if ("$($rule.Enabled)" -eq 'True' -and "$($rule.Direction)" -eq 'Inbound' -and "$($rule.Action)" -eq 'Allow' -and
            ($protocol -eq 'UDP' -or $protocol -eq 'Any')) {
            $good = $true
            if ($address -notin @('Any', 'LocalSubnet')) { $byAddress = $true }
        }
    }
    if ($rules.Count -gt 1) { Write-Log "[!] правил $($rules.Count): запустите настройку ещё раз, останется одно" }
    # Запрещающее правило сильнее разрешающего: Windows создаёт такие сама, если в окне
    # «Разрешить доступ?» нажать Отмена.
    foreach ($b in @(Get-NetFirewallRule -PolicyStore ActiveStore -Direction Inbound -Action Block -Enabled True -ErrorAction SilentlyContinue)) {
        $bp = ($b | Get-NetFirewallPortFilter).Protocol
        if ($bp -eq 'UDP' -or $bp -eq 'Any') {
            $prog = ($b | Get-NetFirewallApplicationFilter).Program
            if ($prog -and $prog -ne 'Any') {
                # Касается только этой программы: мешает, лишь если MAKCU-программа именно она.
                Write-Log ("[i] запрет только для программы: {0} ({1})" -f $b.DisplayName, $prog)
            } else {
                Write-Log ("[!] запрещающее правило для всех программ, оно сильнее нашего: {0}" -f $b.DisplayName)
                $good = $false
            }
        }
    }
    foreach ($net in @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)) {
        Write-Log ("сеть: {0} = {1}" -f $net.InterfaceAlias, $net.NetworkCategory)
    }
    if ($good) {
        Write-Log '[+] правило есть, события кнопок разрешены'
        if ($byAddress) { Write-Log '[!] правило привязано к IP: если адрес платы сменится, запустите скрипт снова' }
    } else {
        Write-Log '[-] рабочего правила нет: события кнопок будут отрезаны брандмауэром'
    }
    return $good
}

# Без прав: перезапуститься через UAC, вывод окна с правами показать здесь.
if (-not (Test-Admin) -and -not $WhatIfPreference) {
    $log = Join-Path ([IO.Path]::GetTempPath()) 'makcu_firewall.log'
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -LogPath `"$log`""
    if ($Check) { $argLine += ' -Check' }
    if ($Remove) { $argLine += ' -Remove' }
    if ($PSBoundParameters.ContainsKey('RemoteAddress')) { $argLine += " -RemoteAddress `"$RemoteAddress`"" }
    if ($InterfaceAlias) { $argLine += " -InterfaceAlias `"$InterfaceAlias`"" }
    try {
        $child = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argLine -Wait -PassThru -WindowStyle Hidden
    } catch {
        Write-Host '[-] права администратора не получены (UAC), ничего не сделано'
        exit 1
    }
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log -Encoding UTF8 | Write-Host
    } else {
        Write-Host "[!] окно с правами ничего не записало (код $($child.ExitCode)): скрипт в нём, похоже, не запустился"
    }
    exit $child.ExitCode
}

if ($Check) {
    if (Show-State) { exit 0 }
    exit 1
}

try {
    $old = Get-MakcuRules
    if ($Remove) {
        foreach ($rule in $old) { Remove-NetFirewallRule -Name $rule.Name }
        Write-Log "[+] удалено правил: $($old.Count)"
        exit 0
    }

    # Сначала новое, потом удаление старых: если параметры неверны, прежнее правило останется.
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
} catch {
    Write-Log "[-] ошибка: $($_.Exception.Message)"
    exit 1
}
if ($WhatIfPreference) { exit 0 }
if (Show-State) { exit 0 }
exit 1
