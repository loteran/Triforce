namespace Triforce.Launcher.Host;

/// <summary>The window the page lives in. Windows: WinHost (WebView2 in our own frameless window). Linux: LinuxHost
/// (Photino, WebKitGTK, the desktop's own window frame).</summary>
public interface IHost
{
    /// <summary>Opens the window on the page and returns when it closes. Call on the thread that made the host.</summary>
    void Run();

    /// <summary>Sends one JSON message to the page. Any thread.</summary>
    void Post(string json);

    /// <summary>Runs an action on the window's thread.</summary>
    void Invoke(Action a);

    /// <summary>A message from the page: its JSON, and the full paths of any files dropped with it (Windows hands those over
    /// with the drop; Linux hands over file:// links in the JSON instead).</summary>
    event Action<string, string[]> Message;

    void Minimize();
    void ToggleMaximize();
    void Close();
    void Activate();
    /// <summary>Asks for the player's eye without taking the focus (Windows: the taskbar button flashes until the window is
    /// in front; Linux: a desktop notification).</summary>
    void Attention(string title, string text);

    /// <summary>The system's file picker. Null when cancelled.</summary>
    string? PickFile(string title, string filterName, string pattern);
    string? PickFolder(string title);

    /// <summary>True when the page draws the title bar itself (Windows); false when the desktop draws it (Linux).</summary>
    bool CustomChrome { get; }

    /// <summary>Where the page is served from, e.g. https://app.triforce/ (ends with a slash).</summary>
    string Origin { get; }
}
