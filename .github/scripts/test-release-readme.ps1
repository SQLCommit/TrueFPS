param([string]$Root = (Join-Path $PSScriptRoot '../..'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$fixture = @'
<div align="center">
# ![TrueFPS](https://readme-typing-svg.demolab.com/?lines=TrueFPS)
[![Download](https://custom-icon-badges.demolab.com/badge/download?logo=data%3Abase64%2Ctest)](https://example.com/releases)
</div>
## ![Features](https://readme-typing-svg.demolab.com/?lines=Features)
<details open>
<summary><strong>Cutscenes</strong> &mdash; Keep this summary.</summary>
<dl><dd>
Keep **all** feature details &amp; [links](https://example.com/a_(b)).
</dd></dl></details>
<details>
<summary>Closed feature</summary>
Keep closed details, too.
</details>
| `/truefps limit <rate>` | `/ashita/<Name>_<id>/` |
Literal `<details>` and ``<summary>`example`</summary>``.
````html
<details><summary>Unchanged code</summary></details>
```
````
    <details>Indented code</details>
~~~html
<div>Also unchanged</div>
~~~
'@
$plain = ConvertTo-ReleaseReadme $fixture
$hardBreak = "First line.  `nSecond line.`n"
Assert-True ((ConvertTo-ReleaseReadme $hardBreak) -ceq $hardBreak) 'Markdown hard line breaks changed.'
foreach ($expected in @(
    '# TrueFPS', '## Features', '### Cutscenes', 'Keep this summary.',
    '[Download](https://example.com/releases)',
    'Keep **all** feature details & [links](https://example.com/a_(b)).',
    '### Closed feature', 'Keep closed details, too.',
    '| `/truefps limit <rate>` | `/ashita/<Name>_<id>/` |',
    'Literal `<details>` and ``<summary>`example`</summary>``.',
    '<details><summary>Unchanged code</summary></details>',
    '    <details>Indented code</details>', '<div>Also unchanged</div>'
)) { Assert-True ($plain.Contains($expected)) "Conversion lost: $expected" }
Assert-True ($plain -notmatch 'demolab|base64') 'Decorative image URLs remain.'
Assert-True ((ConvertTo-ReleaseReadme $plain) -ceq $plain) 'Plain Markdown changed on a second conversion.'
$rejected = $false
try { ConvertTo-ReleaseReadme '<iframe src="example"></iframe>' | Out-Null } catch { $rejected = $true }
Assert-True $rejected 'Unsupported HTML should fail instead of silently losing content.'

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('truefps-release-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $cfg = Get-ReleaseConfig -Root $Root
    $originalPath = Join-Path $Root 'README.md'
    $originalHash = (Get-FileHash -LiteralPath $originalPath).Hash
    $original = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $originalPath).Path)
    foreach ($doc in $cfg.docs) {
        $target = Join-Path $scratch $doc
        [IO.Directory]::CreateDirectory((Split-Path $target -Parent)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Root $doc) -Destination $target
    }
    $dll = Join-Path $scratch 'test.dll'
    [IO.File]::WriteAllBytes($dll, [byte[]]@(0x4D, 0x5A, 0, 1))
    $outDir = Join-Path $scratch 'out'
    foreach ($revision in 1, 2) {
        $source = $original
        if ($revision -eq 2) { $source += "`nREADME edited after building the DLL.`n" }
        [IO.File]::WriteAllText((Join-Path $scratch 'README.md'), $source)
        $zipName = New-ReleasePackage $cfg -Dll $dll -Version '1.0' -Interface '4.30' -BuildInfo @{ test = $true } -OutDir $outDir -Root $scratch
        $zipPath = Join-Path $outDir $zipName
        $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            Assert-True ($zip.Entries.Count -eq ($cfg.docs.Count + 2)) 'Unexpected files in ZIP.'
            foreach ($doc in $cfg.docs) {
                $entry = $zip.GetEntry("docs/$($cfg.docsFolder)/$(Split-Path $doc -Leaf)")
                Assert-True ($null -ne $entry) "Missing packaged document: $doc"
                $reader = New-Object IO.StreamReader($entry.Open())
                try { $actual = $reader.ReadToEnd() } finally { $reader.Dispose() }
                $expected = [IO.File]::ReadAllText((Join-Path $scratch $doc))
                if ((Split-Path $doc -Leaf) -ieq 'README.md') { $expected = ConvertTo-ReleaseReadme $expected }
                Assert-True ($actual -ceq $expected) "Packaged $doc differs from its expected content."
            }
            $stream = $zip.GetEntry("plugins/$($cfg.dll)").Open()
            try {
                $bytes = New-Object IO.MemoryStream
                $stream.CopyTo($bytes)
                Assert-True ([BitConverter]::ToString($bytes.ToArray()) -eq '4D-5A-00-01') 'DLL changed during packaging.'
            } finally { $stream.Dispose(); $bytes.Dispose() }
        } finally { $zip.Dispose() }
        $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-True ((Get-Content -LiteralPath (Join-Path $outDir 'SHA256SUMS.txt')).Trim() -eq "$hash  $zipName") 'ZIP checksum is incorrect.'
        Assert-True ([IO.File]::ReadAllText((Join-Path $scratch 'README.md')) -ceq $source) 'Packaging overwrote its source README.'
    }
    Assert-True ((Get-FileHash -LiteralPath $originalPath).Hash -eq $originalHash) 'Source checkout README was changed.'
    Write-Host 'PASS: README conversion, code preservation, ZIP contents, checksums, and documentation edits after build.'
} finally { Remove-Item -LiteralPath $scratch -Recurse -Force }
