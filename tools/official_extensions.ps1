# Writes the hash of each extension in extensions\ as a C++ string list, the
# same hash the overlay makes at load (Extensions::loadLuaExtension), so it
# can tell its own extensions apart. The file is rewritten only when it changes.
param([string]$extensions, [string]$out)

$sha = [Security.Cryptography.SHA512]::Create()
function Hex([byte[]]$bytes) { -join ($bytes | ForEach-Object { $_.ToString("x2") }) }

$lines = @()
foreach ($folder in Get-ChildItem -LiteralPath $extensions -Directory | Sort-Object Name) {
    if (-not (Test-Path -LiteralPath (Join-Path $folder.FullName "main.lua"))) { continue }
    $files = Get-ChildItem -LiteralPath $folder.FullName -Recurse -File | Where-Object { $_.Extension -ieq ".lua" }
    $paths = [string[]]@($files | ForEach-Object { $_.FullName.Substring($folder.FullName.Length + 1).Replace("\", "/") })
    $hashes = [string[]]@($files | ForEach-Object { Hex $sha.ComputeHash([IO.File]::ReadAllBytes($_.FullName)) })
    [Array]::Sort($paths, $hashes, [StringComparer]::Ordinal)

    $combined = ""
    for ($i = 0; $i -lt $paths.Length; $i++) { $combined += $paths[$i] + "`n" + $hashes[$i] + "`n" }
    $hash = Hex $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($combined))
    $lines += "`"$hash`", // $($folder.Name)"
}

$text = ($lines -join "`n") + "`n"
$old = if (Test-Path -LiteralPath $out) { [IO.File]::ReadAllText($out) } else { $null }
if ($text -ne $old) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
    [IO.File]::WriteAllText($out, $text)
}
