# ch343_overnight.ps1 - долгий прогон COM для проверки синего экрана 0xD1 в драйвере CH343 (HANDOFF_CH343_BSOD.md).
# Работает без чата: владелец запускает сам, утром смотрит итог.
#
# Запуск (из папки basics\comport_way, обычный PowerShell, права не нужны):
#   powershell -ExecutionPolicy Bypass -File .\ch343_overnight.ps1            # 8 ч по умолчанию
#   powershell -ExecutionPolicy Bypass -File .\ch343_overnight.ps1 -Hours 1
#   powershell -ExecutionPolicy Bypass -File .\ch343_overnight.ps1 -Report    # итог: был ли синий экран, хвост журнала
# Что делает прогон: track_test_com.exe --stress (нагрузка, которая на драйвере 2.1 роняла систему на 610-й с): инжект порциями
# 200 мс через 300 мс, между ними слежение ~950 опросов 0x63/с, клик раз в 2 с, блокировка раз в 10 с. Эталон читает порт
# с тайм-аутом 50 мс - путь, на котором драйвер WCH падает. Окно стенда стартует по клику, курсор всё время двигается в нём:
# ПК в это время не использовать, мышь не трогать.
# Признаки: синий экран - журнал обрывается, после перезагрузки -Report покажет событие BugCheck 1001 и номер дампа;
# без сбоя - журнал доходит до конца, строка "итог".
param([double]$Hours = 8, [switch]$Report)

$dir   = $PSScriptRoot
$state = Join-Path $dir 'ch343_overnight_last.txt'   # путь журнала и время старта последнего прогона

function Show-Driver { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $dir 'switch_ch343_driver.ps1') -Status }
function Show-Pcap {
    $f = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{36fc9e60-c465-11cf-8056-444553540000}').UpperFilters
    $loaded = [bool](Get-CimInstance Win32_SystemDriver -Filter "Name='USBPcap'" -ErrorAction SilentlyContinue | Where-Object State -eq 'Running')
    "USBPcap: фильтр класса USB = '{0}', драйвер работает = {1}" -f ($f -join ','), $loaded
}

if ($Report) {
    if (-not (Test-Path $state)) { "Прогонов ещё не было ($state нет)"; return }
    $s = Get-Content $state; $start = [datetime]::Parse($s[0]); $log = $s[1]
    "Старт прогона: $start, журнал: $log"
    "Загрузка Windows: " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    $bc = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001; StartTime=$start} -ErrorAction SilentlyContinue |
          Where-Object { $_.Message -match 'bugcheck' }
    if ($bc) { "СИНИЙ ЭКРАН после старта:"; $bc | ForEach-Object { "  {0}  {1}" -f $_.TimeCreated, (($_.Message -replace '\s+',' ').Substring(0,220)) } }
    else { "Синих экранов (System 1001 bugcheck) после старта нет." }
    Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Kernel-Power'; Id=41; StartTime=$start} -ErrorAction SilentlyContinue |
        ForEach-Object { "  Kernel-Power 41: {0}, код {1}" -f $_.TimeCreated, $_.Properties[0].Value }
    if (Test-Path $log) { "Хвост журнала:"; Get-Content $log -Encoding UTF8 -Tail 6 } else { "Журнала нет: прогон не стартовал (не было клика в окно?)" }
    Show-Driver; Show-Pcap
    return
}

Show-Driver; Show-Pcap
$secs = [int]($Hours * 3600)
$log  = Join-Path $dir ("stress_ch343_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
Set-Content -Path $state -Value @((Get-Date).ToString('o'), $log) -Encoding UTF8
[console]::beep(1000,180); Start-Sleep -Milliseconds 120; [console]::beep(1000,180)
""
"Прогон $Hours ч ($secs с). КЛИКНИТЕ В ОКНО СТЕНДА (ждёт 300 с), потом НЕ ТРОГАЙТЕ МЫШЬ до конца."
"Журнал: $log"
Set-Location $dir
& .\track_test_com.exe --stress $secs --load inject,track,busy --burst 200,300 --slog $log
"Код выхода: $LASTEXITCODE (0 - прогон дошёл до конца, 3 - обрыв связи с платой)"
