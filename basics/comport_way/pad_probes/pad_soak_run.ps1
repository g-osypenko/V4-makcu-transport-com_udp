# pad_soak_run.ps1 - лестница pad_soak с окном наблюдения и снимками платы после обрыва.
#   1. Окно pad_probe_view.exe --rate 0 --notouch --dur (ступени * Step + 30): "НЕ ТРОГАЙ геймпад: стик и кнопки двигает программа...",
#      при отказе "ГЕЙМПАД ПРОПАЛ/МОЛЧИТ ... Ничего не трогай: жди сообщения в чате".
#   2. ..\perfect-transport-gamepad-com\pad_soak.exe --step Step --steps Steps (журнал data\soak_<Tag>.log).
#   3. Если pad_soak вышел с кодом 3 (обрыв): pad_probe_info --poll 1 сразу, через 10 и через 30 с (data\soak_<Tag>_info_*.txt),
#      затем два сигнала (нужно действие владельца). Окно при этом не закрывается (Stop-Process владелец принял бы за краш).
# Запуск (из pad_probes): powershell -NoProfile -ExecutionPolicy Bypass -File .\pad_soak_run.ps1 -Tag NAME -Steps a,b -Step 180
# Файл в UTF-8 С BOM (PowerShell 5.1).
param(
    [string]$Tag = "soak",
    [string]$Steps = "hold2-p100",
    [int]$Step = 180,
    [string]$Port = "COM5",
    [int]$KeepGoing = 0,  # >0: pad_soak --keep-going N (отказ не останавливает лестницу, ждёт возврата до N с; код 2 - были отказы)
    [string]$View = "--keepalive"  # режим окна pad_probe_view: --keepalive (инжект) или --pollonly (только опрос)
)
Set-Location $PSScriptRoot
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$log = "data\soak_$Tag.log"
$logFull = Join-Path $PSScriptRoot $log
# Запись в журнал не должна падать, если файл кто-то читает (2026-11-20: tail -f из Git Bash блокировал Add-Content,
# журнал pad_soak пропал): FileShare.ReadWrite, три попытки.
function Write-LogLine([string]$line) {
    for ($i = 0; $i -lt 3; $i++) {
        try {
            $fs = [System.IO.File]::Open($logFull, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
            $b = [System.Text.Encoding]::UTF8.GetBytes($line + "`r`n")
            $fs.Write($b, 0, $b.Length); $fs.Close(); return
        } catch { Start-Sleep -Milliseconds 20 }
    }
}
function Say([string]$s) {
    $line = "{0} {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $s
    Write-Output $line
    Write-LogLine $line
}
function Snapshot([string]$name) {
    $out = "data\soak_${Tag}_info_$name.txt"
    # --poll 0 (2026-11-20): --poll 1 гнал 0x54 замкнутым циклом ~3500/с целую секунду - сразу после отказа это мешало
    # плате подняться и могло испортить вывод «сам не вернулся». Без цикла снимок даёт один sequence: сравнивать t0/t10/t30.
    $p = Start-Process -FilePath ".\pad_probe_info.exe" -ArgumentList "--port $Port --poll 0" -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    if (-not $p.WaitForExit(25000)) { Say "  снимок ${name}: pad_probe_info не закончил за 25 с, процесс снят"; try { $p.Kill() } catch {} }
    Say ("  снимок {0}: COM-порты {1}" -f $name, ([System.IO.Ports.SerialPort]::GetPortNames() -join ","))
    Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue |
        Select-String -Pattern 'kinds|km\.device|0x54 CONTROLLER|запросов|sequence менялся|usb_timestamp|не открылся' |
        Select-Object -First 10 | ForEach-Object { Say ("    | " + $_.Line.Trim()) }
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^USB\\VID_3537' -and $_.InstanceId -notmatch '&(MI|IG)_' } |
        ForEach-Object { Say ("    USB: " + $_.InstanceId) }
}

$n = ($Steps -split ',').Count
$viewDur = $n * $Step + 30
Say "=== старт ${Tag}: ступени $Steps по $Step с, окно $viewDur с"
$null = Start-Process -FilePath ".\pad_probe_view.exe" -ArgumentList "--rate 0 $View --dur $viewDur --log data\view_soak_$Tag.log" -PassThru
Start-Sleep -Seconds 2
Push-Location ..\perfect-transport-gamepad-com
$soakArgs = @("--port", $Port, "--step", $Step, "--steps", $Steps)
if ($KeepGoing -gt 0) { $soakArgs += @("--keep-going", $KeepGoing) }
& .\pad_soak.exe @soakArgs 2>&1 | ForEach-Object { $line = $_.ToString(); Write-LogLine $line; Write-Host $line }
$code = $LASTEXITCODE
Pop-Location
Say "pad_soak: код выхода $code"
if ($code -eq 3) {
    Snapshot "t0"
    Start-Sleep -Seconds 10
    Snapshot "t10"
    Start-Sleep -Seconds 20
    Snapshot "t30"
    [console]::beep(1000,180); Start-Sleep -Milliseconds 120; [console]::beep(1000,180)
}
exit $code
