using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace STFInstaller;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "--extract-test")
        {
            StfsExtractor.Extract(args[1], args[2], (_, _) => { });
            File.WriteAllText(Path.Combine(args[2], ".extract-ok"), "ok");
            return;
        }
        ApplicationConfiguration.Initialize();

        Application.Run(new InstallerForm());
    }
}

internal sealed class InstallerForm : Form
{
    private const string ExpectedSha256 = "6AC5A4B1D473191D8C0FA5008033A3A5F96AF2F6BCB8D5E54B3A6213A51F318E";
    private readonly string installDir = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
    private readonly Dictionary<string, Image> backgrounds = new();
    private readonly Font titleFont = new("Arial", 28, FontStyle.Bold);
    private readonly Font subtitleFont = new("Arial", 15, FontStyle.Bold);
    private readonly Font menuFont = new("Arial Narrow", 22, FontStyle.Regular);
    private readonly Font smallFont = new("Arial Narrow", 16, FontStyle.Regular);
    private int page = 1;
    private bool russian;
    private bool sourceVerified;
    private string? sourceFile;
    private bool busy;
    private int progress;
    private string progressText = "";
    private Rectangle panelRect;
    private readonly List<(Rectangle Rect, string Id)> hitAreas = new();
    private readonly bool uninstallMode;

    public InstallerForm()
    {
        uninstallMode = Path.GetFileName(Environment.ProcessPath ?? "").Equals("uninstall.exe", StringComparison.OrdinalIgnoreCase);
        Text = "Sonic the Fighters Recompiled — Installer";
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? SystemIcons.Application;
        DoubleBuffered = true;
        KeyPreview = true;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        ClientSize = new Size(1280, 720);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.Black;
        Cursor = Cursors.Hand;
        LoadImages();
        page = uninstallMode ? 10 : 1;
        KeyDown += (_, e) => { if (e.KeyCode == Keys.Escape && !busy) Close(); };
        MouseDown += OnMouseDown;
    }

    private void LoadImages()
    {
        foreach (string name in new[] { "Sonic.png", "tails.png", "Knuckles.png", "super sonic.png", "eggman.png" })
        {
            using Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(name)
                ?? throw new InvalidOperationException($"Installer background resource '{name}' was not embedded.");
            backgrounds[name] = new Bitmap(stream);
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
        float scale = Math.Min(ClientSize.Width / 1920f, ClientSize.Height / 1080f);
        int w = (int)(1920 * scale), h = (int)(1080 * scale);
        int ox = (ClientSize.Width - w) / 2, oy = (ClientSize.Height - h) / 2;
        g.TranslateTransform(ox, oy);
        g.ScaleTransform(scale, scale);

        string bg = page switch
        {
            2 => "tails.png",
            3 => "Knuckles.png",
            4 => "super sonic.png",
            10 => "eggman.png",
            _ => "Sonic.png"
        };
        g.DrawImage(backgrounds[bg], 0, 0, 1920, 1080);
        using var shade = new SolidBrush(Color.FromArgb(45, 0, 0, 20));
        g.FillRectangle(shade, 0, 0, 1920, 1080);

        DrawHeader(g);
        hitAreas.Clear();
        if (page == 10) DrawUninstaller(g);
        else DrawInstallerPage(g);

        g.ResetTransform();
    }

    private void DrawHeader(Graphics g)
    {
        DrawCentered(g, "SONIC THE FIGHTERS: RECOMPILED", titleFont, Color.White, new RectangleF(0, 40, 1920, 60));
        DrawCentered(g, uninstallMode ? "UNINSTALLER" : "INSTALLER", subtitleFont, Color.FromArgb(90, 220, 255), new RectangleF(0, 103, 1920, 42));
    }

    private void DrawInstallerPage(Graphics g)
    {
        panelRect = new Rectangle(820, 300, 925, 510);
        DrawPanel(g, panelRect);
        switch (page)
        {
            case 1: DrawLanguage(g); break;
            case 2: DrawSource(g); break;
            case 3: DrawInstall(g); break;
            case 4: DrawComplete(g); break;
        }
    }

    private void DrawLanguage(Graphics g)
    {
        DrawText(g, russian ? "Пожалуйста, выберите язык." : "Please select a language.", menuFont, 875, 355, 800, 70);
        DrawChoice(g, "RUS", new Rectangle(885, 520, 360, 70), russian, "rus");
        DrawChoice(g, "ENG", new Rectangle(1280, 520, 360, 70), !russian, "eng");
        DrawButton(g, russian ? "ДАЛЕЕ" : "NEXT", new Rectangle(1390, 680, 250, 70), "next");
        DrawButton(g, russian ? "ВЫХОД" : "QUIT", new Rectangle(885, 680, 250, 70), "quit");
    }

    private void DrawSource(Graphics g)
    {
        DrawText(g, russian ? "Добавьте исходный файл игры." : "Add the sources for the game.", menuFont, 875, 350, 800, 70);
        if (sourceVerified)
            DrawText(g, russian ? "Файл игры успешно проверен." : "Game file successfully verified.", smallFont, 875, 480, 800, 55, Color.FromArgb(80, 255, 130));
        DrawButton(g, russian ? "ДОБАВИТЬ ФАЙЛ" : "ADD FILES", new Rectangle(875, 650, 390, 70), "add");
        DrawButton(g, russian ? "ДАЛЕЕ" : "NEXT", new Rectangle(1390, 650, 250, 70), "next", sourceVerified);
    }

    private void DrawInstall(Graphics g)
    {
        long required = CalculateRequiredBytes();
        long available = new DriveInfo(Path.GetPathRoot(installDir)!).AvailableFreeSpace;
        string body = russian
            ? $"Содержимое будет установлено в папку программы.\n\nТребуется места: {GiB(required)}\nДоступно места: {GiB(available)}"
            : $"The content will be installed to the program's folder.\n\nRequired space: {GiB(required)}\nAvailable space: {GiB(available)}";
        DrawText(g, body, menuFont, 875, 345, 800, 230);
        if (busy)
        {
            DrawProgress(g, new Rectangle(875, 650, 765, 48), progress);
            DrawText(g, progressText, smallFont, 875, 710, 765, 45);
        }
        else
        {
            DrawButton(g, russian ? "ДАЛЕЕ" : "NEXT", new Rectangle(1390, 650, 250, 70), "next", available > required);
        }
    }

    private void DrawComplete(Graphics g)
    {
        DrawText(g, russian ? "Игра успешно установлена!" : "Game installed successfully!", menuFont, 875, 390, 800, 80);
        DrawButton(g, russian ? "ИГРАТЬ" : "PLAY", new Rectangle(1390, 650, 250, 70), "play");
    }

    private void DrawUninstaller(Graphics g)
    {
        panelRect = new Rectangle(820, 300, 925, 510);
        DrawPanel(g, panelRect);
        DrawText(g, russian ? "Выберите действие." : "Choose an action.", menuFont, 875, 350, 800, 70);
        DrawButton(g, russian ? "ПЕРЕУСТАНОВИТЬ" : "REINSTALL", new Rectangle(875, 525, 430, 72), "reinstall");
        DrawButton(g, russian ? "УДАЛИТЬ ИГРУ" : "UNINSTALL", new Rectangle(875, 625, 430, 72), "uninstall");
        DrawButton(g, russian ? "ВЫХОД" : "QUIT", new Rectangle(1390, 625, 250, 72), "quit");
        DrawChoice(g, "RUS", new Rectangle(1390, 525, 110, 55), russian, "rus");
        DrawChoice(g, "ENG", new Rectangle(1525, 525, 110, 55), !russian, "eng");
    }

    private void OnMouseDown(object? sender, MouseEventArgs e)
    {
        if (busy) return;
        float scale = Math.Min(ClientSize.Width / 1920f, ClientSize.Height / 1080f);
        float ox = (ClientSize.Width - 1920 * scale) / 2f, oy = (ClientSize.Height - 1080 * scale) / 2f;
        var logical = new Point((int)((e.X - ox) / scale), (int)((e.Y - oy) / scale));
        string? id = hitAreas.FirstOrDefault(x => x.Rect.Contains(logical)).Id;
        switch (id)
        {
            case "rus": russian = true; Invalidate(); break;
            case "eng": russian = false; Invalidate(); break;
            case "quit": Close(); break;
            case "add": SelectSource(); break;
            case "next": Next(); break;
            case "play": FinishAndPlay(); break;
            case "reinstall": Reinstall(); break;
            case "uninstall": Uninstall(); break;
        }
    }

    private async void SelectSource()
    {
        using var dialog = new OpenFileDialog
        {
            Title = russian ? "Выберите исходный файл Sonic the Fighters" : "Select the Sonic the Fighters source file",
            Filter = russian ? "Все файлы (*.*)|*.*" : "All files (*.*)|*.*",
            CheckFileExists = true,
            Multiselect = false
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        UseWaitCursor = true;
        try
        {
            string hash = await Task.Run(() => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(dialog.FileName))));
            sourceVerified = hash.Equals(ExpectedSha256, StringComparison.OrdinalIgnoreCase);
            sourceFile = sourceVerified ? dialog.FileName : null;
            if (!sourceVerified)
                MessageBox.Show(this,
                    russian ? "Выбранный файл не является поддерживаемым файлом Sonic the Fighters." : "The selected file is not a supported Sonic the Fighters game file.",
                    "Sonic the Fighters Recompiled", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        catch (Exception ex) { ShowError(ex); }
        finally { UseWaitCursor = false; Invalidate(); }
    }

    private void Next()
    {
        if (page == 1) page = 2;
        else if (page == 2 && sourceVerified) page = 3;
        else if (page == 3) _ = InstallAsync();
        Invalidate();
    }

    private async Task InstallAsync()
    {
        if (sourceFile is null) return;
        busy = true; progress = 2; progressText = russian ? "Подготовка..." : "Preparing..."; Invalidate();
        try
        {
            await Task.Run(() =>
            {
                string dataDir = Path.Combine(installDir, "_unpacked");
                if (Directory.Exists(dataDir)) Directory.Delete(dataDir, true);
                Directory.CreateDirectory(dataDir);
                UpdateProgress(8, russian ? "Проверка игровых данных..." : "Verifying game data...");
                StfsExtractor.Extract(sourceFile, dataDir, (done, total) =>
                    UpdateProgress(8 + (int)(done * 76L / Math.Max(total, 1)), russian ? "Распаковка игровых данных..." : "Extracting game data..."));
                UpdateProgress(88, russian ? "Установка программы..." : "Installing program...");
                ExtractResource("stfrecompiled.exe", Path.Combine(installDir, "stfrecompiled.exe"));
                ExtractResource("rexruntime.dll", Path.Combine(installDir, "rexruntime.dll"));
                ExtractResource("rexgpu-xenos.dll", Path.Combine(installDir, "rexgpu-xenos.dll"));
                EnsureKeyboardInputConfig();
                var state = new InstallState { SourceSha256 = ExpectedSha256, InstalledAt = DateTimeOffset.Now };
                File.WriteAllText(Path.Combine(installDir, "stf-install.json"), JsonSerializer.Serialize(state, new JsonSerializerOptions { WriteIndented = true }));
                UpdateProgress(100, russian ? "Готово." : "Complete.");
            });
            await Task.Delay(350);
            page = 4;
        }
        catch (Exception ex)
        {
            try { string dir = Path.Combine(installDir, "_unpacked"); if (Directory.Exists(dir)) Directory.Delete(dir, true); } catch { }
            ShowError(ex);
        }
        finally { busy = false; Invalidate(); }
    }

    private void UpdateProgress(int value, string text)
    {
        progress = Math.Clamp(value, 0, 100); progressText = text;
        if (IsHandleCreated) BeginInvoke(Invalidate);
    }

    private void FinishAndPlay()
    {
        string self = Environment.ProcessPath!;
        string target = Path.Combine(installDir, "uninstall.exe");
        string game = Path.Combine(installDir, "stfrecompiled.exe");
        string args = "--game_data_root \"" + Path.Combine(installDir, "_unpacked") +
                      "\" --user_data_root \"" + Path.Combine(installDir, "userdata") +
                      "\" --gpu_plugin xenos --mnk_mode --no-audio_mute";
        string cmd = Path.Combine(Path.GetTempPath(), "stf-finish-" + Guid.NewGuid().ToString("N") + ".cmd");
        File.WriteAllText(cmd, $"@echo off\r\nping 127.0.0.1 -n 2 >nul\r\nmove /Y \"{self}\" \"{target}\" >nul\r\nstart \"\" \"{game}\" {args}\r\ndel \"%~f0\"\r\n", Encoding.ASCII);
        Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + cmd + "\"") { CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden });
        Close();
    }

    private void Reinstall()
    {
        if (MessageBox.Show(this, russian ? "Удалить установленные файлы и начать установку заново?" : "Remove the installed files and start installation again?", "Sonic the Fighters: Recompiled", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        RemoveInstalledFiles(false);
        string self = Environment.ProcessPath!;
        string target = Path.Combine(installDir, "install.exe");
        RunAfterExit($"move /Y \"{self}\" \"{target}\" >nul\r\nstart \"\" \"{target}\"");
    }

    private void Uninstall()
    {
        if (MessageBox.Show(this, russian ? "Полностью удалить игру?" : "Completely uninstall the game?", "Sonic the Fighters: Recompiled", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
        RemoveInstalledFiles(true);
        string self = Environment.ProcessPath!;
        RunAfterExit($"del /F /Q \"{self}\"");
    }

    private void RemoveInstalledFiles(bool removeRuntime)
    {
        foreach (string file in new[] { "stfrecompiled.exe", "stf-install.json" })
            try { File.Delete(Path.Combine(installDir, file)); } catch { }
        if (removeRuntime)
        {
            foreach (string file in new[] { "rexruntime.dll", "rexgpu-xenos.dll" })
                try { File.Delete(Path.Combine(installDir, file)); } catch { }
            try { File.Delete(Path.Combine(installDir, "stf_xbla.toml")); } catch { }
        }
        try { Directory.Delete(Path.Combine(installDir, "_unpacked"), true); } catch { }
        if (removeRuntime)
            try { Directory.Delete(Path.Combine(installDir, "userdata"), true); } catch { }
    }

    private void RunAfterExit(string commands)
    {
        string cmd = Path.Combine(Path.GetTempPath(), "stf-cleanup-" + Guid.NewGuid().ToString("N") + ".cmd");
        File.WriteAllText(cmd, $"@echo off\r\nping 127.0.0.1 -n 2 >nul\r\n{commands}\r\ndel \"%~f0\"\r\n", Encoding.ASCII);
        Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + cmd + "\"") { CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden });
        Close();
    }

    private static void ExtractResource(string name, string destination)
    {
        using Stream input = Assembly.GetExecutingAssembly().GetManifestResourceStream(name) ?? throw new InvalidOperationException("Missing embedded resource: " + name);
        using FileStream output = File.Create(destination);
        input.CopyTo(output);
    }

    private void EnsureKeyboardInputConfig()
    {
        string path = Path.Combine(installDir, "stf_xbla.toml");
        if (!File.Exists(path))
        {
            ExtractResource("stf_xbla.toml", path);
            return;
        }

        string[] lines = File.ReadAllLines(path);
        int modeLine = Array.FindIndex(lines, line =>
        {
            string[] parts = line.Split('=', 2);
            return parts.Length == 2 && parts[0].Trim().Equals("mnk_mode", StringComparison.OrdinalIgnoreCase);
        });
        if (modeLine < 0)
        {
            File.AppendAllText(path,
                (lines.Length > 0 && !string.IsNullOrWhiteSpace(lines[^1]) ? Environment.NewLine : "") +
                "mnk_mode = true" + Environment.NewLine);
        }
        else if (lines[modeLine].Split('=', 2)[1].Trim().Equals("false", StringComparison.OrdinalIgnoreCase))
        {
            lines[modeLine] = "mnk_mode = true";
            File.WriteAllLines(path, lines);
        }
    }

    private long CalculateRequiredBytes()
    {
        long source = sourceFile is null ? 101_867_520 : new FileInfo(sourceFile).Length;
        using Stream? game = Assembly.GetExecutingAssembly().GetManifestResourceStream("stfrecompiled.exe");
        return source + (game?.Length ?? 40_000_000) + 64L * 1024 * 1024;
    }

    private static string GiB(long bytes) => $"{bytes / 1073741824d:0.00} GiB";
    private void ShowError(Exception ex) => MessageBox.Show(this, (russian ? "Ошибка установки:\n" : "Installation error:\n") + ex.Message, "Sonic the Fighters Recompiled", MessageBoxButtons.OK, MessageBoxIcon.Error);

    private void DrawPanel(Graphics g, Rectangle r)
    {
        using var fill = new SolidBrush(Color.FromArgb(205, 24, 44, 76));
        using var border = new Pen(Color.FromArgb(225, 240, 245, 255), 4);
        using var inner = new Pen(Color.FromArgb(150, 80, 120, 180), 2);
        g.FillRectangle(fill, r); g.DrawRectangle(border, r); g.DrawRectangle(inner, Rectangle.Inflate(r, -9, -9));
    }

    private void DrawChoice(Graphics g, string text, Rectangle r, bool selected, string id)
    {
        using var fill = new SolidBrush(selected ? Color.FromArgb(225, 0, 165, 225) : Color.FromArgb(175, 25, 48, 86));
        using var border = new Pen(Color.White, selected ? 3 : 1);
        g.FillRectangle(fill, r); g.DrawRectangle(border, r);
        DrawCentered(g, text, smallFont, Color.White, r);
        hitAreas.Add((r, id));
    }

    private void DrawButton(Graphics g, string text, Rectangle r, string id, bool enabled = true)
    {
        using var fill = new SolidBrush(enabled ? Color.FromArgb(235, 0, 155, 218) : Color.FromArgb(180, 60, 72, 90));
        using var border = new Pen(enabled ? Color.White : Color.Gray, 3);
        g.FillRectangle(fill, r); g.DrawRectangle(border, r);
        DrawCentered(g, text, smallFont, enabled ? Color.White : Color.Gray, r);
        if (enabled) hitAreas.Add((r, id));
    }

    private static void DrawProgress(Graphics g, Rectangle r, int value)
    {
        using var back = new SolidBrush(Color.FromArgb(220, 12, 25, 45));
        using var fill = new SolidBrush(Color.FromArgb(255, 0, 178, 235));
        using var border = new Pen(Color.White, 3);
        g.FillRectangle(back, r);
        g.FillRectangle(fill, r.X + 4, r.Y + 4, (r.Width - 8) * value / 100, r.Height - 8);
        g.DrawRectangle(border, r);
    }

    private static void DrawText(Graphics g, string text, Font font, float x, float y, float w, float h, Color? color = null)
    {
        using var brush = new SolidBrush(color ?? Color.White);
        using var format = new StringFormat { Alignment = StringAlignment.Near, LineAlignment = StringAlignment.Near };
        g.DrawString(text, font, brush, new RectangleF(x, y, w, h), format);
    }

    private static void DrawCentered(Graphics g, string text, Font font, Color color, RectangleF r)
    {
        using var brush = new SolidBrush(color);
        using var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
        g.DrawString(text, font, brush, r, format);
    }

    private sealed class InstallState { public string SourceSha256 { get; set; } = ""; public DateTimeOffset InstalledAt { get; set; } }
}

internal static class StfsExtractor
{
    public static void Extract(string packagePath, string destination, Action<int, int> report)
    {
        byte[] data = File.ReadAllBytes(packagePath);
        uint Integer(int offset, int length, bool little = false)
        {
            uint value = 0;
            if (little) for (int i = length - 1; i >= 0; --i) value = (value << 8) | data[offset + i];
            else for (int i = 0; i < length; ++i) value = (value << 8) | data[offset + i];
            return value;
        }
        string magic = Encoding.ASCII.GetString(data, 0, 4);
        if (magic is not ("LIVE" or "PIRS" or "CON ")) throw new InvalidDataException("Not an STFS package.");
        if ((data[0x37B] & 1) == 0) throw new InvalidDataException("Only the read-only STFS layout is supported.");
        long baseOffset = (Integer(0x340, 4) + 4095) & ~4095L;
        int totalBlocks = checked((int)Integer(0x395, 4));
        (byte[] Content, int Next) Block(int index)
        {
            if (index < 0 || index >= totalBlocks) throw new InvalidDataException("STFS block index is out of range.");
            long physical = index, level = 170;
            for (int i = 0; i < 3; ++i) { physical += (index + level) / level; if (index < level) break; level *= 170; }
            long offset = baseOffset + physical * 4096;
            if (offset + 4096 > data.LongLength) throw new InvalidDataException("Truncated STFS block.");
            byte[] content = data.AsSpan((int)offset, 4096).ToArray();
            long table = index < 170 ? 0 : (index / 170) * 171L + index / 28900 + 1 + (index >= 28900 ? 1 : 0);
            long record = baseOffset + table * 4096 + (index % 170) * 24L;
            byte[] actual = SHA1.HashData(content);
            if (!actual.AsSpan().SequenceEqual(data.AsSpan((int)record, 20))) throw new InvalidDataException($"STFS block {index} failed SHA-1 verification.");
            int next = (data[record + 21] << 16) | (data[record + 22] << 8) | data[record + 23];
            return (content, next);
        }

        int tableIndex = checked((int)Integer(0x37E, 3, true));
        int tableCount = checked((int)Integer(0x37C, 2, true));
        var entries = new List<Entry>();
        var seenTables = new HashSet<int>();
        for (int t = 0; t < tableCount; ++t)
        {
            if (!seenTables.Add(tableIndex)) throw new InvalidDataException("STFS directory chain cycle.");
            var block = Block(tableIndex); tableIndex = block.Next;
            for (int offset = 0; offset < 4096; offset += 64)
            {
                ReadOnlySpan<byte> raw = block.Content.AsSpan(offset, 64);
                if (raw[0] == 0) break;
                int nameLength = raw[40] & 63;
                string name = Encoding.UTF8.GetString(raw[..nameLength]);
                if (string.IsNullOrWhiteSpace(name) || name is "." or ".." || name.IndexOfAny(new[] { '/', '\\', ':' }) >= 0) throw new InvalidDataException("Unsafe STFS entry name.");
                int parent = (raw[50] << 8) | raw[51];
                if (parent != 65535 && (parent >= entries.Count || !entries[parent].Directory)) throw new InvalidDataException("Invalid STFS directory parent.");
                string relative = parent == 65535 ? name : Path.Combine(entries[parent].RelativePath, name);
                entries.Add(new Entry(relative, (raw[40] & 128) != 0,
                    (raw[52] << 24) | (raw[53] << 16) | (raw[54] << 8) | raw[55],
                    raw[47] | (raw[48] << 8) | (raw[49] << 16)));
            }
        }

        string root = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
        int completed = 0;
        foreach (Entry entry in entries)
        {
            string target = Path.GetFullPath(Path.Combine(destination, entry.RelativePath));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("STFS path escapes destination.");
            if (entry.Directory) Directory.CreateDirectory(target);
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                using FileStream output = File.Create(target);
                var seen = new HashSet<int>();
                int remaining = entry.Size, index = entry.Start;
                while (remaining > 0)
                {
                    if (!seen.Add(index)) throw new InvalidDataException("STFS file chain cycle.");
                    var block = Block(index); index = block.Next;
                    int count = Math.Min(4096, remaining); output.Write(block.Content, 0, count); remaining -= count;
                }
            }
            report(++completed, entries.Count);
        }
    }

    private sealed record Entry(string RelativePath, bool Directory, int Size, int Start);
}
