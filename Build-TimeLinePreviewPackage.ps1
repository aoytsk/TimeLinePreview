param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$PackageName = "timeline_preview.au2pkg",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Resolve-MSBuildPath {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\17\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\17\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )
    foreach ($path in $candidates) {
        if (Test-Path $path) { return $path }
    }
    throw "MSBuild.exe が見つかりません。"
}

function Ensure-FileExists {
    param([string]$PathToCheck)
    if (-not (Test-Path $PathToCheck)) {
        throw "必要ファイルが見つかりません: $PathToCheck"
    }
}

$scriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot   = $scriptDir
$packageDir    = Join-Path $projectRoot "Package"
$outputZipPath = Join-Path $projectRoot ($PackageName + ".zip")

Ensure-FileExists (Join-Path $packageDir "package.ini")
Ensure-FileExists (Join-Path $packageDir "package.txt")

if (-not $SkipBuild) {
    $msbuild = Resolve-MSBuildPath
    $proj = Join-Path $projectRoot "TimeLinePreview\TimeLinePreview.vcxproj"
    & $msbuild $proj "/t:Build" "/m" `
        "/p:Configuration=$Configuration;Platform=$Platform;RunPostBuildEvent=Never;PostBuildEventUseInBuild=false"
    if ($LASTEXITCODE -ne 0) { throw "ビルド失敗: $proj" }
}

# ---- バイナリ確認 ----
$pluginBin = Join-Path $projectRoot "$Platform\$Configuration\TimeLinePreview.aux2"
Ensure-FileExists $pluginBin

# ---- パッケージフォルダ構築 ----
$pluginDir = Join-Path $packageDir "Plugin"
$langDir   = Join-Path $packageDir "Language"

foreach ($d in @($pluginDir, $langDir)) {
    if (Test-Path $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    New-Item -ItemType Directory -Path $d | Out-Null
}

# プラグイン本体
Copy-Item $pluginBin -Destination (Join-Path $pluginDir "TimeLinePreview.aux2") -Force

# ---- 言語ファイル ----
# TimeLinePreview プロジェクトフォルダの *.aul2 をすべて収集する。
# ファイルが存在しない場合はエラーで停止する（フォールバック生成なし）。
$projLangDir = Join-Path $projectRoot "TimeLinePreview"
$langFiles = Get-ChildItem -Path $projLangDir -Filter "*.aul2" -ErrorAction SilentlyContinue
if ($null -eq $langFiles -or $langFiles.Count -eq 0) {
    throw "言語ファイルが見つかりません: $projLangDir\*.aul2"
}
foreach ($file in $langFiles) {
    $langName = $file.BaseName                    # 例: "English"
    $dstName  = "$langName.TimeLinePreview.aul2"  # 例: "English.TimeLinePreview.aul2"
    Copy-Item $file.FullName -Destination (Join-Path $langDir $dstName) -Force
}

# ZIP 圧縮
if (Test-Path $outputZipPath) { Remove-Item -LiteralPath $outputZipPath -Force }
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $outputZipPath -Force

Write-Host "Package folder updated: $packageDir"
Write-Host "Package created: $outputZipPath"
