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
    [string]$Port = "COM5"
)
Set-Location $PSScriptRoot
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$log = "data\soak_$Tag.log"
function Say([string]$s) {
    $line = "{0} {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $s
    Write-Output $line
    Add-Content -Path $log -Value $line -Encoding utf8
}
function Snapshot([string]$name) {
    $out = "data\soak_${Tag}_info_$name.txt"
    $p = Start-Process -FilePath ".\pad_probe_info.exe" -ArgumentList "--port $Port --poll 1" -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    if (-not $p.WaitForExit(25000)) { Say "  снимок ${name}: pad_probe_info не закончил за 25 с, процесс снят"; try { $p.Kill() } catch {} }
    Say ("  снимок {0}: COM-порты {1}" -f $name, ([System.IO.Ports.SerialPort]::GetPortNames() -join ","))
    Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue |
        Select-String -Pattern 'kinds|km\.device|0x54 CONTROLLER|запросов|sequence менялся|timing|не открылся' |
        Select-Object -First 10 | ForEach-Object { Say ("    | " + $_.Line.Trim()) }
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^USB\\VID_3537' -and $_.InstanceId -notmatch '&(MI|IG)_' } |
        ForEach-Object { Say ("    USB: " + $_.InstanceId) }
}

$n = ($Steps -split ',').Count
$viewDur = $n * $Step + 30
Say "=== старт ${Tag}: ступени $Steps по $Step с, окно $viewDur с"
$null = Start-Process -FilePath ".\pad_probe_view.exe" -ArgumentList "--rate 0 --keepalive --dur $viewDur --log data\view_soak_$Tag.log" -PassThru
Start-Sleep -Seconds 2
Push-Location ..\perfect-transport-gamepad-com
& .\pad_soak.exe --port $Port --step $Step --steps $Steps 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -Variable soakOut | Out-Null
$code = $LASTEXITCODE
Pop-Location
$soakOut | Add-Content -Path $log -Encoding utf8
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
