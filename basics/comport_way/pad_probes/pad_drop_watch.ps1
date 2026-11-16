# pad_drop_watch.ps1 - журнал присутствия геймпада в USB (раз в 0.2 с) рядом с окном pad_probe_view, снимки платы по COM.
#
# Режим drop (по умолчанию): ловит первый отказ и сразу снимает состояние платы по COM.
#   1. За -PreSec секунд до окна начинает писать присутствие VID_3537 в USB (чтобы видеть, совпал ли обрыв со стартом окна).
#   2. Запускает окно pad_probe_view.exe --rate 0 (COM закрыт, плате не уходит ни одной команды).
#   3. Ждёт геймпад до -WaitSec, снимок "before": pad_probe_info.exe --poll 1 (только чтение; COM открыт ~3 с).
#   4. Отказ = геймпад пропал из USB или окно записало "МОЛЧИТ" / "в XInput НЕТ" -> снимки "eN_t0", "eN_t10", "eN_t30",
#      дальше следит снова; после трёх отказов или в конце -DurSec выход с кодом 3. Окно запущено с --watch: в нём прямые
#      инструкции ("Крути ПРАВЫЙ стик...", после отказа 40 с "НИЧЕГО НЕ ТРОГАЙ ещё N с: опрашиваю плату").
# Режим hotplug (-Hotplug): владелец сам вынимает и вставляет донгл; скрипт только пишет каждое пропадание и появление с
#   временем и длительностью, COM не открывает до конца окна. В конце, если геймпада нет, - снимок "end" (жив ли порт платы).
# -NoBoard: донгл в ПК, плату по COM не спрашивать вообще.
# Каждый запуск pad_probe_info ограничен 25 с (если плата не отвечает и проба висит, её процесс снимается).
#
# Запуск (из pad_probes): powershell -NoProfile -ExecutionPolicy Bypass -File .\pad_drop_watch.ps1 -Tag NAME
#                         [-DurSec 300] [-WaitSec 180] [-PreSec 5] [-Hotplug] [-NoBoard]
# Журнал: data\drop_<Tag>.log (этот скрипт), data\view_<Tag>.log (окно), data\drop_<Tag>_info_*.txt (снимки платы).
# Файл должен быть в UTF-8 С BOM: PowerShell 5.1 без BOM читает кириллицу как ANSI и не разбирает скрипт.
param(
    [string]$Tag = "drop",
    [int]$DurSec = 300,
    [int]$WaitSec = 180,
    [int]$PreSec = 5,
    [switch]$Hotplug,
    [switch]$NoBoard,
    [string]$Port = "COM5"
)
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
$log = "data\drop_$Tag.log"

function Say([string]$s) {
    $line = "{0} {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $s
    Write-Output $line
    Add-Content -Path $log -Value $line -Encoding utf8
}
function PadIds {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^USB\\VID_3537' -and $_.InstanceId -notmatch '&(MI|IG)_' } |
        ForEach-Object { $_.InstanceId }
}
function UsbList {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^USB\\VID_(3537|045E|1A86)' -and $_.InstanceId -notmatch '&(MI|IG)_' } |
        ForEach-Object { "    {0} | {1}" -f $_.FriendlyName, $_.InstanceId }
}
function Snapshot([string]$name) {
    if ($NoBoard) { return }
    $out = "data\drop_${Tag}_info_$name.txt"
    $t0 = Get-Date
    $p = Start-Process -FilePath ".\pad_probe_info.exe" -ArgumentList "--port $Port --poll 1" -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    if (-not $p.WaitForExit(25000)) {
        Say "  снимок ${name}: pad_probe_info не закончил за 25 с - плата не отвечает? процесс снят"
        try { $p.Kill() } catch {}
    }
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    Say ("  снимок {0}: {1} мс, COM-порты: {2}" -f $name, $ms, ([System.IO.Ports.SerialPort]::GetPortNames() -join ","))
    Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue |
        Select-String -Pattern 'kinds|km\.device|km\.version|0x54|запросов|sequence менялся|не открылся|нет ответа' |
        Select-Object -First 12 | ForEach-Object { Say ("    | " + $_.Line.Trim()) }
}

# Состояние присутствия: пишет строку только на смене (появился / пропал / сменился PID), с длительностью прошлого состояния.
$script:last = $null
$script:lastT = Get-Date
$script:drops = 0
function Poll {
    $ids = @(PadIds) -join " ; "
    if ($ids -ne $script:last) {
        $was = ((Get-Date) - $script:lastT).TotalSeconds
        if ($ids) { Say ("[+] в USB: {0}   (до этого {1:F1} с: {2})" -f $ids, $was, ($(if ($script:last) { $script:last } else { "нет" }))) }
        else { Say ("[!] геймпада в USB НЕТ   (был {0:F1} с: {1})" -f $was, $script:last); if ($null -ne $script:last) { $script:drops++ } }
        $script:last = $ids
        $script:lastT = Get-Date
    }
    return [bool]$ids
}

Say ("=== старт {0}: режим {1}, журнал USB за {2} с до окна, окно pad_probe_view --rate 0 --dur {3}{4}" -f $Tag,
    ($(if ($Hotplug) { "hotplug" } else { "drop" })), $PreSec, $DurSec, ($(if ($NoBoard) { ", плату не спрашиваю" } else { "" })))
$tPre = Get-Date
while (((Get-Date) - $tPre).TotalSeconds -lt $PreSec) { Poll | Out-Null; Start-Sleep -Milliseconds 200 }
Say "--- окно открывается"
$viewLog = "data\view_$Tag.log"
$viewArgs = "--rate 0 --dur $DurSec --log $viewLog" + ($(if ($Hotplug) { "" } else { " --watch" }))
$null = Start-Process -FilePath ".\pad_probe_view.exe" -ArgumentList $viewArgs -PassThru
$tStart = Get-Date

if ($Hotplug) {
    while (((Get-Date) - $tStart).TotalSeconds -lt $DurSec + 2) { Poll | Out-Null; Start-Sleep -Milliseconds 200 }
    $present = Poll
    Say "--- конец: пропаданий $($script:drops), геймпад сейчас в USB: $present"
    UsbList | ForEach-Object { Say $_ }
    if (-not $present) { Snapshot "end"; exit 3 }
    exit 0
}

while (-not (Poll)) {
    if (((Get-Date) - $tStart).TotalSeconds -gt $WaitSec) { Say "геймпад так и не появился за $WaitSec с"; UsbList | ForEach-Object { Say $_ }; Snapshot "never"; exit 4 }
    Start-Sleep -Milliseconds 200
}
UsbList | ForEach-Object { Say $_ }
Snapshot "before"
# События окна (журнал pad_probe_view без BOM, UTF-8): "МОЛЧИТ" (XInput подключён, пакетов нет 2 с) и "в XInput НЕТ".
# Только строки событий "[!] ...", с учётом регистра: в строках статистики раз в 10 с есть "молчит 0", а -match без
# учёта регистра принял их за отказ (2026-11-16 22:04, три ложных отказа). Возвращает массив через запятую: иначе
# PowerShell разворачивает массив из одной строки в строку, и [-1] даёт последний символ.
function ViewEvents {
    $lines = Get-Content $viewLog -Encoding utf8 -ErrorAction SilentlyContinue
    return ,@($lines | Where-Object { $_ -cmatch '^\[!\].*(МОЛЧИТ|в XInput НЕТ)' })
}
$tPad = Get-Date
$seen = (ViewEvents).Count
$events = 0
$nextView = Get-Date
while (((Get-Date) - $tStart).TotalSeconds -lt $DurSec) {
    $why = $null
    if (-not (Poll)) { $why = "пропал из USB" }
    elseif ((Get-Date) -ge $nextView) {
        $nextView = (Get-Date).AddSeconds(1)
        $ev = ViewEvents
        if ($ev.Count -gt $seen) { $why = "окно: " + $ev[-1].Trim() }
    }
    if ($why) {
        $events++
        Say ("!!! отказ {0}: {1}; через {2:F1} с после появления" -f $events, $why, ((Get-Date) - $tPad).TotalSeconds)
        Snapshot "e${events}_t0"
        Start-Sleep -Seconds 10
        Say ("через 10 с: геймпад в USB {0}" -f (Poll))
        Snapshot "e${events}_t10"
        Start-Sleep -Seconds 20
        Say ("через 30 с: геймпад в USB {0}" -f (Poll))
        Snapshot "e${events}_t30"
        $seen = (ViewEvents).Count
        if ($events -ge 3) { Say "три отказа - выход"; exit 3 }
        $tPad = Get-Date
    }
    Start-Sleep -Milliseconds 200
}
if ($events -gt 0) { Say "--- конец: отказов $events"; exit 3 }
Say "за $DurSec с отказов не было"
Snapshot "after"
exit 0
