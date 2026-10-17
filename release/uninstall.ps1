param([string]$GameDir = "")

if (-not $GameDir) {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($line in Get-Content $vdf) {
            if ($line -match '"path"\s+"(.+)"') { $libs += $matches[1] -replace '\\\\', '\' }
        }
    }
    foreach ($lib in $libs) {
        $p = Join-Path $lib "steamapps\common\Batman Arkham Knight"
        if (Test-Path (Join-Path $p "Binaries\Win64\BatmanAK.exe")) { $GameDir = $p }
    }
}
if (-not $GameDir) {
    Write-Host "couldnt find the game, run it like this: uninstall.bat `"D:\path\to\Batman Arkham Knight`""
    exit 1
}

$win64 = Join-Path $GameDir "Binaries\Win64"
if (Test-Path "$win64\libScePad_x64.dll.original") {
    Move-Item "$win64\libScePad_x64.dll.original" "$win64\libScePad_x64.dll" -Force
}
Remove-Item "$GameDir\BmGame\Scripts\DualSenseScript.cs" -ErrorAction SilentlyContinue
$loc = Join-Path $GameDir "BmGame\Localization\INT\PC.Int"
if (Test-Path "$loc.original") { Move-Item "$loc.original" $loc -Force }
Write-Host "removed. ak_dualsense.ini and the log are left in Binaries\Win64 if you want them"
