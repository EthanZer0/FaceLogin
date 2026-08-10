[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$l = Get-Content 'D:\WorkDir\Facelogin\scripts\cp.log' -Encoding Unicode
Write-Host "=== CP 管道连接相关（含错误详情）==="
$l | Select-String -Pattern 'connect|Connect|pipe|Pipe|WaitNamed|CreateFile|ERROR|failed|Failed|open|Open|err=' | Select-Object -Last 25 | ForEach-Object {
    $line = $_.Line
    $ts = ""
    if ($line -match '\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]') { $ts = $Matches[1] }
    # 取最后一个 ] 之后的纯消息
    $idx = $line.LastIndexOf(']')
    $msg = if ($idx -ge 0) { $line.Substring($idx + 1).Trim() } else { $line }
    Write-Host ($ts + "  " + $msg)
}
