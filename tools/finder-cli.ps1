# finder-cli.ps1 - talk to the ESP32-S3 finder over its native USB port.
#
# Examples:
#   .\finder-cli.ps1 -Port COM7
#   .\finder-cli.ps1 -Port COM7 -Command status -Seconds 2
#   .\finder-cli.ps1 -Port COM7 -Command "btn 4"
#   .\finder-cli.ps1 -Port COM7 -Command "btn 4 long"
#
# Interactive mode streams logs and sends a line when you press Enter.
# Ctrl+C quits.

param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [int]$Baud = 115200,

    [string]$Command,

    [int]$Seconds = 2
)

$ErrorActionPreference = "Stop"

$ports = [System.IO.Ports.SerialPort]::GetPortNames()
if ($ports -notcontains $Port) {
    Write-Host ("Port {0} not found. Available: {1}" -f $Port, ($ports -join ", "))
    exit 1
}

$sp = New-Object System.IO.Ports.SerialPort -ArgumentList $Port, $Baud, "None", 8, "One"
$sp.ReadTimeout = 200
$sp.WriteTimeout = 1000
$sp.NewLine = "`r`n"
$sp.Open()

try {
    if ($Command) {
        $sp.WriteLine($Command)
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            if ($sp.BytesToRead -gt 0) {
                Write-Host -NoNewline $sp.ReadExisting()
            } else {
                Start-Sleep -Milliseconds 50
            }
        }
        Write-Host ""
    } else {
        Write-Host ("Connected to {0}. Type a command and press Enter. Ctrl+C quits." -f $Port)

        $keyboard = $true
        try { [void][Console]::KeyAvailable } catch { $keyboard = $false }

        if ($keyboard) {
            $typed = ""
            while ($true) {
                while ($sp.BytesToRead -gt 0) {
                    Write-Host -NoNewline $sp.ReadExisting()
                }
                if ([Console]::KeyAvailable) {
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
                }
                Start-Sleep -Milliseconds 20
            }
        } else {
            while ($true) {
                if ($sp.BytesToRead -gt 0) {
                    Write-Host -NoNewline $sp.ReadExisting()
                }
                $line = Read-Host "finder"
                if ($line.Length -gt 0) { $sp.WriteLine($line) }
            }
        }
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
