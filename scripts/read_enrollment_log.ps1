[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$c = Get-Content 'D:\Fold\FaceLogin\log\enrollment.log' -Encoding Unicode
Write-Host ('total lines: ' + $c.Count)
Write-Host '=== GetLinkedAccountUpn / account_identity related ==='
$c | Select-String -Pattern 'GetLinkedAccountUpn|QueryUserNamePrincipal|FindMsaShadowSidInToken|MSA shadow|MSA identity|direct UPN|linked MSA|scanning token' | Select-Object -Last 30
Write-Host '=== last 8 lines ==='
$c | Select-Object -Last 8
