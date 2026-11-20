# udp_pad_ladder.ps1 - лестница долгих прогонов геймпада по UDP: ищем нагрузку, на которой плата не виснет.
# Каждая ступень - udp_pad_inject.exe --soak, нагрузка растёт от ступени к ступени; первая ступень с отказом останавливает
# лестницу (после зависания порта 8080 нужно переподключение платы). Окно pad_probe_view --keepalive на всю лестницу.
#
#   .\udp_pad_ladder.ps1 -Tag T [-Step 300] [-Only "250","250-st10"] [-Smoothing fixed] [-Buffer 1]
#
# Ступень: <команд/с>[-st<период потока мс>]. Без -st поток физики выключен.
param(
    [string]$Tag = "ladder",
    [int]$Step = 300,
    [string[]]$Only = @(),
    [string]$Smoothing = "fixed",
    [int]$Buffer = 1
)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pad = Join-Path $here "..\..\comport_way\pad_probes"
$all = @("250", "250-st10", "250-st1", "500", "500-st10")
$names = @($all | Where-Object { $Only.Count -eq 0 -or $Only -contains $_ })
$dur = [int](($Step + 15) * $names.Count + 60)

[console]::beep(1000, 180)
$view = Start-Process -FilePath (Join-Path $pad "pad_probe_view.exe") -WorkingDirectory $pad -PassThru `
    -ArgumentList @("--rate", "0", "--keepalive", "--dur", "$dur", "--log", "data\view_udp_ladder_$Tag.log")
Start-Sleep -Seconds 3

$summary = @()
Push-Location $here
try {
    foreach ($n in $names) {
        $rate, $st = $n.Split("-")
        $period = [int](1000000 / [int]$rate)
        $a = @("--period", "$period", "--smoothing", $Smoothing, "--soak", "$Step", "--log", "udp_pad_ladder_${Tag}_$n.log")
        if ($Smoothing -ne "off") { $a += @("--buffer", "$Buffer") }
        if ($st) { $a += @("--stream", $st.Substring(2)) }
        Write-Host "=== $n : udp_pad_inject.exe $($a -join ' ')"
        & .\udp_pad_inject.exe @a | Out-File -Encoding utf8 "udp_pad_ladder_${Tag}_${n}_console.txt"
        $code = $LASTEXITCODE
        $txt = Get-Content "udp_pad_ladder_${Tag}_$n.log" -Encoding utf8 -ErrorAction SilentlyContinue
        $txt | Select-String "!!|\+\+|ИТОГ" | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
        $it = ($txt | Select-String "ИТОГ" | Select-Object -Last 1).Line
        $summary += "$n | код $code | $(if ($it) { $it.Trim() } else { '-' })"
        if ($code -ne 0) { Write-Host "!! ступень $n с отказом (код $code) - лестница остановлена"; break }
        Start-Sleep -Seconds 5
    }
} finally {
    Pop-Location
}
Write-Host "=== итог"
$summary | ForEach-Object { Write-Host $_ }
