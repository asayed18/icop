param(
    [string]$VlcRoot = (Join-Path $PSScriptRoot '..\vlc-portable'),
    [string]$VideoPath = (Join-Path $PSScriptRoot '..\sample.mp4'),
    [string]$PluginPath = (Join-Path $PSScriptRoot '..\build-ninja\libicop_plugin.dll'),
    [string]$CorePath = (Join-Path $PSScriptRoot '..\build-ninja\icop_core.dll'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build-ninja\d3d11-runtime-check'),
    [string]$FfmpegPath = 'ffmpeg.exe'
)

$ErrorActionPreference = 'Stop'

$vlc = Join-Path $VlcRoot 'vlc.exe'
$cacheGenerator = Join-Path $VlcRoot 'vlc-cache-gen.exe'
$pluginDirectory = Join-Path $VlcRoot 'plugins\video_filter'
$pluginTree = Join-Path $VlcRoot 'plugins'
$decisionMap = Join-Path $OutputDirectory 'force-block.map'
$seekDecisionMap = Join-Path $OutputDirectory 'seek-block.map'
$nv12Video = Join-Path $OutputDirectory 'fixture-4k60-nv12.mp4'
$p010Video = Join-Path $OutputDirectory 'fixture-1080p-p010.mkv'
$seekVideo = Join-Path $OutputDirectory 'fixture-seek-intra.mp4'
$referencePpm = Join-Path $OutputDirectory 'fixture-4k60-reference.ppm'

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $PluginPath -Destination $pluginDirectory -Force
Copy-Item -LiteralPath $CorePath -Destination $pluginDirectory -Force
& $cacheGenerator (Resolve-Path $pluginTree).Path
if ($LASTEXITCODE -ne 0) {
    throw "vlc-cache-gen failed with exit code $LASTEXITCODE"
}
Set-Content -LiteralPath $decisionMap -Encoding ASCII -Value 'blocked 0 18446744073709551615'
Set-Content -LiteralPath $seekDecisionMap -Encoding ASCII -Value 'blocked 1900 2800'

function Invoke-Ffmpeg {
    param([string[]]$Arguments)

    & $FfmpegPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path -LiteralPath $nv12Video)) {
    Invoke-Ffmpeg @(
        '-y', '-hide_banner', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc2=size=3840x2160:rate=60',
        '-frames:v', '120', '-c:v', 'h264_nvenc', '-preset', 'p1',
        '-pix_fmt', 'yuv420p', $nv12Video
    )
}
if (-not (Test-Path -LiteralPath $p010Video)) {
    Invoke-Ffmpeg @(
        '-y', '-hide_banner', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=30',
        '-vf', 'format=p010le', '-frames:v', '60',
        '-c:v', 'hevc_nvenc', '-preset', 'p1', '-profile:v', 'main10',
        '-pix_fmt', 'p010le', $p010Video
    )
}
if (-not (Test-Path -LiteralPath $seekVideo)) {
    Invoke-Ffmpeg @(
        '-y', '-hide_banner', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30',
        '-frames:v', '150', '-c:v', 'h264_nvenc', '-preset', 'p1',
        '-g', '1', '-pix_fmt', 'yuv420p', $seekVideo
    )
}
if (-not (Test-Path -LiteralPath $referencePpm)) {
    Invoke-Ffmpeg @(
        '-y', '-hide_banner', '-loglevel', 'error',
        '-i', $nv12Video, '-frames:v', '1', '-pix_fmt', 'rgb24',
        '-c:v', 'ppm', $referencePpm
    )
}

function Invoke-NsfwVlc {
    param(
        [string]$Name,
        [string[]]$ExtraArguments,
        [string]$InputVideo = $VideoPath,
        [string]$DumpPrefix = '',
        [switch]$Profile
    )

    $stderr = Join-Path $OutputDirectory "$Name.stderr.log"
    $vlcLog = Join-Path $OutputDirectory "$Name.vlc.log"
    Remove-Item -LiteralPath $stderr, $vlcLog -Force -ErrorAction SilentlyContinue
    if ($DumpPrefix) {
        Get-ChildItem -Path $OutputDirectory -Filter "$DumpPrefix*.ppm" `
            -ErrorAction SilentlyContinue | Remove-Item -Force
        $env:NSFW_DEBUG_DUMP_PREFIX = Join-Path $OutputDirectory $DumpPrefix
    }
    if ($Profile) {
        $env:NSFW_D3D11_PROFILE = '1'
    }

    $inputMrl = ([Uri](Resolve-Path $InputVideo).Path).AbsoluteUri
    $arguments = @(
        '--no-one-instance',
        '--intf=dummy',
        '--play-and-exit',
        '--run-time=2',
        '--file-logging',
        "--logfile=$vlcLog",
        '--log-verbose=2',
        '--avcodec-hw=d3d11va',
        '--vout=direct3d11',
        '--video-filter=icop',
        '--nsfw-debug-overlay=0'
    ) + $ExtraArguments + @($inputMrl)

    try {
        $process = Start-Process -FilePath $vlc -ArgumentList $arguments `
            -RedirectStandardError $stderr -PassThru
        if (-not $process.WaitForExit(60000)) {
            Stop-Process -Id $process.Id -Force
            throw "$Name timed out"
        }
        $process.WaitForExit()
        $process.Refresh()
        if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
            throw "$Name exited with $($process.ExitCode)"
        }
    }
    finally {
        Remove-Item Env:NSFW_D3D11_PROFILE -ErrorAction SilentlyContinue
        Remove-Item Env:NSFW_DEBUG_DUMP_PREFIX -ErrorAction SilentlyContinue
    }

    return Get-Content -LiteralPath $stderr -Raw
}

function Read-Ppm {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    $prefixLength = [Math]::Min($bytes.Length, 256)
    $prefix = [Text.Encoding]::ASCII.GetString($bytes, 0, $prefixLength)
    if ($prefix -notmatch '^P6\s+(\d+)\s+(\d+)\s+255\s') {
        throw "Unsupported PPM header in $Path"
    }
    $headerLength = [Text.Encoding]::ASCII.GetByteCount($Matches[0])
    $width = [int]$Matches[1]
    $height = [int]$Matches[2]
    if ($bytes.Length - $headerLength -ne $width * $height * 3) {
        throw "Unexpected PPM payload size in $Path"
    }
    return [pscustomobject]@{
        Width = $width
        Height = $height
        Offset = $headerLength
        Bytes = $bytes
    }
}

function Get-PpmMean {
    param($Image, [int]$Step = 16)

    [double]$sum = 0
    [long]$samples = 0
    for ($y = 0; $y -lt $Image.Height; $y += $Step) {
        for ($x = 0; $x -lt $Image.Width; $x += $Step) {
            $i = $Image.Offset + ($y * $Image.Width + $x) * 3
            $sum += $Image.Bytes[$i] + $Image.Bytes[$i + 1] + $Image.Bytes[$i + 2]
            $samples += 3
        }
    }
    return $sum / $samples
}

function Get-PpmDetail {
    param($Image, [int]$Step = 8)

    [double]$sum = 0
    [long]$samples = 0
    for ($y = 0; $y -lt $Image.Height - $Step; $y += $Step) {
        for ($x = 0; $x -lt $Image.Width - $Step; $x += $Step) {
            $i = $Image.Offset + ($y * $Image.Width + $x) * 3
            $right = $i + $Step * 3
            $down = $i + $Step * $Image.Width * 3
            for ($channel = 0; $channel -lt 3; ++$channel) {
                $sum += [Math]::Abs([int]$Image.Bytes[$i + $channel] -
                                    [int]$Image.Bytes[$right + $channel])
                $sum += [Math]::Abs([int]$Image.Bytes[$i + $channel] -
                                    [int]$Image.Bytes[$down + $channel])
                $samples += 2
            }
        }
    }
    return $sum / $samples
}

function Compare-WarningRegion {
    param($Reference, $Warning)

    if ($Reference.Width -ne $Warning.Width -or
        $Reference.Height -ne $Warning.Height) {
        throw 'Warning/reference dimensions differ'
    }
    $regionSize = 112
    [long]$outsideChanged = 0
    [long]$outsideSamples = 0
    [long]$insideChanged = 0
    for ($y = 0; $y -lt $Warning.Height; $y += 8) {
        for ($x = 0; $x -lt $Warning.Width; $x += 8) {
            $i = $Warning.Offset + ($y * $Warning.Width + $x) * 3
            $r = $Reference.Offset + ($y * $Reference.Width + $x) * 3
            $difference = [Math]::Abs([int]$Warning.Bytes[$i] - [int]$Reference.Bytes[$r]) +
                          [Math]::Abs([int]$Warning.Bytes[$i + 1] - [int]$Reference.Bytes[$r + 1]) +
                          [Math]::Abs([int]$Warning.Bytes[$i + 2] - [int]$Reference.Bytes[$r + 2])
            $inside = $x -ge $Warning.Width - $regionSize -and
                      $y -ge $Warning.Height - $regionSize
            if ($inside) {
                if ($difference -gt 120) { $insideChanged++ }
            } else {
                if ($difference -gt 120) { $outsideChanged++ }
                $outsideSamples++
            }
        }
    }
    return [pscustomobject]@{
        OutsideRatio = $outsideChanged / [double]$outsideSamples
        InsideChanged = $insideChanged
    }
}

$thresholds = @{ black = 1.0; warning = 1.0; blur = 2.0 }
$styleNumbers = @{ black = 0; blur = 1; warning = 2 }
$captures = @{}
foreach ($style in @('black', 'warning', 'blur')) {
    $prefix = "d3d11-4k-$style"
    $text = Invoke-NsfwVlc -Name $prefix -Profile -InputVideo $nv12Video `
        -DumpPrefix $prefix -ExtraArguments @(
            '--nsfw-processing-backend=d3d11',
            "--nsfw-decision-map-path=$decisionMap",
            "--nsfw-block-style=$style"
        )
    if ($text -notmatch 'video backend=d3d11.*texture=NV12') {
        throw "$style did not activate the NV12 D3D11 backend"
    }
    if ($text -match 'black fallback|blocked render.*failed|effect style=.*failed') {
        throw "$style reported a D3D11 render fallback or failure"
    }
    $styleNumber = $styleNumbers[$style]
    if ($text -notmatch "D3D11 profile style=$styleNumber frames=\d+ average=[0-9.]+ ms median=([0-9.]+) ms") {
        throw "$style did not emit median GPU timing data"
    }
    $median = [double]$Matches[1]
    if ($median -gt $thresholds[$style]) {
        throw "$style median was $median ms, above $($thresholds[$style]) ms"
    }
    $capture = Get-ChildItem -Path $OutputDirectory -Filter "$prefix*.ppm" |
        Select-Object -First 1
    if ($null -eq $capture) {
        throw "$style did not produce a debug capture"
    }
    $captures[$style] = Read-Ppm $capture.FullName
    Write-Host "$style 4K GPU median: $median ms"
}

$reference = Read-Ppm $referencePpm
$blackMean = Get-PpmMean $captures.black
if ($blackMean -gt 8.0) {
    throw "Black output mean was $blackMean instead of near zero"
}
$sourceDetail = Get-PpmDetail $reference
$blurDetail = Get-PpmDetail $captures.blur
if ($sourceDetail -le 0 -or $blurDetail -ge $sourceDetail * 0.45) {
    throw "Blur retained too much detail (source=$sourceDetail blur=$blurDetail)"
}
$warningStats = Compare-WarningRegion $reference $captures.warning
if ($warningStats.OutsideRatio -gt 0.02) {
    throw "Warning changed $($warningStats.OutsideRatio * 100)% of sampled pixels outside its region"
}
if ($warningStats.InsideChanged -lt 5) {
    throw 'Warning did not visibly change its expected bottom-right region'
}

$p010Text = Invoke-NsfwVlc -Name 'd3d11-p010-live' -InputVideo $p010Video `
    -ExtraArguments @(
        '--nsfw-processing-backend=d3d11',
        '--nsfw-decision-map-path=',
        '--nsfw-provider=cpu',
        '--nsfw-analysis-stride=1',
        '--nsfw-threshold=1.0'
    )
if ($p010Text -notmatch 'video backend=d3d11.*texture=P010') {
    throw 'P010 fixture did not activate the P010 D3D11 path'
}
if ($p010Text -notmatch 'D3D11 analysis readback active at \d+x\d+') {
    throw 'P010 model-sized readback did not complete'
}

$overlayText = Invoke-NsfwVlc -Name 'd3d11-overlay-enabled' `
    -InputVideo $p010Video -ExtraArguments @(
        '--nsfw-processing-backend=d3d11',
        '--nsfw-decision-map-path=',
        '--nsfw-provider=cpu',
        '--nsfw-analysis-stride=1',
        '--nsfw-threshold=1.0',
        '--nsfw-debug-overlay=1'
    )
if ($overlayText -notmatch 'video backend=d3d11' -or
    $overlayText -notmatch 'D3D11 debug overlay enabled \(cached GPU composition\)' -or
    $overlayText -notmatch 'D3D11 debug overlay active \(cached GPU composition\)' -or
    $overlayText -notmatch 'D3D11 debug overlay rendered frames=[1-9]\d*' -or
    $overlayText -match 'requesting VLC software conversion') {
    throw 'D3D11 debug overlay did not render on the GPU backend'
}

$queueText = Invoke-NsfwVlc -Name 'd3d11-opaque-queue-cap' -ExtraArguments @(
    '--nsfw-processing-backend=d3d11',
    '--nsfw-decision-map-path=',
    '--nsfw-provider=cpu',
    '--nsfw-analysis-stride=24',
    '--nsfw-block-padding-frames=20',
    '--nsfw-buffered-frames=24',
    '--nsfw-worker-threads=2',
    '--nsfw-threshold=1.0'
)
if ($queueText -notmatch 'capped D3D11 opaque queue at (\d+) frames and analysis stride at \1 \(decoder surfaces=(\d+)') {
    throw 'D3D11 did not derive queue depth from the decoder texture'
}
$queueDepth = [int]$Matches[1]
$surfaceCount = [int]$Matches[2]
if ($queueDepth -gt 8 -or $queueDepth -gt [Math]::Max(1, $surfaceCount - 3)) {
    throw "Unsafe D3D11 queue depth $queueDepth for $surfaceCount surfaces"
}
if ($queueText -notmatch 'D3D11 analysis readback active at \d+x\d+') {
    throw 'The dynamically capped D3D11 queue did not progress'
}

$seekText = Invoke-NsfwVlc -Name 'd3d11-media-time' -InputVideo $seekVideo `
    -ExtraArguments @(
        '--start-time=2',
        '--nsfw-processing-backend=d3d11',
        "--nsfw-decision-map-path=$seekDecisionMap",
        '--nsfw-block-style=blur'
    )
if ($seekText -notmatch 'first decision-map frame is (\d+) ms, blocked=1') {
    throw 'Non-zero-start decision map did not block at media time'
}
$firstMediaTime = [int64]$Matches[1]
if ($firstMediaTime -lt 1750 -or $firstMediaTime -gt 2500) {
    throw "Decision-map media time was $firstMediaTime ms after --start-time=2"
}

$cpuText = Invoke-NsfwVlc -Name 'cpu-fallback' -ExtraArguments @(
    '--nsfw-processing-backend=cpu',
    "--nsfw-decision-map-path=$decisionMap",
    '--nsfw-block-style=warning'
)
if ($cpuText -notmatch 'video backend=cpu') {
    throw 'CPU fallback did not activate'
}

Write-Host 'D3D11 runtime checks passed.'
