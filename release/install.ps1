param([string]$GameDir = "")

$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Game {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return "" }
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($line in Get-Content $vdf) {
            if ($line -match '"path"\s+"(.+)"') { $libs += $matches[1] -replace '\\\\', '\' }
        }
    }
    foreach ($lib in $libs) {
        $p = Join-Path $lib "steamapps\common\Batman Arkham Knight"
        if (Test-Path (Join-Path $p "Binaries\Win64\BatmanAK.exe")) { return $p }
    }
    return ""
}

if (-not $GameDir) { $GameDir = Find-Game }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "Binaries\Win64\BatmanAK.exe"))) {
    Write-Host "couldnt find arkham knight, run it like this: install.bat `"D:\path\to\Batman Arkham Knight`""
    exit 1
}
Write-Host "game folder: $GameDir"

if (Get-Process BatmanAK -ErrorAction SilentlyContinue) {
    Write-Host "close the game first"
    exit 1
}

$win64 = Join-Path $GameDir "Binaries\Win64"
$scripts = Join-Path $GameDir "BmGame\Scripts"
if (-not (Test-Path (Join-Path $win64 "plugins\BmSDK.Host.asi"))) {
    Write-Host "BmSDK isnt installed, get it from https://github.com/Team-BmSDK/BmSDK-AK/releases first"
    exit 1
}

$dll = Join-Path $win64 "libScePad_x64.dll"
$backup = Join-Path $win64 "libScePad_x64.dll.original"
if ((Test-Path $dll) -and -not (Test-Path $backup)) {
    Copy-Item $dll $backup
    Write-Host "original libScePad_x64.dll saved as libScePad_x64.dll.original"
}
Copy-Item (Join-Path $here "Binaries\Win64\libScePad_x64.dll") $dll -Force
if (-not (Test-Path (Join-Path $win64 "ak_dualsense.ini"))) {
    Copy-Item (Join-Path $here "Binaries\Win64\ak_dualsense.ini") $win64
}
New-Item -ItemType Directory -Force $scripts | Out-Null
Copy-Item (Join-Path $here "BmGame\Scripts\DualSenseScript.cs") $scripts -Force
Write-Host "dll and script installed"

Write-Host ""
Write-Host "done. last thing: in steam go to arkham knight > properties > controller and set it to 'disable steam input'"
