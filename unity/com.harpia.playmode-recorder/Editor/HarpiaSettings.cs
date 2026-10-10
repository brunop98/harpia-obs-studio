// The package's settings (per user, per machine: EditorPrefs) and its page in
// Edit > Preferences > Harpia Recorder.

using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace Harpia.PlayModeRecorder
{
    public enum RecordArea
    {
        GamePicture = 0,   // just the rendered game, without the Game view's toolbar or letterbox bars
        WholeGameView = 1, // everything under the Game view's toolbar
    }

    public static class HarpiaSettings
    {
        const string Prefix = "Harpia.PlayModeRecorder.";
        public const int DefaultPort = 47811; // Harpia's default (Settings > System)

        public static bool Enabled
        {
            get { return EditorPrefs.GetBool(Prefix + "Enabled", true); }
            set { EditorPrefs.SetBool(Prefix + "Enabled", value); }
        }

        public static int Port
        {
            get { return Mathf.Clamp(EditorPrefs.GetInt(Prefix + "Port", DefaultPort), 1024, 65535); }
            set { EditorPrefs.SetInt(Prefix + "Port", Mathf.Clamp(value, 1024, 65535)); }
        }

        // Play sessions shorter than this (paused time not counted) are not kept.
        public static float MinSeconds
        {
            get { return Mathf.Max(0f, EditorPrefs.GetFloat(Prefix + "MinSeconds", 3f)); }
            set { EditorPrefs.SetFloat(Prefix + "MinSeconds", Mathf.Max(0f, value)); }
        }

        public static RecordArea Area
        {
            get { return (RecordArea)EditorPrefs.GetInt(Prefix + "Area", (int)RecordArea.GamePicture); }
            set { EditorPrefs.SetInt(Prefix + "Area", (int)value); }
        }

        // A moment after entering Play Mode before the Game view is measured,
        // so "Maximize on Play" has finished changing the layout.
        public static int StartDelayMs
        {
            get { return Mathf.Clamp(EditorPrefs.GetInt(Prefix + "StartDelayMs", 300), 0, 5000); }
            set { EditorPrefs.SetInt(Prefix + "StartDelayMs", Mathf.Clamp(value, 0, 5000)); }
        }

        public static bool LogEachRecording
        {
            get { return EditorPrefs.GetBool(Prefix + "LogEachRecording", true); }
            set { EditorPrefs.SetBool(Prefix + "LogEachRecording", value); }
        }
    }

    static class HarpiaSettingsProvider
    {
        [SettingsProvider]
        public static SettingsProvider Create()
        {
            return new SettingsProvider("Preferences/Harpia Recorder", SettingsScope.User)
            {
                label = "Harpia Recorder",
                guiHandler = _ => Draw(),
                keywords = new HashSet<string> { "Harpia", "record", "recording", "Play Mode", "video", "capture" },
            };
        }

        static void Draw()
        {
            EditorGUIUtility.labelWidth = 220;
            EditorGUILayout.HelpBox(
                "Records the Game view with Harpia Recorder and Editor every time you enter Play Mode, using " +
                "Harpia's current preset (quality, frame rate, audio...). Pausing pauses the recording; " +
                "leaving Play Mode saves it to the preset's output folder. Harpia must be running, with " +
                "Unity control on (Harpia > Settings > System).",
                MessageType.Info);

            HarpiaSettings.Enabled = EditorGUILayout.Toggle(
                new GUIContent("Record Play Mode", "Start a recording on entering Play Mode."), HarpiaSettings.Enabled);
            using (new EditorGUI.DisabledScope(!HarpiaSettings.Enabled))
            {
                HarpiaSettings.Area = (RecordArea)EditorGUILayout.EnumPopup(
                    new GUIContent("Area", "Game Picture: only the rendered game. Whole Game View: everything " +
                                           "under the Game view's toolbar, letterbox bars included."),
                    HarpiaSettings.Area);
                HarpiaSettings.MinSeconds = EditorGUILayout.FloatField(
                    new GUIContent("Don't keep runs shorter than (s)",
                        "Quick test plays shorter than this are deleted instead of saved. 0 keeps every run."),
                    HarpiaSettings.MinSeconds);
                HarpiaSettings.Port = EditorGUILayout.IntField(
                    new GUIContent("Harpia port", "The same number as in Harpia > Settings > System (default " +
                                                  HarpiaSettings.DefaultPort + ")."),
                    HarpiaSettings.Port);
                HarpiaSettings.StartDelayMs = EditorGUILayout.IntField(
                    new GUIContent("Start delay (ms)",
                        "Wait this long after entering Play Mode before measuring the Game view, so a " +
                        "maximized Game view has settled."),
                    HarpiaSettings.StartDelayMs);
                HarpiaSettings.LogEachRecording = EditorGUILayout.Toggle(
                    new GUIContent("Log each start and stop", "Problems are always logged."),
                    HarpiaSettings.LogEachRecording);
            }

            EditorGUILayout.Space();
            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("Test Connection", GUILayout.Width(150)))
                    HarpiaMenu.TestConnection();
                if (GUILayout.Button("Show Recording Area", GUILayout.Width(150)))
                    HarpiaMenu.ShowRecordingArea();
            }
            EditorGUILayout.LabelField(
                "Show Recording Area asks Harpia to outline the Game view it would record. If the outline " +
                "is off, try the other Area, and check that all displays use the same scaling.",
                EditorStyles.wordWrappedMiniLabel);
        }
    }
}
