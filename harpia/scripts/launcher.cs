// Harpia.exe launcher. Sits at the distribution root and starts the real
// recorder in bin\64bit with the correct working directory, because libobs
// resolves its core data as "..\..\data\libobs" relative to the CWD (see
// libobs/obs-windows.c). Double-clicking this from the zip root "just works".
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows.Forms;

static class HarpiaLauncher
{
    [STAThread]
    static int Main(string[] args)
    {
        string root = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
        string binDir = Path.Combine(root, "bin", "64bit");
        string exe = Path.Combine(binDir, "harpia.exe");

        if (!File.Exists(exe))
        {
            MessageBox.Show(
                "harpia.exe was not found at:\n" + exe +
                "\n\nThis launcher must sit at the root of the Harpia folder " +
                "(next to the bin, data and obs-plugins folders).",
                "Harpia", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }

        var psi = new ProcessStartInfo
        {
            FileName = exe,
            WorkingDirectory = binDir,   // libobs needs CWD = bin\64bit
            UseShellExecute = false,
        };
        if (args.Length > 0)
            psi.Arguments = string.Join(" ", args);

        try
        {
            Process.Start(psi);
        }
        catch (Exception ex)
        {
            MessageBox.Show("Failed to start harpia.exe:\n" + ex.Message,
                "Harpia", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
        return 0;
    }
}
