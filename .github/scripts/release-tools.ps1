# Shared by the Prepare release, Release check and Ashita rebuild workflows (.github/workflows). The plugin-specific
# parts are .github/release.json and .github/scripts/build.ps1. Works in Windows PowerShell 5.1 and PowerShell 7.
Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'readme-tools.ps1')

function Get-ReleaseConfig {
    param([string]$Root = '.')
    $cfg = Get-Content -Raw (Join-Path $Root '.github/release.json') | ConvertFrom-Json
    foreach ($key in 'name', 'dll', 'buildOutput', 'docsFolder', 'docs', 'versionFile', 'versionPattern', 'repository', 'ashitaSdkRef', 'ashitaInterface') {
        if ($cfg.PSObject.Properties.Name -notcontains $key) { throw ".github/release.json has no '$key'." }
    }
    return $cfg
}

# The version the plugin reports to Ashita, read from its source ("1.3"). Ashita reads it as a number, so 1.10 would
# show as 1.1: after 1.9 comes 2.0.
function Get-PluginVersion {
    param($Cfg, [string]$Root = '.')
    $text = Get-Content -Raw (Join-Path $Root $Cfg.versionFile)
    $m = [regex]::Match($text, $Cfg.versionPattern)
    if (-not $m.Success) { throw "No version found in $($Cfg.versionFile) (pattern $($Cfg.versionPattern))." }
    return $m.Groups[1].Value
}

# ASHITA_INTERFACE_VERSION from the text of an SDK's Ashita.h ("4.30").
function Get-SdkInterface {
    param([string]$Text)
    $m = [regex]::Match($Text, 'ASHITA_INTERFACE_VERSION\s*=\s*([0-9]+\.[0-9]+)')
    if (-not $m.Success) { throw 'ASHITA_INTERFACE_VERSION was not found in Ashita.h.' }
    return $m.Groups[1].Value
}

# Adds a line to the run's summary page (and the log).
function Write-Summary {
    param([string]$Text)
    Write-Host $Text
    if ($env:GITHUB_STEP_SUMMARY) { Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value $Text }
}

# Runs gh with up to three tries (GitHub's API times out now and then) and returns its output as one string. With
# -AllowNotFound a "not found" answer returns $null; any other failure after three tries stops the run.
$script:GhRetryDelay = 5
function Invoke-Gh {
    param([string[]]$Arguments, [switch]$AllowNotFound)
    $message = ''
    for ($try = 1; $try -le 3; $try++) {
        $saved = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $out = @(& gh @Arguments 2>&1)
        $code = $LASTEXITCODE
        $ErrorActionPreference = $saved
        $message = ($out | ForEach-Object { "$_" }) -join "`n"
        if ($code -eq 0) { $global:LASTEXITCODE = 0; return $message }
        if ($AllowNotFound -and $message -match '(HTTP 404|release not found|Not Found)') { $global:LASTEXITCODE = 0; return $null }
        if ($try -lt 3) { Start-Sleep -Seconds ($script:GhRetryDelay * $try) }
    }
    throw "gh $(($Arguments | Select-Object -First 2) -join ' ') failed: $message"
}
$script:RawFile = 'Accept: application/vnd.github.raw'

# The plugin interface of Ashita's SDK at a commit (a full SHA, or "main").
function Get-AshitaInterfaceAt {
    param([string]$Ref)
    return Get-SdkInterface (Invoke-Gh @('api', '-H', $script:RawFile, "repos/AshitaXI/Ashita-v4beta/contents/plugins/sdk/Ashita.h?ref=$Ref"))
}

# Builds with the SDK at $Sdk, then checks the DLL is a 32-bit x86 module. Returns its full path.
function Invoke-PluginBuild {
    param($Cfg, [string]$Sdk, [string]$Interface, [string]$Root = '.')
    $sdkPath = (Resolve-Path $Sdk).Path
    $found = Get-SdkInterface (Get-Content -Raw (Join-Path $sdkPath 'Ashita.h'))
    if ($found -ne $Interface) { throw "The Ashita SDK is interface $found, not $Interface." }
    & (Join-Path $Root '.github/scripts/build.ps1') -Sdk $sdkPath
    $dll = (Resolve-Path (Join-Path $Root $Cfg.buildOutput)).Path
    $bytes = [IO.File]::ReadAllBytes($dll)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x14C) { throw "$($Cfg.buildOutput) is not a 32-bit x86 DLL." }
    return $dll
}

# What was built, from what, and by which run: BUILDINFO.json inside the zip.
function Get-BuildInfo {
    param([string]$Version, [string]$Interface, [string]$SdkRef, [string]$Root = '.')
    $source = (git -C $Root rev-parse HEAD)
    if ($LASTEXITCODE) { throw 'Could not read the source commit.' }
    $vs = ''
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) { $vs = "$(& $vswhere -latest -products * -property installationVersion)".Trim() }
    $cmake = ''
    if (Get-Command cmake -ErrorAction SilentlyContinue) { $cmake = "$((cmake --version) | Select-Object -First 1)".Trim() }
    $global:LASTEXITCODE = 0
    return [ordered]@{
        version        = $Version
        ashitaInterface = $Interface
        sourceCommit   = "$source".Trim()
        ashitaSdkCommit = $SdkRef
        workflowCommit = $env:GITHUB_SHA
        run            = if ($env:GITHUB_RUN_ID) { "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID (attempt $env:GITHUB_RUN_ATTEMPT)" } else { 'local' }
        runnerImage    = $env:ImageVersion
        visualStudio   = $vs
        cmake          = $cmake
    }
}

# In $OutDir: <Name>-v<Version>_Interface-<Interface>.zip (plugins\<dll>, docs\<folder>\... with BUILDINFO.json, and any
# extras at their own paths, so it extracts straight into the Ashita folder) and SHA256SUMS.txt. Every listed document
# is required. Returns the zip's file name.
function New-ReleasePackage {
    param($Cfg, [string]$Dll, [string]$Version, [string]$Interface, $BuildInfo, [string]$OutDir, [string]$Root = '.')
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $rootPath = (Resolve-Path $Root).Path
    $outPath = (Resolve-Path $OutDir).Path
    $zipName = "$($Cfg.name)-v${Version}_Interface-$Interface.zip"
    $zipPath = Join-Path $outPath $zipName
    $entries = New-Object System.Collections.Generic.List[object]
    $readmes = @{}
    $entries.Add(@($Dll, "plugins/$($Cfg.dll)"))
    foreach ($doc in $Cfg.docs) {
        $src = Join-Path $rootPath $doc
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "$doc (listed in .github/release.json) is not in the repository." }
        if ((Split-Path $doc -Leaf) -ieq 'README.md') {
            $readmes[$src] = ConvertTo-ReleaseReadme ([IO.File]::ReadAllText($src))
        }
        $entries.Add(@($src, "docs/$($Cfg.docsFolder)/$(Split-Path $doc -Leaf)"))
    }
    $extras = if ($Cfg.PSObject.Properties.Name -contains 'extras') { @($Cfg.extras) } else { @() }
    foreach ($extra in $extras) {
        $dir = Join-Path $rootPath $extra
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { throw "$extra (listed in extras) is not in the repository." }
        foreach ($file in Get-ChildItem -LiteralPath $dir -Recurse -File) {
            $rel = $file.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/')
            if (($rel -split '/') | Where-Object { $_.StartsWith('.') }) { continue }   # .gitignore and the like
            $entries.Add(@($file.FullName, $rel))
        }
    }
    $info = Join-Path $outPath 'BUILDINFO.json'
    $BuildInfo | ConvertTo-Json | Set-Content -LiteralPath $info -Encoding Ascii
    $entries.Add(@($info, "docs/$($Cfg.docsFolder)/BUILDINFO.json"))
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($e in $entries) {
            if ($readmes.ContainsKey($e[0])) {
                $entry = $zip.CreateEntry($e[1], [IO.Compression.CompressionLevel]::Optimal)
                $writer = New-Object IO.StreamWriter($entry.Open(), (New-Object Text.UTF8Encoding($false)))
                try { $writer.Write($readmes[$e[0]]) } finally { $writer.Dispose() }
            } else {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $e[0], $e[1], [IO.Compression.CompressionLevel]::Optimal)
            }
        }
    } finally { $zip.Dispose() }
    Remove-Item -LiteralPath $info
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath (Join-Path $outPath 'SHA256SUMS.txt') -Value "$hash  $zipName" -Encoding Ascii
    return $zipName
}

# A link to this version's section of the changelog as it is at $Tag: the first heading below the title that names
# the version, in CHANGELOG.md, else in README.md (a Version History section). $null when neither has one. The anchor
# follows GitHub's heading ids (lower case, punctuation dropped, spaces to hyphens, -1/-2 for repeats).
function Get-HeadingSlug {
    param([string]$Text)
    $t = $Text -replace '\[([^\]]*)\]\([^)]*\)', '$1'
    $t = ($t -replace '[`*]', '').ToLowerInvariant()
    $t = [regex]::Replace($t, '[^\p{L}\p{M}\p{N}\p{Pc} -]', '')
    return $t.Replace(' ', '-')
}
function Get-ChangelogLink {
    param($Cfg, [string]$Version, [string]$Tag, [string]$Root = '.')
    $pattern = '(?<![\w.])v?' + [regex]::Escape($Version) + '(?![\w.])'
    foreach ($file in 'CHANGELOG.md', 'README.md') {
        $path = Join-Path $Root $file
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $seen = @{}
        $fence = $false
        foreach ($line in Get-Content -LiteralPath $path) {
            if ($line -match '^\s*(```|~~~)') { $fence = -not $fence; continue }
            if ($fence -or $line -notmatch '^(#{1,6})\s+(.+?)\s*#*\s*$') { continue }
            $level = $Matches[1].Length
            $text = $Matches[2]
            $slug = Get-HeadingSlug $text
            $n = if ($seen.ContainsKey($slug)) { $seen[$slug] } else { 0 }
            $seen[$slug] = $n + 1
            if ($level -ge 2 -and $text -match $pattern) {
                $anchor = if ($n -gt 0) { "$slug-$n" } else { $slug }
                return "https://github.com/$($Cfg.repository)/blob/$Tag/$file#$anchor"
            }
        }
    }
    return $null
}

# The download footer added to a release's notes: a What's new link to the changelog (when it has a section for
# this version), a rule, then a Note box with the Download line and the
# verification details folded away (Virus scan adds its line to them). Everything from the marker line down is generated and replaced when the files
# are prepared again; the owner's notes go above it.
$script:NotesMarker = '<!-- release files: generated below this line -->'
function Get-DownloadNotes {
    param($Cfg, [string]$ZipName, [string]$Interface, [string]$SdkRef, [string]$OutDir, [string]$Changelog = '')
    $hash = ((Get-Content -LiteralPath (Join-Path $OutDir 'SHA256SUMS.txt') | Select-Object -First 1) -split '\s+')[0]
    $lines = @($script:NotesMarker, '')
    if ($Changelog) { $lines += @("**What's new:** see the [changelog]($Changelog).", '') }
    $lines += @(
        '---',
        '',
        '> [!NOTE]',
        "> **Download:** ``$ZipName`` - extract it into your Ashita folder (adds ``plugins\$($Cfg.dll)`` and ``docs\$($Cfg.docsFolder)\``). Built for Ashita interface $Interface.",
        '>',
        '> <details><summary>Verify this download</summary>',
        '>',
        "> - SHA-256: ``$hash``",
        "> - Attestation: ``gh attestation verify $ZipName --repo $($Cfg.repository)``",
        '>',
        '> </details>'
    )
    return ($lines -join "`n")
}
function Join-ReleaseNotes {
    param([string]$Body, [string]$Downloads)
    if ($null -eq $Body) { $Body = '' }
    $at = $Body.IndexOf($script:NotesMarker)
    if ($at -ge 0) { $Body = $Body.Substring(0, $at) }
    $Body = $Body.TrimEnd()
    if ($Body) { return "$Body`n`n$Downloads`n" }
    return "$Downloads`n"
}

# What the Ashita rebuild workflow should do for Ashita commit $AshitaCommit. Returns step outputs (key=value lines);
# build=true only when that commit's interface differs from the latest release's and no rebuild for it exists yet.
function Get-RebuildPlan {
    param([string]$Repo, [string]$AshitaCommit)
    $skip = { param($why) Write-Host "::notice::$why"; Write-Summary "No rebuild: $why"; return @('build=false') }
    if ($AshitaCommit -notmatch '^[0-9a-fA-F]{40}$') { throw 'Enter the full 40-character Ashita commit ID.' }
    $interface = Get-AshitaInterfaceAt $AshitaCommit
    $latest = Invoke-Gh @('api', "repos/$Repo/releases/latest", '--jq', '.tag_name') -AllowNotFound
    if (-not $latest) { return & $skip 'this repository has no published release yet.' }
    $latest = $latest.Trim()
    $base = $latest -replace '_Interface-[0-9.]+$', ''
    if ($latest -match '_Interface-([0-9.]+)$') { $current = $Matches[1] }
    else {
        $json = Invoke-Gh @('api', '-H', $script:RawFile, "repos/$Repo/contents/.github/release.json?ref=$base") -AllowNotFound
        if (-not $json) { return & $skip "release $base was published before the release workflows were added; make a new release first." }
        $current = ($json | ConvertFrom-Json).ashitaInterface
    }
    if ($interface -eq $current) { return & $skip "Ashita commit $($AshitaCommit.Substring(0, 7)) is still interface $current, the same as $latest." }
    $tag = "${base}_Interface-$interface"
    if (Invoke-Gh @('release', 'view', $tag, '--repo', $Repo, '--json', 'tagName') -AllowNotFound) {
        return & $skip "$tag already exists (published or draft)."
    }
    Write-Summary "Rebuilding $base for Ashita interface $interface (was $current) as $tag."
    return @('build=true', "base=$base", "tag=$tag", "previous=$latest", "from=$current", "interface=$interface", "sdk-ref=$($AshitaCommit.ToLowerInvariant())")
}
