# TCM HMI — arranca servidor Flask y abre el navegador
param(
    [int]$Port = 5050,
    [switch]$NoBrowser
)

$ErrorActionPreference = 'Continue'
$HmiRoot = Split-Path $PSScriptRoot -Parent
$Url = "http://127.0.0.1:$Port"
$DistIndex = Join-Path $HmiRoot 'frontend\dist\index.html'

function Test-HmiPort {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        $ok = $connect.AsyncWaitHandle.WaitOne(400, $false)
        if (-not $ok) { return $false }
        $client.EndConnect($connect)
        return $true
    } catch {
        return $false
    } finally {
        if ($client.Connected) { $client.Close() }
    }
}

function Test-HmiServer {
    if (-not (Test-HmiPort)) { return $false }
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        try {
            $code = & curl.exe -s -o $null -w '%{http_code}' --connect-timeout 1 --max-time 2 "$Url/api/state" 2>$null
            return ($code -eq '200')
        } catch {
            return $true
        }
    }
    return $true
}

function Get-PythonExe {
    $venvPy = Join-Path $HmiRoot '.venv\Scripts\python.exe'
    if (Test-Path $venvPy) { return $venvPy }
    return 'python'
}

function Open-Browser {
    if (-not $NoBrowser) {
        Start-Process $Url
    }
}

# Servidor ya escuchando → solo abrir web
if (Test-HmiPort) {
    Write-Host "TCM HMI ya en ejecucion ($Url)"
    Open-Browser
    exit 0
}

if (-not (Test-Path $DistIndex)) {
    Write-Host "Compilando UI (frontend)..."
    Push-Location (Join-Path $HmiRoot 'frontend')
    try {
        npm run build
        if ($LASTEXITCODE -ne 0) {
            Write-Error "npm run build fallo"
            exit 1
        }
    } finally {
        Pop-Location
    }
}

$python = Get-PythonExe
$serverCmd = "cd /d `"$HmiRoot`" && title TCM HMI :$Port && `"$python`" app.py"

Write-Host "Iniciando servidor TCM HMI en :$Port ..."
Start-Process cmd.exe -ArgumentList '/k', $serverCmd

$ready = $false
for ($i = 1; $i -le 30; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-HmiPort) {
        $ready = $true
        break
    }
    if ($i % 2 -eq 0) {
        Write-Host "  esperando puerto $Port ... ($([math]::Floor($i / 2))/15)"
    }
}

Open-Browser

if ($ready) {
    Write-Host "Listo: $Url"
    Start-Sleep -Seconds 1
    exit 0
}

Write-Warning "El servidor tarda. Revisa la ventana 'TCM HMI :$Port'."
Start-Sleep -Seconds 3
exit 0
