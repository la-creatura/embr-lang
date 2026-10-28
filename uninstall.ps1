# uninstall.ps1
# takes out everything install.ps1 put on the system. the install keeps a copy of this script at %LOCALAPPDATA%\embr\uninstall.ps1.
#
# usage
#   powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\embr\uninstall.ps1"
#   powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\embr\uninstall.ps1" -Purge    # also delete the extra folder
#
# good to know
#   - your own files in the extra folder (plugins\ and modules\) are kept unless you pass -Purge.
#   - PATH and EMBR_PATH only lose the entries the installer added, the rest of them is not touched.
#   - close every running embr first, windows will not delete a program that is running.

param([switch]$Purge)

$ErrorActionPreference = 'Stop'

function Say($msg) { Write-Host "embr: $msg" }

# the folder this script was copied to, and not the one it is run from (a copy in the release is not an install)
$ehome = if ($env:EMBR_INSTALL_HOME) { $env:EMBR_INSTALL_HOME } else { Join-Path $env:LOCALAPPDATA 'embr' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ((Test-Path (Join-Path $here 'current')) -and -not $env:EMBR_INSTALL_HOME) { $ehome = $here }
if (-not (Test-Path -LiteralPath $ehome)) { Say "nothing installed in $ehome"; }

# a folder cannot be deleted while it is the current folder
Set-Location ([IO.Path]::GetTempPath())

$bin = Join-Path $ehome 'current\bin'
$extra = Join-Path $ehome 'extra'

function Remove-FromList($name, $entry) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    try {
        $cur = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($null -eq $cur) { return }
        $parts = @($cur -split ';' | Where-Object { $_ -ne '' -and $_ -ne $entry })
        if ($parts.Count -eq 0) { $key.DeleteValue($name, $false) }
        else {
            $kind = if ($name -eq 'Path') { 'ExpandString' } else { 'String' }
            $key.SetValue($name, ($parts -join ';'), $kind)
        }
    } finally { $key.Close() }
}

Remove-FromList 'Path' $bin
Remove-FromList 'EMBR_PATH' $extra

# the file types. a type is only removed when it still points at embr (you may have picked another program since)
$classes = 'HKCU:\Software\Classes'
foreach ($t in @(@('.embr', 'embr.script'), @('.vmbr', 'embr.bytecode'), @('.pmbr', 'embr.package'))) {
    $ext = "$classes\$($t[0])"
    if ((Test-Path $ext) -and ((Get-ItemProperty $ext).'(default)' -eq $t[1])) { Remove-Item -Recurse -Force $ext }
    if (Test-Path "$classes\$($t[1])") { Remove-Item -Recurse -Force "$classes\$($t[1])" }
}
Remove-Item -Recurse -Force 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\embr' -ErrorAction SilentlyContinue

# tell open windows and explorer
Add-Type -Namespace Embr -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
[DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr a, IntPtr b);
'@ -ErrorAction SilentlyContinue
try {
    $r = [UIntPtr]::Zero
    [void][Embr.Native]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 3000, [ref]$r)
    [Embr.Native]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
} catch { }

# the files. the junction goes first, so removing it never follows it into the version folder
$failed = $false
$cur = Join-Path $ehome 'current'
if (Test-Path -LiteralPath $cur) { (Get-Item -LiteralPath $cur).Delete() }
foreach ($d in Get-ChildItem -LiteralPath $ehome -Directory -ErrorAction SilentlyContinue) {
    if ($d.Name -eq 'extra') { continue }
    try { Remove-Item -LiteralPath $d.FullName -Recurse -Force } catch { $failed = $true; Say "could not remove $($d.FullName), is embr running?" }
}
if ($Purge) {
    Remove-Item -LiteralPath $extra -Recurse -Force -ErrorAction SilentlyContinue
} elseif (Test-Path -LiteralPath $extra) {
    Say "kept $extra (your own plugins and modules), pass -Purge to delete it"
}
if (Test-Path -LiteralPath $ehome) {
    # this script is one of the files in it; windows lets a running script be deleted
    Remove-Item -LiteralPath (Join-Path $ehome 'uninstall.ps1') -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem -LiteralPath $ehome -Force -ErrorAction SilentlyContinue)) { Remove-Item -LiteralPath $ehome -Force -ErrorAction SilentlyContinue }
}

if ($failed) { Say 'done, but some files are left'; exit 1 }
Say 'done. embr is removed. open terminals keep the old PATH until you close them'
