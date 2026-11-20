"""After a build that stopped, Triforce Setup's build page offers Copy report beside Show details (setup/Guard.cs Report;
no Setup is started, no log of this PC is read and the clipboard is not touched).

A player with a failed build had to open the details, select and copy them, and find setup.log in the install folder; what
they pasted carried their Windows account's name in nearly every path.

Compiles Guard.cs with tests/setup_report_test.cs and runs it: the last 200 lines of each log; the account's folder read as
%USERPROFILE% however a tool printed it (either slash, doubled backslashes, other case, its short name), while another
account whose name only begins the same, and a name with characters a pattern would read otherwise, are left as they are;
the report put together with nothing of the name in it. Reads the source for the window's side: the link is on the build
page and shown only after a build that stopped by itself, and what it copies is made from the page's details, setup.log
and the account's folder.

    python -I -B tests/test_setup_report.py
"""
import re
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    xaml = setup_cs.source("setup/Main.xaml")
    page = xaml[xaml.index('<Grid x:Name="BuildPage"'):xaml.index("<!-- a question that must be answered")]
    if not page.index('x:Name="ShowLog"') < page.index('<Button x:Name="CopyReport" Style="{StaticResource Link}"') < page.index('x:Name="OpenButton"'):
        raise SystemExit("setup/Main.xaml: Copy report is no longer a link beside Show details on the build page")
    setup_cs.need(page[page.index('x:Name="CopyReport"'):].split("/>")[0], 'Visibility="Collapsed"', "setup/Main.xaml: Copy report shows before any build stopped")
    shell = setup_cs.source("setup/Shell.cs")
    for needle, why in (
            ('CopyReport = Part<Button>("CopyReport");', "the link is not found in the window"),
            ("CopyReport.Click += (_, _) => CopyTheReport();", "the link no longer copies the report"),
            ('SetButtons(back: true, report: failure != "cancelled");', "the link is no longer offered after a build that stopped, or is after one the player cancelled"),
            ("CopyReport.Visibility = report ? Visibility.Visible : Visibility.Collapsed;", "the link is no longer hidden while a build runs and after one that ended well")):
        setup_cs.need(shell, needle, "setup/Shell.cs: " + why)
    if len(re.findall(r"SetButtons\([^)]*report:", shell)) != 1:
        raise SystemExit("setup/Shell.cs: Copy report is offered somewhere else than after a build that stopped")
    copy = shell[shell.index("void CopyTheReport()"):shell.index("void ShowClock()")]
    for needle, why in (
            ("Environment.GetFolderPath(Environment.SpecialFolder.UserProfile)", "the account's folder is no longer taken out of the report"),
            ("Report.Compose(known, Log.Text, log, profile, Report.Short(profile))", "the report is no longer made of what Setup knows, the build's details and setup.log, without the account's folder and its short name"),
            ('File.ReadAllText(System.IO.Path.Combine(home, "setup.log"))', "setup.log is no longer read from beside Setup"),
            ("Clipboard.SetDataObject(", "the report no longer goes to the clipboard"),
            ("Program.Version", "the report no longer says which Setup it is from"),
            ("buildLine", "the report no longer says what the build was started with")):
        setup_cs.need(copy, needle, "setup/Shell.cs CopyTheReport: " + why)


def main():
    check_source()
    setup_cs.run_fixture("setup_report_test.cs")


if __name__ == "__main__":
    main()
