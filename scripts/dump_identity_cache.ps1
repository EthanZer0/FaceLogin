[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# ---- 1. Auth identity: what CloudAP/MSA knows about the current account ----
Write-Host "===== AuthIdentity? (CloudAP) ====="
$base = 'HKLM:\SOFTWARE\Microsoft\IdentityStore'
$k = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication'
if (Test-Path $k) {
    Get-ChildItem $k -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("  auth: " + (Split-Path $_.PSPath -Leaf)) }
}

# ---- 2. IdentityCRL (classic MSA cache) ----
Write-Host ""
Write-Host "===== IdentityCRL\StoredIdentities ====="
$icrl = 'HKCU:\SOFTWARE\Microsoft\IdentityCRL\StoredIdentities'
if (Test-Path $icrl) {
    Get-ChildItem $icrl | ForEach-Object {
        $ident = Split-Path $_.PSPath -Leaf
        Write-Host ("  identity: " + $ident)
        $p = Get-ItemProperty $_.PSPath
        if ($p) { $p.PSObject.Properties | ForEach-Object { if ($_.Name -notmatch '^PS') { Write-Host ("     " + $_.Name + " = [" + $_.Value + "]") } } }
    }
} else { Write-Host "(absent)" }

# ---- 3. TokenLinked / LSA linked SID mapping ----
Write-Host ""
Write-Host "===== HKLM LSA / token linked ====="
foreach ($lk in @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers',
    'HKLM:\SYSTEM\CurrentControlSet\Control\Lsa\TokenLinked',
    'HKLM:\SYSTEM\CurrentControlSet\Control\Lsa\LinkedTokens')) {
    Write-Host ("-- " + $lk + " --")
    if (Test-Path $lk) { Get-ChildItem $lk -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("  " + (Split-Path $_.PSPath -Leaf)) } } else { Write-Host "  (absent)" }
}

# ---- 4. The authoritative per-SID provider values under Cache for ME (dump raw subkey values) ----
Write-Host ""
Write-Host "===== Dump Cache\mySID\IdentityStore\{providers} raw (bypassed Get-ItemProperty) ====="
$cache = 'HKLM:\SOFTWARE\Microsoft\IdentityStore\Cache\S-1-5-21-3537359410-1215674007-1769204640-1001'
if (Test-Path $cache) {
    reg query $cache /s 2>&1 | Select-Object -First 60
} else { Write-Host "(no cache)" }

# ---- 5. Where does the MSA shadow SID live in the token? Try AccountSid of linked provider ----
Write-Host ""
Write-Host "===== net user Sylvan (full) ====="
net user Sylvan 2>&1
