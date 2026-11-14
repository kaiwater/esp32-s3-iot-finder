# finder-cli.ps1 - talk to the ESP32-S3 finder over its native USB port.
#
# The script finds the COM port itself and reconnects when the port
# disappears, which is what happens while a new firmware boots.
#
# Examples:
#   .\finder-cli.ps1 -ListPorts
#   .\finder-cli.ps1 -Port COM4             # explicit port
#   .\finder-cli.ps1 -Port COM4 -Stream 1000  # live RF lines every second
#   .\finder-cli.ps1 -Command status -Seconds 2
#   .\finder-cli.ps1 -Command "btn 4 long"
#
# Interactive mode streams logs and sends a line when you press Enter.
# Ctrl+C quits.

param(
    [string]$Port = "auto",

    [int]$Baud = 115200,

    [string]$Command,

    [int]$Seconds = 2,

    # How long to keep looking for the port after a disconnect.
    # 0 = retry forever.
    [int]$ReconnectSeconds = 30,

    # After connecting, turn on 'stream <ms>' so the PC gets RF data.
    # 0 = leave streaming off.
    [int]$Stream = 0,

    [switch]$ListPorts
)

$ErrorActionPreference = "Stop"
$script:LastPort = $null

function Get-PortDevice([string]$name) {
    $dev = $null
    try {
        $dev = Get-CimInstance -ClassName Win32_SerialPort -ErrorAction Stop |
            Where-Object { $_.DeviceID -eq $name } | Select-Object -First 1
    } catch { }
    if (-not $dev) {
        try {
            $dev = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
                Where-Object { $_.Name -like "*($name)*" } | Select-Object -First 1
        } catch { }
    }
    return $dev
}

function Get-PortSummary([string]$name) {
    $dev = Get-PortDevice $name
    $desc = ""
    $pnp = ""
    if ($dev) {
        if ($dev.Name) { $desc = [string]$dev.Name }
        elseif ($dev.Description) { $desc = [string]$dev.Description }
        if ($dev.PNPDeviceID) { $pnp = [string]$dev.PNPDeviceID }
    }
    return [pscustomobject]@{ Desc = $desc; Pnp = $pnp }
}

if ($ListPorts) {
    $names = [System.IO.Ports.SerialPort]::GetPortNames()
    if (-not $names) {
        Write-Host "No COM ports found."
    } else {
        foreach ($n in $names) {
            $info = Get-PortSummary $n
            Write-Host ("{0,-8} {1}" -f $n, $info.Desc)
            if ($info.Pnp) { Write-Host ("         {0}" -f $info.Pnp) }
        }
    }
    exit 0
}

function Find-FinderPort {
    $names = [System.IO.Ports.SerialPort]::GetPortNames()
    $best = $null
    $bestScore = -999
    foreach ($n in $names) {
        if ([string]::IsNullOrEmpty($n)) { continue }
        $info = Get-PortSummary $n
        $score = 0
        # The ESP32-S3 USB-Serial/JTAG is VID 303A.  This is language
        # independent, unlike the device description.
        if ($info.Pnp -match "VID_303A") { $score += 30 }
        if ($info.Desc -match "JTAG|CDC|Serial Device") { $score += 5 }
        # UART bridges used for flashing are not the console.
        if ($info.Pnp -match "VID_1A86|VID_10C4|VID_0403|VID_067B") { $score -= 20 }
        if ($script:LastPort -and $n -eq $script:LastPort) { $score += 10 }
        if ($score -gt $bestScore) {
            $bestScore = $score
            $best = $n
        }
    }
    return $best
}

function Connect-Finder([string]$want, [int]$timeoutSec) {
    if ($timeoutSec -gt 0) {
        $deadline = (Get-Date).AddSeconds($timeoutSec)
    } else {
        $deadline = $null
    }

    while ($true) {
        $name = $want
        if ([string]::IsNullOrEmpty($name) -or $name -eq "auto") {
            $name = Find-FinderPort
        }

        if ($name) {
            $sp = $null
            try {
                $sp = New-Object System.IO.Ports.SerialPort `
                    -ArgumentList $name, $Baud, "None", 8, "One"
                $sp.ReadTimeout = 200
                $sp.WriteTimeout = 1000
                $sp.NewLine = "`r`n"
                # Do not assert DTR/RTS: the ESP32-S3 USB-Serial/JTAG can
                # interpret those lines as a chip reset.
                $sp.DtrEnable = $false
                $sp.RtsEnable = $false
                $sp.Open()
                $sp.DtrEnable = $false
                $sp.RtsEnable = $false
                $script:LastPort = $name
                Write-Host ("[finder] connected {0}" -f $name)

                # Opening the port may still reset the board once.  Give the
                # firmware time to boot before the caller sends a command.
                Start-Sleep -Milliseconds 1500
                return $sp
            } catch {
                if ($sp) {
                    try { $sp.Dispose() } catch { }
                }
            }
        }

        if ($null -ne $deadline -and (Get-Date) -gt $deadline) {
            Write-Host "[finder] no COM port found (use -ListPorts)"
            return $null
        }
        Start-Sleep -Milliseconds 400
    }
}

function Close-Finder($sp) {
    if ($sp) {
        try { $sp.Close() } catch { }
        try { $sp.Dispose() } catch { }
    }
}

$sp = Connect-Finder $Port $ReconnectSeconds
if (-not $sp) { exit 1 }

try {
    if ($Command) {
        $reconnects = 0
        $deadline = (Get-Date).AddSeconds($Seconds)
        $send = $true

        while ((Get-Date) -lt $deadline) {
            try {
                if ($send) {
                    $sp.WriteLine($Command)
                    $send = $false
                }
                if ($sp.BytesToRead -gt 0) {
                    Write-Host -NoNewline $sp.ReadExisting()
                } else {
                    Start-Sleep -Milliseconds 50
                }
            } catch {
                Close-Finder $sp
                $reconnects++
                if ($reconnects -gt 3) {
                    Write-Host ""
                    Write-Host "[finder] giving up after 3 reconnects"
                    exit 1
                }
                Write-Host ""
                Write-Host "[finder] link lost, waiting for the port..."
                $sp = Connect-Finder $Port $ReconnectSeconds
                if (-not $sp) { exit 1 }
                # The device usually rebooted: send the command again.
                $send = $true
                $deadline = (Get-Date).AddSeconds($Seconds)
            }
        }
        Write-Host ""
    } else {
        Write-Host "Connected. Type a command and press Enter. Ctrl+C quits."
        Write-Host "A silent console is normal when nothing is logging; try 'page' or 'stream 1000'."

        $keyboard = $true
        try { [void][Console]::KeyAvailable } catch { $keyboard = $false }

        function Send-Setup {
            if ($Stream -gt 0) {
                try { $sp.WriteLine("stream $Stream") } catch { }
            }
        }

        # Prove both directions right away instead of waiting for a log.
        $probeText = ""
        try { $sp.WriteLine("page") } catch { }
        $probeEnd = (Get-Date).AddMilliseconds(900)
        while ((Get-Date) -lt $probeEnd) {
            if ($sp.BytesToRead -gt 0) {
                $chunk = $sp.ReadExisting()
                Write-Host -NoNewline $chunk
                $probeText += $chunk
            } else {
                Start-Sleep -Milliseconds 30
            }
        }
        if ($probeText -notmatch "page=") {
            Write-Host ""
            Write-Host "[finder] no reply to 'page': USB input may not reach the firmware."
            Write-Host "[finder] check 'status' on the UART: usb host/drv/rx counters."
        } else {
            Send-Setup
        }

        $typed = ""
        while ($true) {
            try {
                while ($sp.BytesToRead -gt 0) {
                    Write-Host -NoNewline $sp.ReadExisting()
                }
                if ($keyboard -and [Console]::KeyAvailable) {
                    $key = [Console]::ReadKey($true)
                    if ($key.Key -eq "Enter") {
                        Write-Host ""
                        if ($typed.Length -gt 0) { $sp.WriteLine($typed) }
                        $typed = ""
                    } elseif ($key.Key -eq "Backspace") {
                        if ($typed.Length -gt 0) {
                            $typed = $typed.Substring(0, $typed.Length - 1)
                            Write-Host -NoNewline "`b `b"
                        }
                    } elseif ([int]$key.KeyChar -ge 32) {
                        $typed += $key.KeyChar
                        Write-Host -NoNewline $key.KeyChar
                    }
                } elseif (-not $keyboard) {
                    $line = Read-Host "finder"
                    if ($line.Length -gt 0) { $sp.WriteLine($line) }
                }
                Start-Sleep -Milliseconds 20
            } catch {
                Close-Finder $sp
                Write-Host ""
                Write-Host "[finder] port lost, reconnecting... (Ctrl+C quits)"
                $sp = Connect-Finder $Port 0
                if (-not $sp) { break }
                Send-Setup
                $typed = ""
            }
        }
    }
} finally {
    Close-Finder $sp
}
