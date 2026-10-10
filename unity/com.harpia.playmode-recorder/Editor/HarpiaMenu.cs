// Tools > Harpia Recorder: switch Play Mode recording on and off, check the
// connection, and see the area Harpia would record.

using UnityEditor;
using UnityEngine;

namespace Harpia.PlayModeRecorder
{
    public static class HarpiaMenu
    {
        const string Root = "Tools/Harpia Recorder/";
        const string Tag = "[Harpia] ";

        [MenuItem(Root + "Record Play Mode", false, 1)]
        static void ToggleEnabled()
        {
            HarpiaSettings.Enabled = !HarpiaSettings.Enabled;
            Debug.Log(Tag + (HarpiaSettings.Enabled ? "Play Mode will be recorded." : "Play Mode recording is off."));
        }

        [MenuItem(Root + "Record Play Mode", true)]
        static bool ToggleEnabledValidate()
        {
            Menu.SetChecked(Root + "Record Play Mode", HarpiaSettings.Enabled);
            return true;
        }

        [MenuItem(Root + "Test Connection", false, 20)]
        public static void TestConnection()
        {
            HarpiaReply r = PlayModeRecorder.Client().Status();
            if (!r.Ok)
            {
                Debug.LogWarning(Tag + r.Error + (r.Unreachable
                    ? ". Start Harpia Recorder and Editor, with Unity control on (Settings > System), and check the port in Preferences > Harpia Recorder."
                    : "."));
                return;
            }
            string app = Json.GetString(r.Body, "app");
            string version = Json.GetString(r.Body, "version");
            string preset = Json.GetString(r.Body, "preset");
            Debug.Log(Tag + "Connected to " + (app.Length > 0 ? app : "Harpia") +
                      (version.Length > 0 ? " " + version : "") + " -- " + r.State +
                      (preset.Length > 0 ? ", preset \"" + preset + "\"" : "") + ".");
        }

        [MenuItem(Root + "Show Recording Area", false, 21)]
        public static void ShowRecordingArea()
        {
            PixelRect area;
            string why;
            if (!GameViewArea.TryGet(HarpiaSettings.Area, out area, out why))
            {
                Debug.LogWarning(Tag + "Cannot measure the Game view: " + why + ".");
                return;
            }
            HarpiaReply r = PlayModeRecorder.Client().ShowArea(area.X, area.Y, area.Width, area.Height);
            if (r.Ok)
                Debug.Log(Tag + "Harpia is outlining " + area + " for a moment.");
            else
                Debug.LogWarning(Tag + r.Error + ".");
        }

        [MenuItem(Root + "Preferences...", false, 40)]
        static void OpenPreferences()
        {
            SettingsService.OpenUserPreferences("Preferences/Harpia Recorder");
        }
    }
}
