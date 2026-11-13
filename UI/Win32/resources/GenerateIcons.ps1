$csharp = @"
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

public static class IconGenerator
{
    private static GraphicsPath CreateRoundedRectangle(RectangleF rect, float radius)
    {
        GraphicsPath path = new GraphicsPath();
        float diameter = radius * 2.0f;
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
        path.AddArc(rect.Right - diameter, rect.Y, diameter, diameter, 270, 90);
        path.AddArc(rect.Right - diameter, rect.Bottom - diameter, diameter, diameter, 0, 90);
        path.AddArc(rect.X, rect.Bottom - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
        return path;
    }

    public static Bitmap GenerateLogo(int size)
    {
        Bitmap bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.CompositingQuality = CompositingQuality.HighQuality;
            g.Clear(Color.Transparent);

            float s = size / 512.0f;

            // 1. Fluent Rounded Container
            float pad = 24.0f * s;
            float r = 100.0f * s;
            RectangleF baseRect = new RectangleF(pad, pad, size - 2 * pad, size - 2 * pad);
            using (GraphicsPath basePath = CreateRoundedRectangle(baseRect, r))
            {
                // Background Gradient: Deep Slate Navy to Dark Carbon
                using (LinearGradientBrush bgBrush = new LinearGradientBrush(
                    new PointF(pad, pad),
                    new PointF(size - pad, size - pad),
                    Color.FromArgb(255, 12, 20, 36),
                    Color.FromArgb(255, 5, 8, 16)))
                {
                    g.FillPath(bgBrush, basePath);
                }

                // Top-Left Ambient Glow (Electric Blue aura)
                using (GraphicsPath glowPath = new GraphicsPath())
                {
                    glowPath.AddEllipse(pad, pad, 300 * s, 300 * s);
                    using (PathGradientBrush pgb = new PathGradientBrush(glowPath))
                    {
                        pgb.CenterColor = Color.FromArgb(75, 0, 140, 255);
                        pgb.SurroundColors = new Color[] { Color.FromArgb(0, 0, 140, 255) };
                        g.FillPath(pgb, glowPath);
                    }
                }

                // Border Rim (Windows Fluent Glass Highlight)
                using (Pen borderPen = new Pen(Color.FromArgb(140, 80, 170, 255), 3.5f * s))
                {
                    g.DrawPath(borderPen, basePath);
                }
            }

            // 2. Automotive Cockpit Speedometer Gauge Arc
            float cx = size / 2.0f;
            float cy = size / 2.0f + 14.0f * s;
            float gaugeR = 152.0f * s;
            RectangleF gaugeRect = new RectangleF(cx - gaugeR, cy - gaugeR, 2 * gaugeR, 2 * gaugeR);

            // Gauge Track Background
            using (Pen trackPen = new Pen(Color.FromArgb(50, 120, 160, 200), 15.0f * s))
            {
                trackPen.StartCap = LineCap.Round;
                trackPen.EndCap = LineCap.Round;
                g.DrawArc(trackPen, gaugeRect, 135, 270);
            }

            // Active Dynamic Neon Gradient Arc (from 135 to 345 deg)
            using (LinearGradientBrush arcBrush = new LinearGradientBrush(
                new PointF(cx - gaugeR, cy + 40 * s),
                new PointF(cx + gaugeR * 0.7f, cy - gaugeR),
                Color.FromArgb(255, 0, 114, 230),  // Windows Blue
                Color.FromArgb(255, 0, 245, 235))) // Vibrant Electric Cyan
            {
                using (Pen activePen = new Pen(arcBrush, 15.0f * s))
                {
                    activePen.StartCap = LineCap.Round;
                    activePen.EndCap = LineCap.Round;
                    g.DrawArc(activePen, gaugeRect, 135, 210);
                }
            }

            // Speedometer Ticks (automotive instrumentation)
            for (int ang = 150; ang <= 390; ang += 24)
            {
                double rad = ang * Math.PI / 180.0;
                float cos = (float)Math.Cos(rad);
                float sin = (float)Math.Sin(rad);

                float r1 = 172.0f * s;
                float r2 = ((ang % 48 == 6) ? 188.0f : 182.0f) * s;

                float x1 = cx + r1 * cos;
                float y1 = cy + r1 * sin;
                float x2 = cx + r2 * cos;
                float y2 = cy + r2 * sin;

                Color tickCol = (ang <= 345) ? Color.FromArgb(220, 0, 235, 255) : Color.FromArgb(60, 120, 160, 200);
                using (Pen tickPen = new Pen(tickCol, 3.2f * s))
                {
                    tickPen.StartCap = LineCap.Round;
                    tickPen.EndCap = LineCap.Round;
                    g.DrawLine(tickPen, x1, y1, x2, y2);
                }
            }

            // 3. Dual Wireless Waves (Connectivity arcs)
            using (Pen wavePen1 = new Pen(Color.FromArgb(150, 0, 210, 255), 4.5f * s))
            {
                wavePen1.StartCap = LineCap.Round;
                wavePen1.EndCap = LineCap.Round;
                g.DrawArc(wavePen1, cx - 88 * s, cy - 24 * s, 176 * s, 176 * s, 210, 120);
            }
            using (Pen wavePen2 = new Pen(Color.FromArgb(220, 60, 245, 255), 5.5f * s))
            {
                wavePen2.StartCap = LineCap.Round;
                wavePen2.EndCap = LineCap.Round;
                g.DrawArc(wavePen2, cx - 58 * s, cy + 6 * s, 116 * s, 116 * s, 210, 120);
            }

            // 4. Central "Play" Chevron (Media & Cockpit Drive Station)
            float tx = cx + 8.0f * s;
            float ty = cy - 14.0f * s;
            float pw = 52.0f * s;
            float ph = 56.0f * s;

            using (GraphicsPath playPath = new GraphicsPath())
            {
                playPath.AddLines(new PointF[] {
                    new PointF(tx - pw * 0.7f, ty - ph),
                    new PointF(tx + pw * 1.15f, ty),
                    new PointF(tx - pw * 0.7f, ty + ph)
                });
                playPath.CloseFigure();

                using (LinearGradientBrush playBrush = new LinearGradientBrush(
                    new PointF(tx - pw, ty - ph),
                    new PointF(tx + pw, ty + ph),
                    Color.FromArgb(255, 0, 245, 240),  // Bright Cyan
                    Color.FromArgb(255, 0, 120, 255))) // Deep Windows Accent Blue
                {
                    g.FillPath(playBrush, playPath);
                }

                // Clean crisp border on Play triangle
                using (Pen playRim = new Pen(Color.FromArgb(190, 255, 255, 255), 3.0f * s))
                {
                    g.DrawPath(playRim, playPath);
                }
            }

            // 5. Cockpit Horizontal Anchor Wing
            using (Pen wingPen = new Pen(Color.FromArgb(200, 0, 190, 255), 4.5f * s))
            {
                wingPen.StartCap = LineCap.Round;
                wingPen.EndCap = LineCap.Round;
                g.DrawLine(wingPen, cx - 55 * s, cy + 96 * s, cx + 55 * s, cy + 96 * s);
            }
        }
        return bmp;
    }

    // Pack standard Windows multi-resolution .ICO (contains 256, 128, 64, 48, 32, 16)
    public static void SaveMultiResIco(string path)
    {
        int[] sizes = new int[] { 256, 128, 64, 48, 32, 16 };
        byte[][] pngBytes = new byte[sizes.Length][];

        for (int i = 0; i < sizes.Length; i++)
        {
            using (Bitmap b = GenerateLogo(sizes[i]))
            {
                using (MemoryStream ms = new MemoryStream())
                {
                    b.Save(ms, ImageFormat.Png);
                    pngBytes[i] = ms.ToArray();
                }
            }
        }

        using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write))
        using (BinaryWriter bw = new BinaryWriter(fs))
        {
            // ICONDIR
            bw.Write((ushort)0); // reserved
            bw.Write((ushort)1); // type 1 = ICO
            bw.Write((ushort)sizes.Length); // count

            int offset = 6 + sizes.Length * 16;

            for (int i = 0; i < sizes.Length; i++)
            {
                int sz = sizes[i];
                bw.Write((byte)(sz >= 256 ? 0 : sz)); // width
                bw.Write((byte)(sz >= 256 ? 0 : sz)); // height
                bw.Write((byte)0); // color count
                bw.Write((byte)0); // reserved
                bw.Write((ushort)1); // color planes
                bw.Write((ushort)32); // bit count
                bw.Write((uint)pngBytes[i].Length); // bytes in res
                bw.Write((uint)offset); // image offset

                offset += pngBytes[i].Length;
            }

            for (int i = 0; i < sizes.Length; i++)
            {
                bw.Write(pngBytes[i]);
            }
        }
    }
}
"@

Add-Type -TypeDefinition $csharp -ReferencedAssemblies "System.Drawing"

[IconGenerator]::SaveMultiResIco("UI\Win32\resources\app_icon.ico")
$logo512 = [IconGenerator]::GenerateLogo(512)
$logo512.Save("UI\Win32\resources\app_icon_512.png", [System.Drawing.Imaging.ImageFormat]::Png)
$logo256 = [IconGenerator]::GenerateLogo(256)
$logo256.Save("UI\Win32\resources\app_icon_256.png", [System.Drawing.Imaging.ImageFormat]::Png)
$logo256.Save("UI\Win32\resources\oem_icon.png", [System.Drawing.Imaging.ImageFormat]::Png)

Write-Output "Successfully built app_icon.ico (multi-resolution), app_icon_512.png, app_icon_256.png, and oem_icon.png"
