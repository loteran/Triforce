using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using Microsoft.Web.WebView2.Core;

namespace Triforce.Launcher.Host;

/// <summary>Windows: a plain Win32 window with no frame of its own (the page draws the bar and the three window buttons),
/// WebView2 filling it. The page is served from https://app.triforce/ by WebResourceRequested (a real secure origin, so
/// the page behaves exactly as in a browser). Dropped files arrive with their full paths
/// (chrome.webview.postMessageWithAdditionalObjects), which a page alone can never see.</summary>
public sealed class WinHost : IHost
{
    public const string Host = "app.triforce";
    public string Origin => "https://" + Host + "/";
    public bool CustomChrome => true;
    public event Action<string, string[]>? Message;

    readonly Content content;
    readonly string dataDir;
    readonly bool devTools;
    readonly int width, height;
    IntPtr hwnd;
    WndProc? proc;   // kept alive: the window calls it
    CoreWebView2Environment? env;
    CoreWebView2Controller? ctl;
    CoreWebView2? web;
    readonly ConcurrentQueue<Action> queue = new();
    readonly List<string> early = new();   // messages posted before the page could take them
    bool pageReady;

    public WinHost(Content content, string dataDir, bool devTools, int width = 1280, int height = 800)
    {
        this.content = content; this.dataDir = dataDir; this.devTools = devTools; this.width = width; this.height = height;
    }

    /// <summary>Is the WebView2 runtime on this PC? (Windows 11 always; Windows 10 nearly always via Edge.) The old Setup
    /// rolls back when the new exe exits nonzero fast, so the caller checks this first and exits 3 when it is missing.</summary>
    public static string? RuntimeVersion()
    {
        try { return CoreWebView2Environment.GetAvailableBrowserVersionString(); } catch { return null; }
    }

    public void Run()
    {
        SetProcessDpiAwarenessContext(new IntPtr(-4));   // per monitor v2 (the manifest says so too)
        var hinst = GetModuleHandle(null);
        proc = WindowProc;
        var wc = new WNDCLASSEX
        {
            cbSize = Marshal.SizeOf<WNDCLASSEX>(), style = 0x0003, lpfnWndProc = proc, hInstance = hinst,
            hCursor = LoadCursor(IntPtr.Zero, 32512), hbrBackground = CreateSolidBrush(0x00080a0a), lpszClassName = "TriforceLauncher",
            hIcon = LoadIcon(hinst, new IntPtr(32512)), hIconSm = LoadIcon(hinst, new IntPtr(32512)),
        };
        RegisterClassEx(ref wc);

        // Size: 1280 x 800 at the system's scale, never bigger than 92 % of the work area (keeps the 16:10 shape).
        var dpi = GetDpiForSystem();
        var work = new RECT(); SystemParametersInfo(0x0030, 0, ref work, 0);
        double k = dpi / 96.0, ww = work.right - work.left, wh = work.bottom - work.top;
        k = Math.Min(k, Math.Min(ww * .92 / width, wh * .92 / height));
        int w = (int)(width * k), h = (int)(height * k);
        int x = work.left + (int)(ww - w) / 2, y = work.top + (int)(wh - h) / 2;
        const uint style = 0x00CF0000 & ~0x00040000u;   // overlapped window, minus the sizing border (fixed size, maximize allowed)
        hwnd = CreateWindowEx(0, "TriforceLauncher", "Triforce", style, x, y, w, h, IntPtr.Zero, IntPtr.Zero, hinst, IntPtr.Zero);
        var m = new MARGINS { cyBottomHeight = 1 };
        DwmExtendFrameIntoClientArea(hwnd, ref m);          // keeps Windows' own shadow and rounded corners
        int dark = 1; DwmSetWindowAttribute(hwnd, 20, ref dark, 4);
        SetWindowPos(hwnd, IntPtr.Zero, 0, 0, 0, 0, 0x0027);  // frame changed: our WM_NCCALCSIZE takes effect

        SynchronizationContext.SetSynchronizationContext(new Ctx(this));
        _ = StartWebView();
        ShowWindow(hwnd, 5);
        UpdateWindow(hwnd);

        while (GetMessage(out var msg, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref msg);
            DispatchMessage(ref msg);
        }
        try { ctl?.Close(); } catch { }
    }

    async Task StartWebView()
    {
        try
        {
            var opt = new CoreWebView2EnvironmentOptions("--autoplay-policy=no-user-gesture-required --disable-features=msSmartScreenProtection");
            env = await CoreWebView2Environment.CreateAsync(null, dataDir, opt);
            ctl = await env.CreateCoreWebView2ControllerAsync(hwnd);
            ctl.DefaultBackgroundColor = System.Drawing.Color.FromArgb(255, 10, 9, 8);
            Resize();
            web = ctl.CoreWebView2;
            var s = web.Settings;
            s.AreDevToolsEnabled = devTools;
            s.AreDefaultContextMenusEnabled = devTools;
            s.AreBrowserAcceleratorKeysEnabled = devTools;
            s.IsZoomControlEnabled = false;
            s.IsStatusBarEnabled = false;
            s.IsPinchZoomEnabled = false;
            s.IsSwipeNavigationEnabled = false;
            s.IsPasswordAutosaveEnabled = false;
            s.IsGeneralAutofillEnabled = false;
            s.IsNonClientRegionSupportEnabled = true;   // CSS app-region: drag = our title bar (move, snap, double-click)
            web.AddWebResourceRequestedFilter("https://" + Host + "/*", CoreWebView2WebResourceContext.All);
            web.WebResourceRequested += OnRequest;
            web.WebMessageReceived += OnMessage;
            web.NewWindowRequested += (_, e) => { e.Handled = true; OpenUrl(e.Uri); };
            web.NavigationStarting += (_, e) =>
            {
                // Only our own page lives in this window; a link elsewhere opens in the player's browser.
                if (!e.Uri.StartsWith(Origin, StringComparison.OrdinalIgnoreCase)) { e.Cancel = true; OpenUrl(e.Uri); }
            };
            web.ProcessFailed += (_, e) => Log.Write("webview process failed: " + e.ProcessFailedKind);
            web.Navigate(Origin + "ui/index.html" + Environment.GetEnvironmentVariable("TRIFORCE_START"));
            ctl.MoveFocus(CoreWebView2MoveFocusReason.Programmatic);
        }
        catch (Exception e)
        {
            Log.Write("webview: " + e);
            MessageBox(hwnd, "Triforce could not open its window (Microsoft Edge WebView2 failed to start).\n\n" + e.Message, "Triforce", 0x10);
            Environment.Exit(3);
        }
    }

    void OnRequest(object? sender, CoreWebView2WebResourceRequestedEventArgs e)
    {
        var path = new Uri(e.Request.Uri).AbsolutePath;
        var got = content.Get(path);
        e.Response = got is { } g
            ? env!.CreateWebResourceResponse(new MemoryStream(g.data), 200, "OK", "Content-Type: " + g.mime + "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *")
            : env!.CreateWebResourceResponse(null, 404, "Not Found", "Cache-Control: no-store");
    }

    void OnMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        string json;
        try { json = e.TryGetWebMessageAsString(); } catch { json = e.WebMessageAsJson; }
        var files = new List<string>();
        try
        {
            if (e.AdditionalObjects != null)
                foreach (var o in e.AdditionalObjects) if (o is CoreWebView2File f) files.Add(f.Path);
        }
        catch { }
        if (json.Contains("\"t\":\"ready\""))
        {
            pageReady = true; foreach (var p in early) web!.PostWebMessageAsString(p); early.Clear();
            if (Environment.GetEnvironmentVariable("TRIFORCE_SHOT") is { Length: > 0 } shot) _ = Shot(shot);
        }
        Message?.Invoke(json, files.ToArray());
    }

    /// <summary>Testing: TRIFORCE_SHOT="a.png@3000;b.png@9000" saves the page as drawn at those times (ms after the page is up),
    /// then closes the window (a single "a.png" uses TRIFORCE_SHOT_DELAY, default 2500). TRIFORCE_SHOT_JS runs in the page first.
    /// Lets a change be checked without anyone looking at the screen.</summary>
    async Task Shot(string spec)
    {
        try
        {
            if (Environment.GetEnvironmentVariable("TRIFORCE_SHOT_JS") is { Length: > 0 } js) await web!.ExecuteScriptAsync(js);
            var t0 = DateTime.UtcNow;
            var def = int.TryParse(Environment.GetEnvironmentVariable("TRIFORCE_SHOT_DELAY"), out var d) ? d : 2500;
            foreach (var part in spec.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                var at = part.LastIndexOf('@');
                var (file, ms) = at > 2 && int.TryParse(part[(at + 1)..], out var m) ? (part[..at], m) : (part, def);
                var wait = TimeSpan.FromMilliseconds(ms) - (DateTime.UtcNow - t0);
                if (wait > TimeSpan.Zero) await Task.Delay(wait);
                using (var f = File.Create(file)) await web!.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, f);
                Log.Write("shot " + file);
            }
        }
        catch (Exception e) { Log.Write("shot: " + e.Message); }
        Close();
    }

    public void Post(string json) => Invoke(() =>
    {
        if (web == null || !pageReady) { early.Add(json); return; }
        web.PostWebMessageAsString(json);
    });

    public void Invoke(Action a)
    {
        queue.Enqueue(a);
        PostMessage(hwnd, WM_RUN, IntPtr.Zero, IntPtr.Zero);
    }

    public void Minimize() => Invoke(() => ShowWindow(hwnd, 6));
    public void ToggleMaximize() => Invoke(() => ShowWindow(hwnd, IsZoomed(hwnd) ? 9 : 3));
    public void Close() => Invoke(() => PostMessage(hwnd, 0x0010, IntPtr.Zero, IntPtr.Zero));
    public void Attention(string title, string text) => Invoke(() =>
    {
        if (GetForegroundWindow() == hwnd) return;
        var f = new FLASHWINFO { cbSize = (uint)Marshal.SizeOf<FLASHWINFO>(), hwnd = hwnd, dwFlags = 3 | 12 /* all, until foreground */ };
        FlashWindowEx(ref f);
    });
    [StructLayout(LayoutKind.Sequential)] struct FLASHWINFO { public uint cbSize; public IntPtr hwnd; public uint dwFlags, uCount, dwTimeout; }
    [DllImport("user32")] static extern bool FlashWindowEx(ref FLASHWINFO f);
    [DllImport("user32")] static extern IntPtr GetForegroundWindow();

    public void Activate() => Invoke(() => { if (IsIconic(hwnd)) ShowWindow(hwnd, 9); SetForegroundWindow(hwnd); });

    void Resize()
    {
        if (ctl == null) return;
        GetClientRect(hwnd, out var r);
        ctl.Bounds = new System.Drawing.Rectangle(0, 0, r.right - r.left, r.bottom - r.top);
    }

    const uint WM_RUN = 0x8001;
    IntPtr WindowProc(IntPtr h, uint msg, IntPtr wp, IntPtr lp)
    {
        switch (msg)
        {
            case WM_RUN:
                while (queue.TryDequeue(out var a)) { try { a(); } catch (Exception ex) { Log.Write("ui: " + ex); } }
                return IntPtr.Zero;
            case 0x0083: // WM_NCCALCSIZE: the whole window is client area (the page draws the bar)
                if (wp != IntPtr.Zero)
                {
                    if (IsZoomed(h))
                    {
                        // Maximized windows overhang the screen by the frame; pull the client back inside the monitor.
                        var p = Marshal.PtrToStructure<NCCALCSIZE_PARAMS>(lp);
                        var mon = MonitorFromWindow(h, 2);
                        var mi = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
                        GetMonitorInfo(mon, ref mi);
                        p.rgrc0 = mi.rcWork;
                        Marshal.StructureToPtr(p, lp, false);
                    }
                    return IntPtr.Zero;
                }
                break;
            case 0x0005: // WM_SIZE
                Resize();
                Post("{\"t\":\"max\",\"on\":" + (IsZoomed(h) ? "true" : "false") + "}");
                return IntPtr.Zero;
            case 0x0003: // WM_MOVE
                ctl?.NotifyParentWindowPositionChanged();
                break;
            case 0x0006: // WM_ACTIVATE: give the page the keyboard back (and so the controller)
                if ((wp.ToInt64() & 0xFFFF) != 0) ctl?.MoveFocus(CoreWebView2MoveFocusReason.Programmatic);
                break;
            case 0x02E0: // WM_DPICHANGED: take the size Windows suggests
                var rc = Marshal.PtrToStructure<RECT>(lp);
                SetWindowPos(h, IntPtr.Zero, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0x0014);
                return IntPtr.Zero;
            case 0x0002: // WM_DESTROY
                PostQuitMessage(0);
                return IntPtr.Zero;
        }
        return DefWindowProc(h, msg, wp, lp);
    }

    static void OpenUrl(string url)
    {
        if (!url.StartsWith("https://", StringComparison.OrdinalIgnoreCase) && !url.StartsWith("http://", StringComparison.OrdinalIgnoreCase)) return;
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true }); } catch { }
    }

    // ---------------- file pickers (the system's own: IFileOpenDialog) ----------------
    public string? PickFile(string title, string filterName, string pattern) => Pick(title, false, filterName, pattern);
    public string? PickFolder(string title) => Pick(title, true, null, null);

    string? Pick(string title, bool folder, string? filterName, string? pattern)
    {
        var dlg = (IFileOpenDialog)new FileOpenDialogRCW();
        try
        {
            dlg.GetOptions(out var o);
            dlg.SetOptions(o | 0x40 /*FORCEFILESYSTEM*/ | (folder ? 0x20u /*PICKFOLDERS*/ : 0) | 0x1000 /*FILEMUSTEXIST*/);
            dlg.SetTitle(title);
            if (!folder && filterName != null)
                dlg.SetFileTypes(2, new[] { new COMDLG_FILTERSPEC { pszName = filterName, pszSpec = pattern! }, new COMDLG_FILTERSPEC { pszName = "All files", pszSpec = "*.*" } });
            if (dlg.Show(hwnd) != 0) return null;
            dlg.GetResult(out var item);
            item.GetDisplayName(0x80058000 /*SIGDN_FILESYSPATH*/, out var path);
            return path;
        }
        catch (Exception e) { Log.Write("picker: " + e.Message); return null; }
        finally { Marshal.ReleaseComObject(dlg); }
    }

    sealed class Ctx(WinHost host) : SynchronizationContext
    {
        public override void Post(SendOrPostCallback d, object? state) => host.Invoke(() => d(state));
        public override void Send(SendOrPostCallback d, object? state) => host.Invoke(() => d(state));
        public override SynchronizationContext CreateCopy() => this;
    }

    // ---------------- Win32 ----------------
    delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASSEX
    {
        public int cbSize; public uint style; public WndProc lpfnWndProc; public int cbClsExtra, cbWndExtra; public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        public string? lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
    }
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }
    [StructLayout(LayoutKind.Sequential)] struct MARGINS { public int cxLeftWidth, cxRightWidth, cyTopHeight, cyBottomHeight; }
    [StructLayout(LayoutKind.Sequential)] struct NCCALCSIZE_PARAMS { public RECT rgrc0, rgrc1, rgrc2; public IntPtr lppos; }
    [StructLayout(LayoutKind.Sequential)] struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }

    [DllImport("user32", CharSet = CharSet.Unicode)] static extern ushort RegisterClassEx(ref WNDCLASSEX c);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowEx(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr p);
    [DllImport("user32")] static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern int GetMessage(out MSG m, IntPtr h, uint a, uint b);
    [DllImport("user32")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32")] static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("user32")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern void PostQuitMessage(int c);
    [DllImport("user32")] static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32")] static extern bool UpdateWindow(IntPtr h);
    [DllImport("user32")] static extern bool IsZoomed(IntPtr h);
    [DllImport("user32")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
    [DllImport("user32")] static extern IntPtr LoadCursor(IntPtr i, int id);
    [DllImport("user32")] static extern IntPtr LoadIcon(IntPtr i, IntPtr id);
    [DllImport("user32")] static extern uint GetDpiForSystem();
    [DllImport("user32")] static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32")] static extern bool SystemParametersInfo(uint a, uint b, ref RECT r, uint c);
    [DllImport("user32")] static extern IntPtr MonitorFromWindow(IntPtr h, uint f);
    [DllImport("user32")] static extern bool GetMonitorInfo(IntPtr m, ref MONITORINFO i);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int MessageBox(IntPtr h, string t, string c, uint f);
    [DllImport("gdi32")] static extern IntPtr CreateSolidBrush(uint c);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string? n);
    [DllImport("dwmapi")] static extern int DwmExtendFrameIntoClientArea(IntPtr h, ref MARGINS m);
    [DllImport("dwmapi")] static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int s);

    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialogRCW { }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct COMDLG_FILTERSPEC { public string pszName, pszSpec; }
    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem
    {
        void BindToHandler(); void GetParent();
        void GetDisplayName(uint sigdn, [MarshalAs(UnmanagedType.LPWStr)] out string name);
        void GetAttributes(); void Compare();
    }
    [ComImport, Guid("d57c7288-d4ad-4768-be02-9d969532d960"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileOpenDialog
    {
        [PreserveSig] int Show(IntPtr parent);
        void SetFileTypes(uint n, [MarshalAs(UnmanagedType.LPArray)] COMDLG_FILTERSPEC[] specs);
        void SetFileTypeIndex(uint i); void GetFileTypeIndex(out uint i);
        void Advise(); void Unadvise();
        void SetOptions(uint o); void GetOptions(out uint o);
        void SetDefaultFolder(IShellItem i); void SetFolder(IShellItem i); void GetFolder(out IShellItem i); void GetCurrentSelection(out IShellItem i);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string n); void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string n);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string t);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string t); void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string t);
        void GetResult(out IShellItem i);
        void AddPlace(IShellItem i, int fdap); void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string e);
        void Close(int hr); void SetClientGuid(ref Guid g); void ClearClientData(); void SetFilter(IntPtr f);
        void GetResults(out IntPtr e); void GetSelectedItems(out IntPtr e);
    }
}
