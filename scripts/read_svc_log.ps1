[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$l = Get-Content 'D:\WorkDir\Facelogin\scripts\svc.log' -Encoding Unicode
Write-Host "=== 20:50-20:55 完整认证序列 ==="
$l | Select-String -Pattern '20:5[0-5]' | ForEach-Object {
    $line = $_.Line
    $ts = ""
    if ($line -match '\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]') { $ts = $Matches[1] }
    $msg = ""
    if ($line -match '\]\s*\[(\w+)\]\s*\[\d+\]\s*\[([^\]]+)\]\s*(.*)$') { $msg = $Matches[1] + " | " + $Matches[2] + " | " + $Matches[3] }
    Write-Host ($ts + "  " + $msg)
}
