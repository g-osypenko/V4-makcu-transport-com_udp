# switch_ch343_driver.ps1 - переключить CH343 платы MAKCU (USB2, 1A86:55D3) между драйвером WCH и встроенным usbser.sys.
# Зачем: синий экран 0xD1 в драйвере WCH CH343S64.SYS (HANDOFF_CH343_BSOD.md). CH343 - стандартный CDC-ACM,
# поэтому Windows умеет работать с ним своим драйвером usbser.sys.
#
# Запуск (нужен администратор; перед этим закрыть ВСЕ программы, которые держат COM-порт платы):
#   powershell -ExecutionPolicy Bypass -File switch_ch343_driver.ps1 -To usbser   # встроенный драйвер Microsoft
#   powershell -ExecutionPolicy Bypass -File switch_ch343_driver.ps1 -To wch      # вернуть драйвер WCH
#   powershell -ExecutionPolicy Bypass -File switch_ch343_driver.ps1 -Status      # только показать (права не нужны)
# Механизм: UpdateDriverForPlugAndPlayDevices(INSTALLFLAG_FORCE) - то же, что "devcon update".
# usbser ставится по совместимому ID USB\Class_02&SubClass_02&Prot_01: затронет все ПОДКЛЮЧЁННЫЕ CDC-ACM устройства
# (на 2026-11-16 подключено только CH343). Пакет WCH остаётся в хранилище драйверов; копия: driver_backup_ch343\.
param([ValidateSet('usbser','wch')][string]$To, [switch]$Status, [string]$Log)

$ErrorActionPreference = 'Stop'
if ($Log) { Start-Transcript -Path $Log -Force | Out-Null }

function Show-State {
    $devs = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_1A86&PID_55D3*' }
    if (-not $devs) { "CH343 (1A86:55D3) не подключён" ; return }
    foreach ($d in $devs) {
        $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service, DEVPKEY_Device_DriverInfPath, DEVPKEY_Device_DriverVersion, DEVPKEY_Device_DriverProvider
        $h = @{}; foreach ($x in $p) { $h[$x.KeyName] = $x.Data }
        "{0} | {1} | служба {2} | {3} {4} {5} | статус {6}" -f $d.FriendlyName, $d.InstanceId, $h['DEVPKEY_Device_Service'],
            $h['DEVPKEY_Device_DriverProvider'], $h['DEVPKEY_Device_DriverInfPath'], $h['DEVPKEY_Device_DriverVersion'], $d.Status
    }
}

if ($Status -or -not $To) { Show-State; if ($Log) { Stop-Transcript | Out-Null }; return }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    "[-] нужен запуск от администратора"; if ($Log) { Stop-Transcript | Out-Null }; exit 2
}

Add-Type -Namespace Native -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string fullInfPath, uint installFlags, out bool rebootRequired);
'@

if ($To -eq 'usbser') {
    $hw = 'USB\Class_02&SubClass_02&Prot_01'; $inf = "$env:windir\INF\usbser.inf"
} else {
    $hw = 'USB\VID_1A86&PID_55D3'
    $inf = Get-ChildItem "$env:windir\INF\oem*.inf" | Where-Object { Select-String -Path $_.FullName -Pattern 'CH343SER' -Quiet } |
           Select-Object -First 1 -ExpandProperty FullName
    if (-not $inf) { $inf = Join-Path $PSScriptRoot 'driver_backup_ch343\ch343ser.inf' }
}
"До:"; Show-State
"Ставлю: $inf по ID $hw"
$reboot = $false
$ok = [Native.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $hw, $inf, 1, [ref]$reboot)
$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
if ($ok) { "[+] готово, перезагрузка нужна: $reboot" } else { "[-] не вышло, ошибка Win32 $err (0x{0:X8})" -f $err }
Start-Sleep -Seconds 2
"После:"; Show-State
if ($Log) { Stop-Transcript | Out-Null }
if ($ok) { exit 0 } else { exit 1 }
