// What this launcher is and where Triforce lives on the internet. Version comes from the TriforceVersion property
// (Triforce.Logic.csproj): the release tag exactly, "v0.5.2", or "dev" for a local build.
namespace Triforce.Logic;

public static partial class BuildInfo
{
    /// <summary>A local build: never replaces itself from a release (old Setup.cs SetupNewer: Program.Version != "dev").</summary>
    public static bool IsDev => Version == "dev";

    // (11-13: Triforce's own home. Releases are published in triforce/downloads on its Forgejo; the list is asked on the main
    // domain, which passes /api/v1 on, so the address outlives whatever answers behind it. Forgejo's list has GitHub's
    // names (tag_name, assets[].name / browser_download_url / size) and takes limit= where GitHub took per_page=.
    // Up to v0.3.5 this was api.github.com/repos/ruckus7382sfgsgdf/CBeta0.2: those builds ask there and nowhere else)
    public const string Home = "https://triforceproject.com";
    public const string ReleaseApi = Home + "/api/v1/repos/triforce/downloads/releases?limit=1";
    /// <summary>The Forgejo downloads repository (Linux: releases/download/&lt;tag&gt;/triforce-linux.txt).</summary>
    public const string Downloads = "https://git.triforceproject.com/triforce/downloads";

    /// <summary>The app itself. Every install, shortcut, Apps-list entry and every built game's triforce_version.txt builder=
    /// line names this file: it must keep this name (research/02 2.8 point 7).</summary>
    public const string SetupAsset = "TriforceSetup.exe";
    public const string ToolsAsset = "Triforce-Tools.zip";
    // ((Triforce, 11-18) the tools in two parts (setup/pack_tools.ps1): the base, which does not change from one release to the
    // next (the compiler, Python, the libraries: 95 % of the zip), and the code, with the base's list (Tools.BaseList). An update
    // fetches the code and keeps the base it has: 12 MB instead of 284. Triforce-Tools.zip stays for older Setups and a first install)
    public const string ToolsCodeAsset = "Triforce-Tools-code.zip";
    public const string ToolsBaseAsset = "Triforce-Tools-base.zip";
    /// <summary>The Linux parts list (builder/linux/linux_update.py LIST_NAME).</summary>
    public const string LinuxListAsset = "triforce-linux.txt";
    public const string AppName = "Triforce";

    /// <summary>The User-Agent every request to the release server carries (old Shell.cs Http).</summary>
    public static string UserAgent => "TriforceSetup/" + Version;
}
