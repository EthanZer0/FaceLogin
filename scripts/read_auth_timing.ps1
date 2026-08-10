[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$c = Get-Content 'D:\Fold\FaceLogin\log\service.log' -Encoding Unicode
Write-Host ("总行数: " + $c.Count)
Write-Host "=== 最近几次认证（带行内容）==="
$c | Select-String -Pattern 'Starting face authentication|Warmup: exposure|Face matched: Sylvan|Match lost|Liveness passed|Credentials sent|Heavy models unloaded|DirectShow webcam' | Select-Object -Last 55 | ForEach-Object {
    $line = $_.Line
    $ts = ""
    if ($line -match '\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})') { $ts = $Matches[1] }
    # 提取事件描述：取最后一个 ] 之后的内容
    $desc = $line
    if ($line -match '\]\s*$') { }
    # 提取函数名+消息：找 'facelogin::' 后的部分
    $msg = ""
    if ($line -match '\]\s*\[\w+\]\s*\[\d+\]\s*\[([^\]]+)\]\s*(.*)$') {
        $msg = $Matches[1] + " | " + $Matches[2]
    } elseif ($line -match '\]\s*\[INFO\]') {
        $msg = $line.Substring($line.LastIndexOf(']') + 1).Trim()
    }
    Write-Host ($ts + "  " + $msg)
}
