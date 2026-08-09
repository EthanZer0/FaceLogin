[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 1. Every IdentityStore provider + its Name2Sid mappings
Write-Host "===== ALL IdentityStore LogonCache providers ====="
$base = 'HKLM:\SOFTWARE\Microsoft\IdentityStore\LogonCache'
if (Test-Path $base) {
    Get-ChildItem $base | ForEach-Object {
        $provider = Split-Path $_.PSPath -Leaf
        Write-Host ("PROVIDER: " + $provider)
        $n2s = $_.PSPath + '\Name2Sid'
        if (Test-Path $n2s) {
            Get-ChildItem $n2s | ForEach-Object {
                $p = Get-ItemProperty $_.PSPath
                Write-Host ("   IdentityName=[" + $p.IdentityName + "] Sid=[" + $p.Sid + "]")
            }
        }
    }
} else {
    Write-Host "(no LogonCache)"
}

# 2. Does any entry map to the current user's SID?
Write-Host "===== Current user ====="
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent()
Write-Host ("TokenName = " + $me.Name)
Write-Host ("TokenSid  = " + $me.User.Value)

# 3. What domain does Windows report for this SID? (machine=local, MicrosoftAccount=MSA)
$sidObj = [System.Security.Principal.SecurityIdentifier]::new($me.User.Value)
$acct = $sidObj.Translate([System.Security.Principal.NTAccount])
Write-Host ("Translate (lookup) = " + $acct.Value)

# 4. net user -- SAM accounts (MSA/cloud accounts do NOT appear here)
Write-Host "===== net user (SAM accounts) ====="
net user 2>&1 | Select-Object -First 20

# 5. GetUserNameExW NameUserPrincipal probe
Write-Host "===== GetUserNameExW(NameUserPrincipal) probe ====="
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UPNProbe {
    [DllImport("secur32.dll", CharSet=CharSet.Unicode)]
    public static extern bool GetUserNameExW(int nameFormat, System.Text.StringBuilder sb, ref uint size);
    public static string Probe() {
        uint size = 64;
        var sb = new System.Text.StringBuilder((int)size);
        if (!GetUserNameExW(8, sb, ref size)) {
            int err = Marshal.GetLastWin32Error();
            if (err == 122 && size > 64) {
                sb = new System.Text.StringBuilder((int)size);
                if (GetUserNameExW(8, sb, ref size)) return sb.ToString();
                return "(fail2 err=" + Marshal.GetLastWin32Error() + ")";
            }
            return "(fail1 err=" + err + " size=" + size + ")";
        }
        return sb.ToString();
    }
}
'@
Write-Host ("NameUserPrincipal = [" + [UPNProbe]::Probe() + "]")
