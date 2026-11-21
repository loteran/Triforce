// The graphics cards of this PC (read only): for NVIDIA DLSS (offered on NVIDIA cards only, DESIGN 6) and the shader step's
// honest time (DESIGN 6 Build: "About 13 min on an RTX 4070"; "1 to 2 hours on an RX 6800 XT").
using System.Runtime.Versioning;

namespace Triforce.Logic;

public enum GpuVendor { Nvidia, Amd, Intel, Other }

public sealed record GpuInfo(GpuVendor Vendor, string Name);

public static class Gpus
{
    static GpuVendor VendorOf(string text, uint? id = null)
    {
        if (id == 0x10DE || text.Contains("NVIDIA", StringComparison.OrdinalIgnoreCase)) return GpuVendor.Nvidia;
        if (id == 0x1002 || text.Contains("AMD", StringComparison.OrdinalIgnoreCase) || text.Contains("Radeon", StringComparison.OrdinalIgnoreCase) ||
            text.Contains("Advanced Micro Devices", StringComparison.OrdinalIgnoreCase)) return GpuVendor.Amd;
        if (id == 0x8086 || text.Contains("Intel", StringComparison.OrdinalIgnoreCase)) return GpuVendor.Intel;
        return GpuVendor.Other;
    }

    /// <summary>The display adapters: Windows from the display class's driver keys (HKLM, read only), Linux from /sys/class/drm.</summary>
    public static IReadOnlyList<GpuInfo> List()
    {
        try { return OperatingSystem.IsWindows() ? Windows() : Linux(); } catch (Exception) { return Array.Empty<GpuInfo>(); }
    }

    [SupportedOSPlatform("windows")]
    static IReadOnlyList<GpuInfo> Windows()
    {
        var found = new List<GpuInfo>();
        using var cls = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}", false);
        if (cls == null) return found;
        foreach (var sub in cls.GetSubKeyNames())
        {
            if (sub.Length != 4 || !sub.All(char.IsDigit)) continue;
            try
            {
                using var k = cls.OpenSubKey(sub, false);
                var name = k?.GetValue("DriverDesc") as string;
                if (string.IsNullOrWhiteSpace(name)) continue;
                var provider = k!.GetValue("ProviderName") as string ?? "";
                var v = VendorOf(name + " " + provider);
                if (v == GpuVendor.Other && (name.Contains("Basic", StringComparison.OrdinalIgnoreCase) || name.Contains("Remote", StringComparison.OrdinalIgnoreCase))) continue;
                if (found.All(g => g.Name != name)) found.Add(new GpuInfo(v, name));
            }
            catch (Exception) { }
        }
        return found;
    }

    static IReadOnlyList<GpuInfo> Linux()
    {
        var found = new List<GpuInfo>();
        if (!Directory.Exists("/sys/class/drm")) return found;
        foreach (var card in Directory.GetDirectories("/sys/class/drm", "card*"))
        {
            if (Path.GetFileName(card).Contains('-')) continue;   // (card0-DP-1: a connector)
            try
            {
                var vendor = File.ReadAllText(Path.Combine(card, "device", "vendor")).Trim();
                var device = File.Exists(Path.Combine(card, "device", "device")) ? File.ReadAllText(Path.Combine(card, "device", "device")).Trim() : "";
                var id = Convert.ToUInt32(vendor.Replace("0x", ""), 16);
                var v = VendorOf("", id);
                found.Add(new GpuInfo(v, v + " " + device));
            }
            catch (Exception) { }
        }
        if (found.All(g => g.Vendor != GpuVendor.Nvidia) && File.Exists("/proc/driver/nvidia/version")) found.Add(new GpuInfo(GpuVendor.Nvidia, "NVIDIA"));
        return found;
    }

    /// <summary>The card that most likely runs the game: an NVIDIA or AMD one before an integrated Intel one.</summary>
    public static GpuInfo? Main()
    {
        var all = List();
        return all.FirstOrDefault(g => g.Vendor == GpuVendor.Nvidia) ?? all.FirstOrDefault(g => g.Vendor == GpuVendor.Amd) ?? all.FirstOrDefault();
    }

    /// <summary>Is NVIDIA DLSS worth offering here: an NVIDIA card.</summary>
    public static bool HasNvidia() => List().Any(g => g.Vendor == GpuVendor.Nvidia);
}

/// <summary>NVIDIA DLSS in the game that is built: NVIDIA's files come under NVIDIA's own licences, so the choice holds only once
/// they are accepted (old Shell.cs:246-260, 1017-1029; hookshot_builder.py DLSS_LICENSE). Kept as Dlss=1: an update builds with
/// it again; an update of a folder built with DLSS keeps it whatever the setting says.</summary>
public sealed class Dlss
{
    readonly TriforceContext ctx;
    public Dlss(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>Show the DLSS row at all (an NVIDIA card).</summary>
    public bool Available => Gpus.HasNvidia();

    public bool Chosen => ctx.Settings.Dlss;

    /// <summary>The licence question, in the old words (11-15: the tick brings DLSS frame generation too: NVIDIA's Streamline (MIT)
    /// and nvngx_dlssg.dll, under the NVIDIA RTX SDKs License as DLSS itself).</summary>
    public string LicenseText => ctx.IsWindows
        ? "NVIDIA DLSS and DLSS frame generation are NVIDIA's, under NVIDIA's own licenses (github.com/NVIDIA/DLSS, LICENSE.txt; " +
          "github.com/NVIDIA-RTX/Streamline, license.txt). Their files are downloaded to this PC; a game folder built with them " +
          "must not be shared. Do you accept NVIDIA's licenses?"
        : "NVIDIA DLSS is NVIDIA's, not Triforce's. With it ticked, the build downloads NVIDIA's DLSS SDK (release v310.9.16) from " +
          "github.com/NVIDIA/DLSS, under NVIDIA's own license, the NVIDIA RTX SDKs license:\n\n" +
          "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt\n\n" +
          "Its files stay on this PC (in the builder's ngx-sdk folder, and libnvidia-ngx-dlss.so in the game folder's ngx folder). " +
          "Triforce never includes or shares them: never share a game folder built with them.\n\n" +
          "Do you accept NVIDIA's license?";

    public const string AcceptLabel = "I accept";

    /// <summary>"I accept": DLSS on, kept.</summary>
    public void Accept() => ctx.Settings.Dlss = true;
    /// <summary>The box un-ticked.</summary>
    public void TurnOff() => ctx.Settings.Dlss = false;

    /// <summary>Does a build use DLSS: the setting, or (an update) a folder built with it, which then stores the choice again
    /// (#bug-tracker 11-18 "Update &amp; merge removes NVIDIA DLSS").</summary>
    public bool ForBuild(string? updateFolder)
    {
        if (ctx.Settings.Dlss) return true;
        if (updateFolder != null && new Game(ctx).HasDlss(updateFolder))
        {
            ctx.Settings.Dlss = true;
            ctx.Log.Write("update: the folder was built with NVIDIA DLSS: updated with it");
            return true;
        }
        return false;
    }
}
