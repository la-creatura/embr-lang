# install.ps1
# installs an embr release on windows: the program, the `embr` command on your PATH, EMBR_PATH, and the icon and
# double-click for .embr, .vmbr and .pmbr files. uninstall.ps1 (it is copied next to the install) takes all of it out again.
#
# usage
#   irm https://raw.githubusercontent.com/la-creatura/embr-lang/main/install.ps1 | iex
#   .\install.ps1 -Version 1.0.0
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/la-creatura/embr-lang/main/install.ps1))) -NoFileTypes
#
# options
#   -Version X.Y.Z    install this release instead of the latest
#   -From FILE        install from a release zip you already have, nothing is downloaded
#   -NoModifyPath     leave PATH and EMBR_PATH alone, you set them yourself
#   -NoFileTypes      skip the icons and double-click for .embr, .vmbr and .pmbr
#
# good to know
#   - it installs for your user only, in %LOCALAPPDATA%\embr. it needs no admin rights.
#   - only 64 bit windows (x86_64) has a release for now.
#   - EMBR_PATH points at %LOCALAPPDATA%\embr\extra, a folder for your own plugins\ and modules\. uninstall keeps it
#     unless you pass -Purge.
#   - open a new terminal after installing, a terminal that was already open does not see the new PATH.
#   - EMBR_INSTALL_BASE replaces the release url and EMBR_INSTALL_HOME replaces %LOCALAPPDATA%\embr. they are there for tests.

param(
    [string]$Version = '',
    [string]$From = '',
    [switch]$NoModifyPath,
    [switch]$NoFileTypes
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repo = 'la-creatura/embr-lang'
$base = if ($env:EMBR_INSTALL_BASE) { $env:EMBR_INSTALL_BASE } else { "https://github.com/$repo/releases" }

function Say($msg) { Write-Host "embr: $msg" }
function Die($msg) { Write-Host "embr: $msg" -ForegroundColor Red; throw "embr: $msg" }

if ($PSVersionTable.PSVersion.Major -lt 5) { Die 'needs powershell 5 or newer' }
if ($env:OS -ne 'Windows_NT') { Die 'this installer is for windows. on linux use install.sh' }
if (-not [Environment]::Is64BitOperatingSystem) { Die 'only 64 bit windows has a release so far' }
# an arm machine runs the x86_64 build through emulation, so the release name is the same
$arch = 'x86_64'

$ehome = if ($env:EMBR_INSTALL_HOME) { $env:EMBR_INSTALL_HOME } else { Join-Path $env:LOCALAPPDATA 'embr' }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("embr-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    # get the zip
    if ($From) {
        if (-not (Test-Path -LiteralPath $From -PathType Leaf)) { Die "no such file: $From" }
        $zip = (Resolve-Path -LiteralPath $From).Path
        $sumFile = "$zip.sha256"
        if (Test-Path -LiteralPath $sumFile) {
            $want = ((Get-Content -LiteralPath $sumFile -TotalCount 1) -split '\s+')[0].ToLower()
            if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower() -ne $want) { Die "the checksum of $From does not match" }
        }
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        if (-not $Version) {
            try {
                $req = [Net.HttpWebRequest]::Create("$base/latest")
                $req.Method = 'HEAD'
                $resp = $req.GetResponse()
                $tag = $resp.ResponseUri.AbsoluteUri.Split('/')[-1]
                $resp.Close()
            } catch { Die "could not reach $base" }
            if ($tag -notmatch '^v\d') { Die "no release found at $base yet" }
            $Version = $tag.Substring(1)
        }
        $Version = $Version.TrimStart('v')
        $name = "embr-$Version-windows-$arch.zip"
        $zip = Join-Path $tmp $name
        Say "downloading $name"
        try { Invoke-WebRequest -UseBasicParsing -Uri "$base/download/v$Version/$name" -OutFile $zip }
        catch { Die "could not download $base/download/v$Version/$name (is there a windows build of that version?)" }
        $sumFile = "$zip.sha256"
        try { Invoke-WebRequest -UseBasicParsing -Uri "$base/download/v$Version/$name.sha256" -OutFile $sumFile }
        catch { Die "could not download the checksum file for $name" }
        $want = ((Get-Content -LiteralPath $sumFile -TotalCount 1) -split '\s+')[0].ToLower()
        if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower() -ne $want) { Die "the checksum of $name does not match, not installing it" }
    }

    $x = Join-Path $tmp 'x'
    try { Expand-Archive -LiteralPath $zip -DestinationPath $x -Force } catch { Die "could not unpack $zip" }
    $src = Get-ChildItem -LiteralPath $x -Directory | Select-Object -First 1
    if (-not $src -or -not (Test-Path (Join-Path $src.FullName 'bin\embr.exe'))) { Die "$zip does not look like an embr release (no bin\embr.exe)" }
    $src = $src.FullName

    # the new build has to start here, or the install would leave a broken command behind
    $out = & (Join-Path $src 'bin\embr.exe') --version 2>$null
    if ($out -notmatch '^embr (\S+)') { Die 'this build of embr does not start here' }
    $ver = $Matches[1]
    Say "installing embr $ver into $ehome"

    # the program. each version has its own folder and `current` is a junction (a folder link, needs no admin) to the newest
    New-Item -ItemType Directory -Force -Path $ehome | Out-Null
    $dest = Join-Path $ehome $ver
    $cur = Join-Path $ehome 'current'
    $old = $null
    if (Test-Path -LiteralPath $cur) { $old = (Get-Item -LiteralPath $cur).Target; if ($old -is [array]) { $old = $old[0] } }
    if (Test-Path -LiteralPath $dest) {
        try { Remove-Item -LiteralPath $dest -Recurse -Force } catch { Die "could not replace $dest, is embr running? close it and try again" }
    }
    Move-Item -LiteralPath $src -Destination $dest
    if (Test-Path -LiteralPath $cur) { (Get-Item -LiteralPath $cur).Delete() }
    New-Item -ItemType Junction -Path $cur -Target $dest | Out-Null
    if ($old -and ($old -ne $dest) -and (Split-Path $old -Parent) -eq $ehome) {
        Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
    }

    # your own plugins and modules
    $extra = Join-Path $ehome 'extra'
    New-Item -ItemType Directory -Force -Path (Join-Path $extra 'plugins'), (Join-Path $extra 'modules') | Out-Null

    $bin = Join-Path $cur 'bin'
    $exe = Join-Path $bin 'embr.exe'
    Copy-Item -LiteralPath (Join-Path $dest 'share\embr\uninstall.ps1') -Destination (Join-Path $ehome 'uninstall.ps1') -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $ehome 'uninstall.ps1'))) {
        Say 'this release has no uninstall.ps1, remove the folder and the registry entries by hand to uninstall'
    }

    # tell open windows that the environment changed
    function Broadcast-Env {
        if (-not ('Embr.Native' -as [type])) {
            Add-Type -Namespace Embr -Name Native -MemberDefinition '[DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);'
        }
        $r = [UIntPtr]::Zero
        [void][Embr.Native]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 3000, [ref]$r)
    }

    # PATH and EMBR_PATH, in the user's environment. the value type is kept (expandable stays expandable)
    function Add-ToList($name, $entry) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
        try {
            $cur = $key.GetValue($name, '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $parts = @($cur -split ';' | Where-Object { $_ -ne '' })
            if ($parts -notcontains $entry) {
                $new = (@($entry) + $parts) -join ';'
                $kind = if ($name -eq 'Path') { 'ExpandString' } else { 'String' }
                $key.SetValue($name, $new, $kind)
            }
        } finally { $key.Close() }
    }

    $pathHelp = $false
    if (-not $NoModifyPath) {
        Add-ToList 'Path' $bin
        Add-ToList 'EMBR_PATH' $extra
        Broadcast-Env
    } else {
        $pathHelp = $true
    }

    # icons and double-click, under HKCU\Software\Classes so no admin is needed. embr.exe carries the icon.
    # a double-clicked script runs in a console that stays open, so its output can be read.
    if (-not $NoFileTypes) {
        $classes = 'HKCU:\Software\Classes'
        $types = @(
            @{ ext = '.embr'; id = 'embr.script';   desc = 'embr script' },
            @{ ext = '.vmbr'; id = 'embr.bytecode'; desc = 'embr compiled script' },
            @{ ext = '.pmbr'; id = 'embr.package';  desc = 'embr package' }
        )
        $run = "cmd.exe /k `"`"$exe`" `"%1`"`""
        foreach ($t in $types) {
            New-Item -Force -Path "$classes\$($t.ext)" | Out-Null
            Set-ItemProperty -Path "$classes\$($t.ext)" -Name '(default)' -Value $t.id
            New-Item -Force -Path "$classes\$($t.id)" | Out-Null
            Set-ItemProperty -Path "$classes\$($t.id)" -Name '(default)' -Value $t.desc
            New-Item -Force -Path "$classes\$($t.id)\DefaultIcon" | Out-Null
            Set-ItemProperty -Path "$classes\$($t.id)\DefaultIcon" -Name '(default)' -Value "`"$exe`",0"
            New-Item -Force -Path "$classes\$($t.id)\shell\open\command" | Out-Null
            if ($t.ext -eq '.pmbr') {
                # a package is not run, it is looked at
                Set-ItemProperty -Path "$classes\$($t.id)\shell\open\command" -Name '(default)' -Value "cmd.exe /k `"`"$exe`" package `"%1`"`""
            } else {
                Set-ItemProperty -Path "$classes\$($t.id)\shell\open\command" -Name '(default)' -Value $run
            }
        }
        # tell explorer that the file types changed
        Add-Type -Namespace Embr -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr a, IntPtr b);' -ErrorAction SilentlyContinue
        try { [Embr.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero) } catch { }
    }

    # the entry in "installed apps"
    $un = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\embr'
    New-Item -Force -Path $un | Out-Null
    Set-ItemProperty -Path $un -Name 'DisplayName' -Value 'embr'
    Set-ItemProperty -Path $un -Name 'DisplayVersion' -Value $ver
    Set-ItemProperty -Path $un -Name 'Publisher' -Value 'la-creatura'
    Set-ItemProperty -Path $un -Name 'InstallLocation' -Value $ehome
    Set-ItemProperty -Path $un -Name 'DisplayIcon' -Value "$exe,0"
    Set-ItemProperty -Path $un -Name 'URLInfo' -Value "https://github.com/$repo"
    Set-ItemProperty -Path $un -Name 'NoModify' -Value 1 -Type DWord
    Set-ItemProperty -Path $un -Name 'NoRepair' -Value 1 -Type DWord
    Set-ItemProperty -Path $un -Name 'UninstallString' -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $ehome 'uninstall.ps1')`""

    Say "done. installed embr $ver"
    if ($pathHelp) {
        Say "add $bin to your PATH and set EMBR_PATH to $extra to use it from a terminal"
    } else {
        Say 'open a new terminal to use the embr command'
    }
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
