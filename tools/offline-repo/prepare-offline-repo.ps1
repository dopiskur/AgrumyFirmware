<#
.SYNOPSIS
  Roadmap #94 (part C2): prepare an offline firmware repository (typically a USB stick) on an
  INTERNET-CONNECTED Windows machine, for import on an offline Agrumy server via the Firmware
  page's "Import from a directory on the server" (#94-2b). Same output contract as
  prepare-offline-repo.sh and the browser "Build offline repo" button:
    <Target>\agrumy-<board>-v<version>.bin   one per board per release
    <Target>\manifest.json                    schemaVersion 1, SHA-256 per file, no url field

.EXAMPLE
  .\prepare-offline-repo.ps1 -Target E:\agrumy-firmware
  .\prepare-offline-repo.ps1 -Target E:\agrumy-firmware -Repo someone/fork -Limit 3
#>
param(
    [Parameter(Mandatory = $true)] [string] $Target,
    [string] $Repo = "dopiskur/AgrumyFirmware",
    [int] $Limit = 0,
    [string] $Token = $env:GITHUB_TOKEN
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Target | Out-Null

$headers = @{ "Accept" = "application/vnd.github+json"; "User-Agent" = "agrumy-offline-repo" }
if ($Token) { $headers["Authorization"] = "Bearer $Token" }

# The same file-name convention the API enforces on import - anything else is not firmware. Board
# allows '-'/'_' (kc868-a6, seeed_xiao_esp32c3); the (?<!-full) lookbehind keeps a full-image
# filename (agrumy-<board>-full-v<version>.bin) from being swallowed as board="<board>-full" -
# this script only ever wants the OTA .bin, never the full image.
$pattern = '^agrumy-(?<board>[a-z0-9_-]+)(?<!-full)-v(?<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?)\.bin$'

Write-Host "Reading releases of $Repo ..."
$releases = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repo/releases?per_page=100"

$manifest = [ordered]@{
    schemaVersion = 1
    generatedAt   = (Get-Date).ToUniversalTime().ToString("o")
    source        = "github:$Repo"
    releases      = @()
}
$count = 0
foreach ($rel in $releases) {
    if ($rel.draft) { continue }
    if ($Limit -gt 0 -and $count -ge $Limit) { break }
    $files = @()
    foreach ($asset in $rel.assets) {
        if ($asset.name -notmatch $pattern) { continue }
        $path = Join-Path $Target $asset.name
        Write-Host "  $($asset.name)"
        Invoke-WebRequest -Headers $headers -Uri $asset.browser_download_url -OutFile $path
        $files += [ordered]@{
            board     = $Matches["board"]
            fileName  = $asset.name
            sizeBytes = (Get-Item $path).Length
            sha256    = (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLowerInvariant()
            url       = $null
        }
    }
    if ($files.Count -eq 0) { continue }
    $version = if ($rel.tag_name.StartsWith("v")) { $rel.tag_name.Substring(1) } else { $rel.tag_name }
    $manifest.releases += [ordered]@{ version = $version; publishedAt = $rel.published_at; files = $files }
    $count++
}

$manifest | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $Target "manifest.json") -Encoding utf8
$total = ($manifest.releases | ForEach-Object { $_.files.Count } | Measure-Object -Sum).Sum
Write-Host "Done: $total file(s) from $count release(s) -> $(Join-Path $Target 'manifest.json')"
