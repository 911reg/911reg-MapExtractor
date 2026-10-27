$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$taskImages=@()
foreach ($taskSize in @(16,32,48,64,128,256)) {
    $taskBitmap=[Drawing.Bitmap]::new($taskSize,$taskSize)
    $taskGraphics=[Drawing.Graphics]::FromImage($taskBitmap)
    $taskGraphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $taskGraphics.ScaleTransform($taskSize/256.0,$taskSize/256.0)
    $taskBackground=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(32,32,32))
    $taskOrange=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,132,40))
    $taskLight=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(237,237,237))
    $taskDark=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(85,85,85))
    $taskOutline=[Drawing.Pen]::new([Drawing.Color]::FromArgb(255,132,40),6)
    $taskFold=[Drawing.Pen]::new([Drawing.Color]::FromArgb(32,32,32),5)
    $taskArrow=[Drawing.Pen]::new([Drawing.Color]::FromArgb(255,132,40),17)
    $taskArrow.StartCap=[Drawing.Drawing2D.LineCap]::Round
    $taskArrow.EndCap=[Drawing.Drawing2D.LineCap]::Round
    $taskPath=[Drawing.Drawing2D.GraphicsPath]::new()
    $taskPath.AddArc(8,8,52,52,180,90); $taskPath.AddArc(196,8,52,52,270,90)
    $taskPath.AddArc(196,196,52,52,0,90); $taskPath.AddArc(8,196,52,52,90,90); $taskPath.CloseFigure()
    $taskGraphics.FillPath($taskBackground,$taskPath); $taskGraphics.DrawPath($taskOutline,$taskPath)
    $taskGraphics.FillPolygon($taskLight,[Drawing.Point[]]@([Drawing.Point]::new(42,78),[Drawing.Point]::new(99,56),[Drawing.Point]::new(99,177),[Drawing.Point]::new(42,199)))
    $taskGraphics.FillPolygon($taskDark,[Drawing.Point[]]@([Drawing.Point]::new(99,56),[Drawing.Point]::new(153,78),[Drawing.Point]::new(153,199),[Drawing.Point]::new(99,177)))
    $taskGraphics.FillPolygon($taskOrange,[Drawing.Point[]]@([Drawing.Point]::new(153,78),[Drawing.Point]::new(210,56),[Drawing.Point]::new(210,177),[Drawing.Point]::new(153,199)))
    $taskGraphics.DrawLine($taskFold,99,56,99,177); $taskGraphics.DrawLine($taskFold,153,78,153,199)
    $taskGraphics.DrawLine($taskArrow,144,114,200,26)
    $taskGraphics.DrawLine($taskArrow,200,26,169,32); $taskGraphics.DrawLine($taskArrow,200,26,211,57)
    $taskFont=[Drawing.Font]::new('Segoe UI',29,[Drawing.FontStyle]::Bold,[Drawing.GraphicsUnit]::Pixel)
    $taskStream=[IO.MemoryStream]::new()
    $taskBitmap.Save($taskStream,[Drawing.Imaging.ImageFormat]::Png)
    $taskImages+=,[pscustomobject]@{Size=$taskSize;Bytes=$taskStream.ToArray()}
    $taskStream.Dispose(); $taskFont.Dispose(); $taskPath.Dispose(); $taskArrow.Dispose(); $taskFold.Dispose()
    $taskOutline.Dispose(); $taskDark.Dispose(); $taskLight.Dispose(); $taskOrange.Dispose(); $taskBackground.Dispose()
    $taskGraphics.Dispose(); $taskBitmap.Dispose()
}
$taskFile=[IO.File]::Create((Join-Path $PSScriptRoot '911reg.ico'))
$taskWriter=[IO.BinaryWriter]::new($taskFile)
try {
    $taskWriter.Write([uint16]0); $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]$taskImages.Count)
    $taskOffset=6+16*$taskImages.Count
    foreach ($taskImage in $taskImages) {
        $taskDimension=if ($taskImage.Size -eq 256) { 0 } else { $taskImage.Size }
        $taskWriter.Write([byte]$taskDimension); $taskWriter.Write([byte]$taskDimension)
        $taskWriter.Write([byte]0); $taskWriter.Write([byte]0)
        $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]32)
        $taskWriter.Write([uint32]$taskImage.Bytes.Length); $taskWriter.Write([uint32]$taskOffset)
        $taskOffset+=$taskImage.Bytes.Length
    }
    foreach ($taskImage in $taskImages) { $taskWriter.Write([byte[]]$taskImage.Bytes) }
} finally { $taskWriter.Dispose() }
