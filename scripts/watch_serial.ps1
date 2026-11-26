# Live console log viewer for nice!nano debug builds.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/watch_serial.ps1 [COM_PORT]
# Leave this running, then press the reset button on the board to see the
# full boot log. Auto-reconnects since the USB port briefly disappears
# during a reset - keep resetting as many times as you want without
# restarting this script. Press Ctrl+C to stop.

param(
    [string]$PortName = "COM3"
)

while ($true) {
    try {
        $port = New-Object System.IO.Ports.SerialPort $PortName,115200,None,8,one
        $port.ReadTimeout = 200
        $port.Open()
        Write-Output "Connected to $PortName. Watching for output (Ctrl+C to stop)..."
        while ($port.IsOpen) {
            try {
                $line = $port.ReadLine()
                Write-Output $line
            } catch [TimeoutException] {
                # no data yet, keep polling
            }
        }
    } catch {
        Write-Output "Waiting for $PortName ... ($($_.Exception.Message))"
        Start-Sleep -Milliseconds 500
    } finally {
        if ($port -and $port.IsOpen) { $port.Close() }
    }
}
