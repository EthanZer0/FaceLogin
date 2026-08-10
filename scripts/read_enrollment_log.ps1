[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$c = Get-Content 'D:\Fold\FaceLogin\log\enrollment.log' -Encoding Unicode
Write-Host ("总行数: " + $c.Count)
Write-Host "=== 最后 60 行（含采集/采样诊断）==="
$c | Select-Object -Last 60 | ForEach-Object {
    $line = $_.Line
    $ts = ""
    if ($line -match '\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]') { $ts = $Matches[1] }
    $msg = ""
    if ($line -match '\]\s*\[(\w+)\]\s*\[\d+\]\s*\[([^\]]+)\]\s*(.*)$') { $msg = $Matches[1] + " | " + $Matches[2] + " | " + $Matches[3] }
    Write-Host ($ts + "  " + $msg)
}
Write-Host "=== 所有 sample 相关诊断日志 ==="
$c | Select-String -Pattern 'sample|采样|Enrollment sample|sampling loop' | Select-Object -Last 20 | ForEach-Object { $_.Line.Substring(0, [Math]::Min($_.Line.Length, 220)) }
