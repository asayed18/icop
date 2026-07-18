[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$VlcRoot,
    [string]$ReleaseRoot,
    [string]$Version,
    [switch]$StopVlc,
    [switch]$SkipCacheGeneration,
    [switch]$NoElevate,
    [string]$StatusPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

trap {
    $details = ($_ | Out-String) + [Environment]::NewLine + $_.ScriptStackTrace
    if (-not [string]::IsNullOrWhiteSpace($StatusPath)) {
        [System.IO.File]::WriteAllText($StatusPath, $details)
    }
    else {
        [Console]::Error.WriteLine($details)
    }
    exit 1
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Test-ChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Child
    )

    $parentPath = (Get-FullPath $Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $childPath = Get-FullPath $Child
    return $childPath.StartsWith(
        $parentPath,
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function Get-PeArchitecture {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a Windows PE executable: $Path"
        }
        $stream.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $reader.ReadInt32()
        $stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Invalid PE signature: $Path"
        }
        switch ($reader.ReadUInt16()) {
            0x014C { return 'x86' }
            0x8664 { return 'x86_64' }
            0xAA64 { return 'arm64' }
            default { throw "Unsupported VLC executable architecture: $Path" }
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Add-VlcCandidate {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Candidates,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }
    $candidate = $Path.Trim().Trim('"')
    if ($candidate -match '\.exe(,\d+)?$') {
        $candidate = Split-Path ($candidate -replace ',\d+$', '') -Parent
    }
    try {
        $candidate = Get-FullPath $candidate
    }
    catch {
        return
    }
    if ((Test-Path -LiteralPath (Join-Path $candidate 'vlc.exe') -PathType Leaf) -and
        -not $Candidates.Contains($candidate)) {
        $Candidates.Add($candidate)
    }
}

function Find-VlcInstallations {
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    $registryPaths = @(
        'HKLM:\SOFTWARE\VideoLAN\VLC',
        'HKLM:\SOFTWARE\WOW6432Node\VideoLAN\VLC',
        'HKCU:\SOFTWARE\VideoLAN\VLC'
    )

    foreach ($registryPath in $registryPaths) {
        if (-not (Test-Path -LiteralPath $registryPath)) {
            continue
        }
        $properties = Get-ItemProperty -LiteralPath $registryPath
        $installDirProperty = $properties.PSObject.Properties['InstallDir']
        $installLocationProperty = $properties.PSObject.Properties['InstallLocation']
        if ($installDirProperty) {
            Add-VlcCandidate $candidates $installDirProperty.Value
        }
        if ($installLocationProperty) {
            Add-VlcCandidate $candidates $installLocationProperty.Value
        }
        Add-VlcCandidate $candidates (Get-Item -LiteralPath $registryPath).GetValue('')
    }

    $uninstallRoots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($uninstallRoot in $uninstallRoots) {
        Get-ItemProperty -Path $uninstallRoot -ErrorAction SilentlyContinue |
            Where-Object {
                $displayName = $_.PSObject.Properties['DisplayName']
                $displayName -and $displayName.Value -like 'VLC media player*'
            } |
            ForEach-Object {
                $installLocation = $_.PSObject.Properties['InstallLocation']
                $displayIcon = $_.PSObject.Properties['DisplayIcon']
                if ($installLocation) {
                    Add-VlcCandidate $candidates $installLocation.Value
                }
                if ($displayIcon) {
                    Add-VlcCandidate $candidates $displayIcon.Value
                }
            }
    }

    Get-Command vlc.exe -All -ErrorAction SilentlyContinue | ForEach-Object {
        Add-VlcCandidate $candidates (Split-Path $_.Source -Parent)
    }
    Add-VlcCandidate $candidates (Join-Path $env:ProgramFiles 'VideoLAN\VLC')
    if (${env:ProgramFiles(x86)}) {
        Add-VlcCandidate $candidates (Join-Path ${env:ProgramFiles(x86)} 'VideoLAN\VLC')
    }

    return $candidates
}

function ConvertTo-VersionKey {
    param([Parameter(Mandatory = $true)][string]$Value)

    $core = ($Value.TrimStart('v') -split '-', 2)[0]
    try {
        return [version]$core
    }
    catch {
        return [version]'0.0.0.0'
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $algorithm.ComputeHash($stream)
        return ([System.BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Find-Release {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [string]$RequestedVersion
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Release root does not exist: $Root"
    }

    $requested = if ([string]::IsNullOrWhiteSpace($RequestedVersion)) {
        ''
    }
    else {
        $RequestedVersion.TrimStart('v')
    }
    $candidates = foreach ($versionDirectory in Get-ChildItem -LiteralPath $Root -Directory) {
        $manifestPath = Join-Path $versionDirectory.FullName 'windows\release.json'
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            continue
        }
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ($manifest.name -ne 'icop' -or
            $manifest.platform -ne 'windows' -or
            $manifest.architecture -ne $Architecture) {
            continue
        }
        if ($requested -and $manifest.version -ne $requested) {
            continue
        }
        [pscustomobject]@{
            Directory = Split-Path $manifestPath -Parent
            Manifest = $manifest
            ManifestPath = $manifestPath
            VersionKey = ConvertTo-VersionKey $manifest.version
            IsStable = $manifest.version -notmatch '-'
        }
    }

    $release = $candidates |
        Sort-Object -Property @(
            @{ Expression = 'VersionKey'; Descending = $true },
            @{ Expression = 'IsStable'; Descending = $true }
        ) |
        Select-Object -First 1
    if (-not $release) {
        $versionText = if ($requested) { " version $requested" } else { '' }
        throw "No icop Windows $Architecture release$versionText was found under $Root."
    }
    return $release
}

function Get-VerifiedPayload {
    param([Parameter(Mandatory = $true)]$Release)

    $payload = New-Object 'System.Collections.Generic.List[object]'
    $prefix = 'plugins/video_filter/'
    foreach ($entry in $Release.Manifest.files) {
        $relativePath = [string]$entry.path
        if (-not $relativePath.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
            throw "Release manifest contains an unexpected path: $relativePath"
        }
        $source = Get-FullPath (Join-Path $Release.Directory ($relativePath -replace '/', '\'))
        if (-not (Test-ChildPath $Release.Directory $source)) {
            throw "Release path escapes its platform directory: $relativePath"
        }
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Release payload file is missing: $source"
        }
        $actualHash = Get-Sha256 $source
        $expectedHash = ([string]$entry.sha256).ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Release checksum mismatch: $relativePath"
        }
        $payloadRelativePath = $relativePath.Substring($prefix.Length).Replace(
            [char]'/',
            [System.IO.Path]::DirectorySeparatorChar
        )
        $payload.Add([pscustomobject]@{
            Source = $source
            RelativePath = $payloadRelativePath
            Sha256 = $expectedHash
        })
    }
    if ($payload.Count -eq 0) {
        throw "Release payload is empty: $($Release.ManifestPath)"
    }
    return $payload
}

function Test-DirectoryWritable {
    param([Parameter(Mandatory = $true)][string]$Path)

    $probe = Join-Path $Path ".icop-write-test-$PID.tmp"
    try {
        $stream = [System.IO.File]::Open(
            $probe,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )
        $stream.Dispose()
        Remove-Item -LiteralPath $probe -Force
        return $true
    }
    catch {
        if (Test-Path -LiteralPath $probe) {
            Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        }
        return $false
    }
}

function Invoke-ElevatedInstaller {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedVlcRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedReleaseRoot
    )

    $hostPath = (Get-Process -Id $PID).Path
    $statusFile = Join-Path ([System.IO.Path]::GetTempPath()) "icop-elevated-install-$PID.log"
    if (Test-Path -LiteralPath $statusFile) {
        Remove-Item -LiteralPath $statusFile -Force
    }
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-VlcRoot', ('"{0}"' -f $ResolvedVlcRoot),
        '-ReleaseRoot', ('"{0}"' -f $ResolvedReleaseRoot),
        '-NoElevate',
        '-StatusPath', ('"{0}"' -f $statusFile)
    )
    if ($Version) {
        $arguments += @('-Version', ('"{0}"' -f $Version))
    }
    if ($StopVlc) {
        $arguments += '-StopVlc'
    }
    if ($SkipCacheGeneration) {
        $arguments += '-SkipCacheGeneration'
    }

    Write-Host "Administrator access is required to update $ResolvedVlcRoot."
    $process = Start-Process -FilePath $hostPath -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    $status = if (Test-Path -LiteralPath $statusFile) {
        Get-Content -LiteralPath $statusFile -Raw
    }
    else {
        ''
    }
    if (Test-Path -LiteralPath $statusFile) {
        Remove-Item -LiteralPath $statusFile -Force
    }
    if ($process.ExitCode -ne 0 -or $status -notmatch '^SUCCESS') {
        $details = if ($status) { "`n$status" } else { '' }
        throw "Elevated icop installation failed with exit code $($process.ExitCode).$details"
    }
    Write-Host $status.Trim()
}

if ($env:OS -ne 'Windows_NT') {
    throw 'This installer currently supports Windows VLC installations.'
}

if ([string]::IsNullOrWhiteSpace($ReleaseRoot)) {
    $scriptDirectory = Split-Path $PSCommandPath -Parent
    $ReleaseRoot = Join-Path $scriptDirectory '..\releases'
}
$ReleaseRoot = Get-FullPath $ReleaseRoot
if ($VlcRoot) {
    $VlcRoot = Get-FullPath $VlcRoot
    if (-not (Test-Path -LiteralPath (Join-Path $VlcRoot 'vlc.exe') -PathType Leaf)) {
        throw "vlc.exe was not found under: $VlcRoot"
    }
}
else {
    $installations = @(Find-VlcInstallations)
    if ($installations.Count -eq 0) {
        throw 'No installed VLC was found. Pass -VlcRoot with the VLC installation directory.'
    }
    if ($installations.Count -gt 1) {
        Write-Verbose "Detected VLC installations: $($installations -join ', ')"
    }
    $VlcRoot = $installations[0]
}

$vlcExe = Join-Path $VlcRoot 'vlc.exe'
$architecture = Get-PeArchitecture $vlcExe
$release = Find-Release $ReleaseRoot $architecture $Version
$payload = @(Get-VerifiedPayload $release)
$pluginDirectory = Join-Path $VlcRoot 'plugins\video_filter'
if (-not (Test-Path -LiteralPath $pluginDirectory -PathType Container)) {
    throw "VLC video filter directory does not exist: $pluginDirectory"
}

$runningVlc = @(
    Get-Process vlc -ErrorAction SilentlyContinue |
        Where-Object {
            try {
                $_.Path -and (Get-FullPath $_.Path) -eq (Get-FullPath $vlcExe)
            }
            catch {
                $false
            }
        }
)
if ($runningVlc.Count -gt 0 -and -not $StopVlc -and -not $WhatIfPreference) {
    throw 'VLC is running. Close it or rerun with -StopVlc.'
}

$action = "Install icop $($release.Manifest.version) ($architecture) and regenerate the VLC plugin cache"
if (-not $PSCmdlet.ShouldProcess($VlcRoot, $action)) {
    return
}

if (-not (Test-DirectoryWritable $pluginDirectory)) {
    if ($NoElevate) {
        throw "VLC plugin directory is not writable: $pluginDirectory"
    }
    Invoke-ElevatedInstaller $VlcRoot $ReleaseRoot
    return
}

if ($runningVlc.Count -gt 0) {
    $runningVlc | Stop-Process -Force
    $runningVlc | Wait-Process -ErrorAction SilentlyContinue
}

$legacyNames = @(
    'libnsfw_filter_plugin.dll',
    'nsfw_filter_core.dll',
    'nsfw_filter_impl.dll',
    'nsfw_filter_core_test.exe',
    'freepik-nsfw.onnx',
    'nsfw-classifier-int8.onnx',
    'dml\freepik-nsfw.onnx',
    'dml\nsfw-classifier-int8.onnx'
)
$managedDestinations = New-Object 'System.Collections.Generic.List[string]'
foreach ($entry in $payload) {
    $destination = Get-FullPath (Join-Path $pluginDirectory $entry.RelativePath)
    if (-not (Test-ChildPath $pluginDirectory $destination)) {
        throw "Install destination escapes VLC's video filter directory: $destination"
    }
    if (-not $managedDestinations.Contains($destination)) {
        $managedDestinations.Add($destination)
    }
}
foreach ($legacyName in $legacyNames) {
    $managedDestinations.Add((Join-Path $pluginDirectory $legacyName))
}

$backupRoot = Get-FullPath (Join-Path ([System.IO.Path]::GetTempPath()) "icop-install-backup-$PID")
if (-not (Test-ChildPath ([System.IO.Path]::GetTempPath()) $backupRoot)) {
    throw "Backup path escaped the temporary directory: $backupRoot"
}
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
$backups = @{}
$createdFiles = New-Object 'System.Collections.Generic.List[string]'

try {
    foreach ($destination in $managedDestinations) {
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            $backup = Join-Path $backupRoot ([System.Guid]::NewGuid().ToString('N'))
            Copy-Item -LiteralPath $destination -Destination $backup -Force
            $backups[$destination] = $backup
        }
        else {
            $createdFiles.Add($destination)
        }
    }

    foreach ($entry in $payload) {
        $destination = Get-FullPath (Join-Path $pluginDirectory $entry.RelativePath)
        $destinationDirectory = Split-Path $destination -Parent
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        Copy-Item -LiteralPath $entry.Source -Destination $destination -Force
    }
    foreach ($legacyName in $legacyNames) {
        $legacyPath = Join-Path $pluginDirectory $legacyName
        if (Test-Path -LiteralPath $legacyPath -PathType Leaf) {
            Remove-Item -LiteralPath $legacyPath -Force
        }
    }

    foreach ($entry in $payload) {
        $destination = Get-FullPath (Join-Path $pluginDirectory $entry.RelativePath)
        $actualHash = Get-Sha256 $destination
        if ($actualHash -ne $entry.Sha256) {
            throw "Installed file checksum mismatch: $destination"
        }
    }

    if (-not $SkipCacheGeneration) {
        $cacheGenerator = Join-Path $VlcRoot 'vlc-cache-gen.exe'
        if (-not (Test-Path -LiteralPath $cacheGenerator -PathType Leaf)) {
            throw "VLC cache generator was not found: $cacheGenerator"
        }
        & $cacheGenerator (Join-Path $VlcRoot 'plugins')
        if ($LASTEXITCODE -ne 0) {
            throw "VLC cache generation failed with exit code $LASTEXITCODE."
        }
    }
}
catch {
    foreach ($destination in $createdFiles) {
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($destination in $backups.Keys) {
        $destinationDirectory = Split-Path $destination -Parent
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        Copy-Item -LiteralPath $backups[$destination] -Destination $destination -Force
    }
    throw
}
finally {
    if (Test-Path -LiteralPath $backupRoot -PathType Container) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
    }
}

$result = [pscustomobject]@{
    Product = 'icop'
    Version = $release.Manifest.version
    Architecture = $architecture
    Runtime = $release.Manifest.runtime
    VlcRoot = $VlcRoot
    PluginDirectory = $pluginDirectory
    FilesInstalled = $payload.Count
    CacheRegenerated = -not $SkipCacheGeneration
}
if (-not [string]::IsNullOrWhiteSpace($StatusPath)) {
    [System.IO.File]::WriteAllText(
        $StatusPath,
        "SUCCESS icop $($release.Manifest.version) installed to $VlcRoot ($($payload.Count) files, cache regenerated: $(-not $SkipCacheGeneration))"
    )
}
$result
