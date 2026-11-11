# A small form at the top-left that takes the focus while a probe runs, so a BuffrX window behind it is visible but not focused.
# Started by tools/unfocused-panel-probe.ps1; close it by stopping its process.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$f = New-Object System.Windows.Forms.Form
$f.StartPosition = 'Manual'
$f.Location = New-Object System.Drawing.Point(0, 0)
$f.Size = New-Object System.Drawing.Size(300, 200)
[void]$f.ShowDialog()
