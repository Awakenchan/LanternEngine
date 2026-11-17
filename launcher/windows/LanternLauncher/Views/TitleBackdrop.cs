// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using BloodborneLauncher.Core;
using Path = Avalonia.Controls.Shapes.Path;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The Play page's night behind its title screen, drawn rather than a picture and in the theme's
    /// colours: lamplight behind the title, Yharnam's roofs along the bottom with haze between their
    /// rows, fog rising, embers drifting up, darker corners and a fine still grain.
    /// </summary>
    public sealed class TitleBackdrop : Panel
    {
        /// <summary>The skyline's own size, scaled to the width it is shown at.</summary>
        private const double CityWidth = 1600, CityHeight = 260;
        private const int EmberCount = 28;

        private readonly Rectangle _glow = Layer(), _fog = Layer(), _vignette = Layer(), _haze = Layer();
        private readonly Path _farCity = City(Skyline(seed: 7, low: 70, spire: 120, cathedralAt: 1040, cathedralHeight: 238));
        private readonly Path _nearCity = City(Skyline(seed: 23, low: 26, spire: 74, cathedralAt: -1, cathedralHeight: 0));
        private readonly Canvas _city = new() { IsHitTestVisible = false, ClipToBounds = true };
        private readonly Canvas _embers = new() { IsHitTestVisible = false, ClipToBounds = true };
        private readonly List<Ember> _sparks = new();
        private readonly DispatcherTimer _motion = new() { Interval = TimeSpan.FromMilliseconds(33) };
        private readonly System.Diagnostics.Stopwatch _since = new();
        private readonly Random _random = new(1693);
        private IBrush _lamp = Brushes.Transparent;
        private double _time;
        private bool _attached;

        public TitleBackdrop()
        {
            IsHitTestVisible = false;
            _city.Children.Add(_farCity);
            _city.Children.Add(_haze);
            _city.Children.Add(_nearCity);
            Children.Add(_glow);
            Children.Add(_city);
            Children.Add(_fog);
            Children.Add(_embers);
            Children.Add(_vignette);
            Children.Add(new Rectangle { IsHitTestVisible = false, Fill = GrainBrush.Value, Opacity = 0.6 });
            SizeChanged += (_, e) => Place(e.NewSize);

            // The embers move only while someone can see them: not on another page, not minimized,
            // never while the game runs (no GPU time taken from it), not with the system's animations off.
            _motion.Tick += (_, _) => Drift();
            Workspace.Current.GameStateChanged += UpdateMotion;
            ApplyTheme();
        }

        protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
        {
            base.OnAttachedToVisualTree(e);
            _attached = true;
            UpdateMotion();
        }

        protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
        {
            base.OnDetachedFromVisualTree(e);
            _attached = false;
            UpdateMotion();
        }

        /// <summary>The colours of the theme in use (the theme can change in Settings between visits).</summary>
        public void ApplyTheme()
        {
            var lamp = Colour("AccentTextFillColorPrimaryBrush", Color.FromRgb(0xC2, 0xA0, 0x6A));
            var night = Colour("WindowBaseBrush", Color.FromRgb(0x0B, 0x0F, 0x11));
            var mist = LauncherTheme.Mix(night, Color.FromRgb(0x4A, 0x7A, 0x80), 0.35);

            _glow.Fill = Radial(0.5, 0.36, 0.5, 0.55,
                (LauncherTheme.WithAlpha(lamp, 0x2E), 0), (LauncherTheme.WithAlpha(lamp, 0x10), 0.45), (LauncherTheme.WithAlpha(lamp, 0), 1));
            _farCity.Fill = new SolidColorBrush(LauncherTheme.Mix(night, mist, 0.28));
            _nearCity.Fill = new SolidColorBrush(LauncherTheme.Mix(night, Colors.Black, 0.45));
            _haze.Fill = Vertical((LauncherTheme.WithAlpha(mist, 0), 0.25), (LauncherTheme.WithAlpha(mist, 0x38), 1));
            _fog.Fill = Vertical((LauncherTheme.WithAlpha(mist, 0), 0.5), (LauncherTheme.WithAlpha(mist, 0x30), 0.85), (LauncherTheme.WithAlpha(mist, 0x50), 1));
            _vignette.Fill = Radial(0.5, 0.42, 0.85, 0.9,
                (Color.FromArgb(0, 0, 0, 0), 0.5), (Color.FromArgb(0x50, 0, 0, 0), 0.85), (Color.FromArgb(0x90, 0, 0, 0), 1));
            _lamp = new SolidColorBrush(lamp);
            foreach (var spark in _sparks) spark.Dot.Fill = _lamp;
        }

        // ---------------------------------------------------------------- layout

        /// <summary>The city scaled to the width (never below 75%, so the spires keep their shape in a
        /// narrow window) and standing on the bottom edge; the haze between its rows; the embers spread out.</summary>
        private void Place(Size size)
        {
            var scale = Math.Max(size.Width / CityWidth, 0.75);
            var top = size.Height - CityHeight * scale;
            var transform = new MatrixTransform(Matrix.CreateScale(scale, scale) *
                                                Matrix.CreateTranslation((size.Width - CityWidth * scale) / 2, top));
            _farCity.RenderTransform = transform;
            _nearCity.RenderTransform = transform;
            _haze.Width = size.Width;
            _haze.Height = CityHeight * scale;
            Canvas.SetTop(_haze, top);
            foreach (var spark in _sparks) spark.Reset(size, _random, anywhere: true);
        }

        /// <summary>
        /// A row of roofs as one shape, <see cref="CityWidth"/> x <see cref="CityHeight"/>, standing on
        /// its bottom edge: houses with gabled or flat roofs, towers with spires, and a cathedral at
        /// <paramref name="cathedralAt"/> (none when negative). The same seed draws the same street.
        /// </summary>
        private static Geometry Skyline(int seed, double low, double spire, double cathedralAt, double cathedralHeight)
        {
            const double ground = CityHeight;
            var random = new Random(seed);
            var points = new List<Point> { new(0, ground) };
            for (double x = 0; x < CityWidth;)
            {
                if (cathedralAt >= x && cathedralAt < x + 120)
                {
                    // Towers either side of a nave, one great spire in the middle.
                    double nave = ground - low - 80, tower = ground - low - 130, eaves = ground - low - 30;
                    foreach (var (dx, y) in new (double, double)[]
                             {
                                 (0, eaves), (20, eaves), (20, tower), (34, tower - 70), (48, tower), (48, nave),
                                 (96, nave), (96, nave - 40), (110, ground - cathedralHeight), (124, nave - 40), (124, nave),
                                 (172, nave), (172, tower), (186, tower - 70), (200, tower), (200, eaves), (220, eaves),
                             })
                        points.Add(new Point(x + dx, y));
                    x += 220;
                    continue;
                }
                double wide = random.Next(34, 104), top = ground - low - random.Next(0, 46);
                points.Add(new Point(x, top));
                switch (random.Next(10))
                {
                    case < 3: // a tower with a spire
                        var towerTop = top - random.Next(20, 60);
                        points.Add(new Point(x + wide * 0.3, top));
                        points.Add(new Point(x + wide * 0.3, towerTop));
                        points.Add(new Point(x + wide * 0.5, towerTop - random.Next((int)(spire * 0.5), (int)spire)));
                        points.Add(new Point(x + wide * 0.7, towerTop));
                        points.Add(new Point(x + wide * 0.7, top));
                        break;
                    case < 7: // a steep gable
                        points.Add(new Point(x + wide * 0.5, top - wide * 0.55));
                        break;
                    default: // flat, with a chimney
                        points.Add(new Point(x + wide * 0.2, top));
                        points.Add(new Point(x + wide * 0.2, top - 12));
                        points.Add(new Point(x + wide * 0.28, top - 12));
                        points.Add(new Point(x + wide * 0.28, top));
                        break;
                }
                points.Add(new Point(Math.Min(x + wide, CityWidth), top));
                x += wide;
            }
            points.Add(new Point(CityWidth, ground));

            var geometry = new StreamGeometry();
            using var context = geometry.Open();
            context.BeginFigure(points[0], true);
            foreach (var point in points.Skip(1)) context.LineTo(point);
            context.EndFigure(true);
            return geometry;
        }

        // ---------------------------------------------------------------- embers

        /// <summary>One rising spark. It moves by its render transform, so a frame lays nothing out.</summary>
        private sealed class Ember
        {
            public readonly Ellipse Dot = new() { IsHitTestVisible = false };
            private readonly TranslateTransform _at = new();
            private double _x, _y, _rise, _sway, _phase, _period, _bright, _top, _bottom;

            public Ember() => Dot.RenderTransform = _at;

            public void Reset(Size size, Random random, bool anywhere)
            {
                _bottom = size.Height;
                _top = size.Height * 0.3;
                _x = random.NextDouble() * size.Width;
                _y = anywhere ? _top + random.NextDouble() * (_bottom - _top) : _bottom + random.Next(0, 40);
                _rise = 7 + random.NextDouble() * 15;
                _sway = 5 + random.NextDouble() * 12;
                _phase = random.NextDouble() * Math.PI * 2;
                _period = 4 + random.NextDouble() * 5;
                _bright = 0.25 + random.NextDouble() * 0.45;
                Dot.Width = Dot.Height = 1.5 + random.NextDouble() * 1.6;
            }

            /// <summary>Moves it on by <paramref name="seconds"/>; false once it has risen out of sight.</summary>
            public bool Step(double seconds, double time)
            {
                _y -= _rise * seconds;
                if (_y < _top) return false;
                _at.X = _x + Math.Sin(time * Math.PI * 2 / _period + _phase) * _sway;
                _at.Y = _y;
                // Brightest low down, gone by the time it reaches the title's height.
                Dot.Opacity = _bright * Math.Clamp((_y - _top) / ((_bottom - _top) * 0.6), 0, 1);
                return true;
            }
        }

        private void UpdateMotion()
        {
            var run = _attached && !Workspace.Current.Game.Running && SystemAnimations();
            _embers.IsVisible = run;
            if (run == _motion.IsEnabled) return;
            if (run)
            {
                _since.Restart();
                _motion.Start();
            }
            else
                _motion.Stop();
        }

        private void Drift()
        {
            var size = _embers.Bounds.Size;
            if (size.Width <= 0 || MainWindow.Instance?.WindowState == WindowState.Minimized) return;
            if (_sparks.Count == 0)
                for (var i = 0; i < EmberCount; i++)
                {
                    var spark = new Ember();
                    spark.Dot.Fill = _lamp;
                    spark.Reset(size, _random, anywhere: true);
                    _sparks.Add(spark);
                    _embers.Children.Add(spark.Dot);
                }
            var seconds = Math.Min(_since.Elapsed.TotalSeconds, 0.1);
            _since.Restart();
            _time += seconds;
            foreach (var spark in _sparks)
                if (!spark.Step(seconds, _time))
                    spark.Reset(size, _random, anywhere: false);
        }

        /// <summary>Windows' "Animation effects" (Settings › Accessibility › Visual effects); elsewhere on.</summary>
        private static bool SystemAnimations()
        {
            if (!OperatingSystem.IsWindows()) return true;
            var on = true;
            return !SystemParametersInfo(SpiGetClientAreaAnimation, 0, ref on, 0) || on;
        }

        private const uint SpiGetClientAreaAnimation = 0x1042;

        [DllImport("user32.dll")]
        private static extern bool SystemParametersInfo(uint action, uint param, ref bool value, uint winIni);

        // ---------------------------------------------------------------- brushes

        /// <summary>A fine film grain: a small tile of faint light and dark specks, made once, repeated.</summary>
        private static readonly Lazy<IBrush> GrainBrush = new(() =>
        {
            const int size = 160;
            var bitmap = new WriteableBitmap(new PixelSize(size, size), new Vector(96, 96),
                Avalonia.Platform.PixelFormat.Bgra8888, Avalonia.Platform.AlphaFormat.Premul);
            var random = new Random(1693);
            using (var frame = bitmap.Lock())
            {
                var pixels = new byte[size * 4];
                for (var y = 0; y < size; y++)
                {
                    for (var x = 0; x < size; x++)
                    {
                        // Alpha 0-16, premultiplied: a light speck or a dark one.
                        var alpha = (byte)random.Next(0, 17);
                        var value = random.Next(2) == 0 ? alpha : (byte)0;
                        pixels[x * 4] = pixels[x * 4 + 1] = pixels[x * 4 + 2] = value;
                        pixels[x * 4 + 3] = alpha;
                    }
                    Marshal.Copy(pixels, 0, frame.Address + y * frame.RowBytes, pixels.Length);
                }
            }
            return new ImageBrush(bitmap)
            {
                TileMode = TileMode.Tile,
                DestinationRect = new RelativeRect(0, 0, size, size, RelativeUnit.Absolute),
                Stretch = Stretch.None,
            };
        });

        private static Rectangle Layer() => new() { IsHitTestVisible = false };

        private static Path City(Geometry outline) =>
            new() { Data = outline, IsHitTestVisible = false, RenderTransformOrigin = RelativePoint.TopLeft };

        private static Color Colour(string key, Color fallback) =>
            LauncherTheme.Brush(key) is ISolidColorBrush { Color: var colour } ? colour : fallback;

        private static RadialGradientBrush Radial(double x, double y, double rx, double ry, params (Color Colour, double Offset)[] stops)
        {
            var brush = new RadialGradientBrush
            {
                Center = new RelativePoint(x, y, RelativeUnit.Relative),
                GradientOrigin = new RelativePoint(x, y, RelativeUnit.Relative),
                RadiusX = new RelativeScalar(rx, RelativeUnit.Relative),
                RadiusY = new RelativeScalar(ry, RelativeUnit.Relative),
            };
            foreach (var (colour, offset) in stops) brush.GradientStops.Add(new GradientStop(colour, offset));
            return brush;
        }

        private static LinearGradientBrush Vertical(params (Color Colour, double Offset)[] stops)
        {
            var brush = new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
            };
            foreach (var (colour, offset) in stops) brush.GradientStops.Add(new GradientStop(colour, offset));
            return brush;
        }
    }
}
