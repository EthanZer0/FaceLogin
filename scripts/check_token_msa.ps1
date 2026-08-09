[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$out = @()
$out += "TokenName = " + $me.Name
$out += "TokenSid  = " + $me.User.Value
$p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$out += "IsElevated = " + $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$out += "--- group SIDs ---"
$me.Groups | ForEach-Object {
    try { $t = $_.Translate([System.Security.Principal.NTAccount]).Value } catch { $t = "(translate failed)" }
    $out += "  " + $_.Value + "  " + $t
}
$out += "--- DONE ---"
$out | Out-File -FilePath 'D:\WorkDir\Facelogin\scripts\elev_out.txt' -Encoding UTF8
