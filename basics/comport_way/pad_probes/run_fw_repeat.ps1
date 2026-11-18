# run_fw_repeat.ps1 - repeat repro_poll54 --rate 2000 --dur 300 N times with one pad_probe_view --keepalive window.
# Same as run_4133_repeat.ps1, but the firmware tag goes into the log names (-Tag 4134 -> data\repro_poll54_4134_N.*).
# Stops on the first failure (exit 3) with two beeps, so the owner can take an R-code without removing power.
param([string]$Tag = "4134", [int]$Runs = 3, [int]$First = 1, [double]$Rate = 2000, [int]$Dur = 300, [string]$Port = "COM5")
# -Port COM7: go through the MAKCU Toolkit virtual port, so Toolkit stays connected (holds COM5) and sees the failure.
Set-Location $PSScriptRoot
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$last = $First + $Runs - 1
$viewDur = ($Dur + 10) * $Runs + 30
[console]::beep(1000,180)
$null = Start-Process -FilePath ".\pad_probe_view.exe" -ArgumentList "--rate 0 --keepalive --dur $viewDur --log data\view_${Tag}_$First-$last.log" -PassThru
Start-Sleep -Seconds 3
for ($i = $First; $i -le $last; $i++) {
    $out = "data\repro_poll54_${Tag}_$i.out"
    "run $i start " + (Get-Date -Format "HH:mm:ss")
    & .\repro_poll54.exe --port $Port --rate $Rate --dur $Dur --log "data\repro_poll54_${Tag}_$i.log" 2>&1 | Out-File -Encoding utf8 $out
    $code = $LASTEXITCODE
    "run $i exit $code at " + (Get-Date -Format "HH:mm:ss") + " : " + (Get-Content $out | Select-Object -Last 1)
    if ($code -ne 0) { [console]::beep(1000,180); Start-Sleep -Milliseconds 120; [console]::beep(1000,180); break }
    Start-Sleep -Seconds 2
}
"done " + (Get-Date -Format "HH:mm:ss")
