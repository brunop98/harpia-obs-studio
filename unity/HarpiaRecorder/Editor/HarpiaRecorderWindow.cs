// Window > Harpia Recorder (also Tools > Harpia Recorder > Open Window): the
// connection to Harpia, the Play Mode recording settings, and Harpia's
// recordings -- watch, open, edit or delete a take without leaving Unity.
//
//   The top line says whether Harpia is running and what it is doing. It is
//   asked once a second while the window is open, and not at all when closed.
//   Recordings: newest first, the same list as Harpia's Recent strip and Clip
//   Library. Click to select; double-click or Enter plays it; right-click for
//   the rest; Delete moves it to the recycle bin, after asking.
//   Settings: the same as Edit > Preferences > Harpia Recorder, plus what
//   would be recorded right now (the Game view's picture, measured live).

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.IMGUI.Controls;
using UnityEngine;

namespace Harpia.PlayModeRecorder
{
    public sealed class HarpiaRecorderWindow : EditorWindow
    {
        const string Tag = "[Harpia] ";
        const int ListLimit = 300;
        const float RowHeight = 62f;
        const float ThumbWidth = 92f;  // 16:9, so about 52 high
        const int ThumbTries = 15;     // about 10 s of "still being made", then a placeholder
        const int PendingAsks = 5;     // re-asks while durations are being read
        static readonly string[] Tabs = { "Recordings", "Settings" };

        [SerializeField] int tab;
        [SerializeField] string search = "";
        [SerializeField] string selectedPath = "";
        [SerializeField] Vector2 scroll;
        [SerializeField] Vector2 settingsScroll;

        // Three lanes, so a thumbnail never holds up the status line or a delete.
        CommandQueue statusLane, listLane, thumbLane;
        SearchField searchField;
        Texture2D dot;
        GUIStyle nameStyle, infoStyle;

        // The connection.
        bool statusAsked;
        double nextStatusAt;
        HarpiaReply status; // null until the first answer
        double statusAt;    // when it came
        string lastState = "";

        // The recordings.
        List<HarpiaClip> clips = new List<HarpiaClip>();
        int total;
        bool listAsked, listLoaded;
        double listAgainAt = -1;
        int pendingAsks;
        string listProblem = "";
        float listHeight = 200f;
        readonly Dictionary<string, Texture2D> thumbs = new Dictionary<string, Texture2D>();
        readonly Dictionary<string, ThumbJob> thumbJobs = new Dictionary<string, ThumbJob>();
        bool thumbAsked, thumbsWanted;

        // What would be recorded, measured once a second while Settings is open.
        string measureText = "";
        MessageType measureType = MessageType.Info;
        double nextMeasureAt;

        sealed class ThumbJob
        {
            public int Tries;
            public double RetryAt;
            public bool Failed;
        }

        [MenuItem("Window/Harpia Recorder")]
        [MenuItem("Tools/Harpia Recorder/Open Window", false, 0)]
        public static void Open()
        {
            var w = GetWindow<HarpiaRecorderWindow>();
            w.titleContent = new GUIContent("Harpia Recorder");
            w.minSize = new Vector2(380, 240);
            w.Show();
        }

        void OnEnable()
        {
            titleContent = new GUIContent("Harpia Recorder");
            wantsMouseMove = true;
            statusLane = new CommandQueue();
            listLane = new CommandQueue();
            thumbLane = new CommandQueue();
            searchField = new SearchField();
            nextStatusAt = 0;
            RefreshList();
        }

        void OnDisable()
        {
            // Also before every script reload: the textures are not kept across it.
            foreach (Texture2D t in thumbs.Values)
                if (t != null)
                    DestroyImmediate(t);
            thumbs.Clear();
            thumbJobs.Clear();
            if (dot != null)
                DestroyImmediate(dot);
        }

        void OnFocus()
        {
            // Back from Harpia or the Explorer: the recordings may have changed there.
            if (listLoaded)
                RefreshList();
        }

        bool Reachable
        {
            get { return status != null && status.Ok; }
        }

        static double Now
        {
            get { return EditorApplication.timeSinceStartup; }
        }

        static string Bin
        {
            get { return Application.platform == RuntimePlatform.OSXEditor ? "Trash" : "recycle bin"; }
        }

        static string RevealLabel
        {
            get { return Application.platform == RuntimePlatform.OSXEditor ? "Show in Finder" : "Show in Explorer"; }
        }

        void OnInspectorUpdate()
        {
            bool answered = statusLane.DrainFinished() + listLane.DrainFinished() + thumbLane.DrainFinished() > 0;
            if (!statusAsked && Now >= nextStatusAt)
                AskStatus();
            if (listAgainAt >= 0 && Now >= listAgainAt && !listAsked)
            {
                listAgainAt = -1;
                RefreshList();
            }
            if (tab == 1 && Now >= nextMeasureAt)
            {
                nextMeasureAt = Now + 1.0;
                Measure();
                answered = true;
            }
            // The clock while recording, and thumbnails that are due another try.
            if (answered || thumbsWanted || (Reachable && status.State == "recording"))
                Repaint();
        }

        // ---- Talking to Harpia ---------------------------------------------------

        void AskStatus()
        {
            statusAsked = true;
            HarpiaClient client = PlayModeRecorder.Client();
            client.TimeoutMs = 1500;
            statusLane.Enqueue(() => client.Status(), r =>
            {
                statusAsked = false;
                bool wasReachable = Reachable;
                status = r;
                statusAt = Now;
                nextStatusAt = Now + (r.Ok ? 1.0 : 3.0);
                string state = r.Ok ? r.State : "";
                // A recording just ended: its file is about to be there (or, for a run too short to keep, not).
                if (lastState.Length > 0 && lastState != "idle" && state == "idle")
                    ScheduleList(1.0);
                if (r.Ok && !wasReachable)
                    ScheduleList(0);
                lastState = state;
            });
        }

        void ScheduleList(double seconds)
        {
            double at = Now + seconds;
            listAgainAt = listAgainAt < 0 ? at : Math.Min(listAgainAt, at);
        }

        void RefreshList()
        {
            if (listAsked)
            {
                ScheduleList(0.5);
                return;
            }
            listAsked = true;
            HarpiaClient client = PlayModeRecorder.Client();
            client.TimeoutMs = 5000;
            listLane.Enqueue(() => client.Recordings(ListLimit), r =>
            {
                listAsked = false;
                if (!r.Ok)
                {
                    // Not running: the status line says so, and the last list stays up.
                    listProblem = r.Unreachable ? ""
                                : r.Status == 404 ? "This Harpia cannot list its recordings: update it to the latest build."
                                : r.Error + ".";
                    return;
                }
                HarpiaRecordingList list = HarpiaRecordingList.From(r);
                listProblem = "";
                SetClips(list.Clips);
                total = list.Total;
                listLoaded = true;
                if (list.Pending && pendingAsks < PendingAsks)
                {
                    pendingAsks++;
                    ScheduleList(1.0);
                }
                else if (!list.Pending)
                {
                    pendingAsks = 0;
                }
            });
        }

        void SetClips(List<HarpiaClip> fresh)
        {
            var keep = new HashSet<string>();
            foreach (HarpiaClip c in fresh)
                keep.Add(c.Key);
            foreach (string key in new List<string>(thumbs.Keys))
            {
                if (keep.Contains(key))
                    continue;
                if (thumbs[key] != null)
                    DestroyImmediate(thumbs[key]);
                thumbs.Remove(key);
            }
            foreach (string key in new List<string>(thumbJobs.Keys))
                if (!keep.Contains(key))
                    thumbJobs.Remove(key);
            clips = fresh;
        }

        // One thumbnail at a time, only for rows on screen.
        void WantThumb(HarpiaClip c)
        {
            if (thumbAsked || !Reachable)
                return;
            ThumbJob job;
            if (!thumbJobs.TryGetValue(c.Key, out job))
                thumbJobs[c.Key] = job = new ThumbJob();
            if (job.Failed || Now < job.RetryAt)
                return;
            thumbAsked = true;
            HarpiaClient client = PlayModeRecorder.Client();
            string key = c.Key, path = c.Path;
            thumbLane.Enqueue(() => client.Thumbnail(path), r =>
            {
                thumbAsked = false;
                if (r.Ok && r.Bytes != null && r.Bytes.Length > 0)
                {
                    var tex = new Texture2D(2, 2, TextureFormat.RGBA32, false);
                    tex.hideFlags = HideFlags.HideAndDontSave;
                    tex.wrapMode = TextureWrapMode.Clamp;
                    if (tex.LoadImage(r.Bytes))
                    {
                        if (thumbs.ContainsKey(key) && thumbs[key] != null)
                            DestroyImmediate(thumbs[key]);
                        thumbs[key] = tex;
                    }
                    else
                    {
                        DestroyImmediate(tex);
                        job.Failed = true;
                    }
                }
                else if (r.Unreachable)
                {
                    job.RetryAt = Now + 3.0;
                }
                else if (r.Pending && ++job.Tries < ThumbTries)
                {
                    job.RetryAt = Now + 0.7;
                }
                else
                {
                    job.Failed = true;
                }
                Repaint();
            });
        }

        // ---- What you can do with a recording ------------------------------------

        static string Native(string path)
        {
            try
            {
                return Path.GetFullPath(path);
            }
            catch (Exception)
            {
                return path;
            }
        }

        bool StillThere(HarpiaClip c)
        {
            if (File.Exists(c.Path))
                return true;
            ShowNotification(new GUIContent("\"" + c.Name + "\" is no longer there"));
            RefreshList();
            return false;
        }

        void Play(HarpiaClip c)
        {
            if (c != null && StillThere(c))
                EditorUtility.OpenWithDefaultApp(Native(c.Path));
        }

        void Reveal(HarpiaClip c)
        {
            if (c != null && StillThere(c))
                EditorUtility.RevealInFinder(Native(c.Path));
        }

        void EditInHarpia(HarpiaClip c)
        {
            if (c == null)
                return;
            LetHarpiaComeForward();
            HarpiaClient client = PlayModeRecorder.Client();
            string name = c.Name, path = c.Path;
            listLane.Enqueue(() => client.Edit(path), r =>
            {
                if (r.Ok)
                {
                    ShowNotification(new GUIContent("Opening \"" + name + "\" in Harpia"));
                    return;
                }
                ShowNotification(new GUIContent(r.Error));
                Debug.LogWarning(Tag + "Edit in Harpia: " + r.Error + ".");
            });
        }

        void DeleteClip(HarpiaClip c)
        {
            if (c == null || !Reachable)
                return;
            // After this OnGUI: a dialog in the middle of a layout pass upsets it.
            EditorApplication.delayCall += () =>
            {
                if (this == null)
                    return; // the window was closed meanwhile
                if (!EditorUtility.DisplayDialog("Delete recording",
                        "Move \"" + c.Name + "\" to the " + Bin + "?", "Delete", "Cancel"))
                    return;
                HarpiaClient client = PlayModeRecorder.Client();
                string path = c.Path, name = c.Name;
                listLane.Enqueue(() => client.Delete(path), r =>
                {
                    if (r.Ok)
                    {
                        // The next one down is selected, so Delete, Delete clears out bad takes.
                        int i = clips.FindIndex(x => x.Path == path);
                        if (i >= 0)
                        {
                            clips.RemoveAt(i);
                            total = Math.Max(0, total - 1);
                            if (selectedPath == path)
                                selectedPath = clips.Count > 0 ? clips[Math.Min(i, clips.Count - 1)].Path : "";
                        }
                        ShowNotification(new GUIContent("\"" + name + "\" moved to the " + Bin));
                    }
                    else
                    {
                        ShowNotification(new GUIContent(r.Error));
                        Debug.LogWarning(Tag + "Delete: " + r.Error + ".");
                    }
                    RefreshList();
                });
            };
        }

        void ShowMenu(HarpiaClip c)
        {
            var menu = new GenericMenu();
            menu.AddItem(new GUIContent("Play"), false, () => Play(c));
            menu.AddItem(new GUIContent(RevealLabel), false, () => Reveal(c));
            if (Reachable && !c.IsGif)
                menu.AddItem(new GUIContent("Edit in Harpia"), false, () => EditInHarpia(c));
            else
                menu.AddDisabledItem(new GUIContent("Edit in Harpia"));
            menu.AddSeparator("");
            menu.AddItem(new GUIContent("Copy Path"), false, () => EditorGUIUtility.systemCopyBuffer = Native(c.Path));
            menu.AddSeparator("");
            if (Reachable)
                menu.AddItem(new GUIContent("Delete..."), false, () => DeleteClip(c));
            else
                menu.AddDisabledItem(new GUIContent("Delete..."));
            menu.ShowAsContext();
        }

#if UNITY_EDITOR_WIN
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool AllowSetForegroundWindow(int processId);
#endif

        // Windows lets a program bring its window forward only with the
        // foreground program's leave; Unity is that program when you click.
        static void LetHarpiaComeForward()
        {
#if UNITY_EDITOR_WIN
            try
            {
                AllowSetForegroundWindow(-1); // ASFW_ANY
            }
            catch (Exception)
            {
            }
#endif
        }

        // ---- Drawing --------------------------------------------------------------

        void OnGUI()
        {
            if (nameStyle == null)
            {
                nameStyle = new GUIStyle(EditorStyles.boldLabel) { clipping = TextClipping.Clip };
                infoStyle = new GUIStyle(EditorStyles.miniLabel) { clipping = TextClipping.Clip };
            }
            if (Event.current.type == EventType.MouseMove)
                Repaint(); // the hover highlight
            DrawStatus();
            DrawToolbar();
            if (tab == 0)
                DrawRecordings();
            else
                DrawSettings();
        }

        static string Clock(double ms)
        {
            TimeSpan t = TimeSpan.FromMilliseconds(Math.Max(0, ms));
            return t.TotalHours >= 1
                ? string.Format("{0}:{1:00}:{2:00}", (int)t.TotalHours, t.Minutes, t.Seconds)
                : string.Format("{0}:{1:00}", t.Minutes, t.Seconds);
        }

        void DrawStatus()
        {
            Color color = new Color(0.55f, 0.55f, 0.55f);
            string line = "Looking for Harpia...";
            string detail = "";
            if (status != null && !status.Ok)
            {
                color = new Color(0.90f, 0.30f, 0.30f);
                line = status.Unreachable ? "Harpia isn't running" : status.Error;
                detail = "Start Harpia Recorder and Editor with Unity control on (Settings > System), port " +
                         HarpiaSettings.Port + ".";
            }
            else if (status != null)
            {
                string app = Json.Str(status.Data, "app");
                string version = Json.Str(status.Data, "version");
                double elapsed = Json.Num(status.Data, "elapsedMs");
                string state = status.State;
                string what;
                if (state == "recording")
                {
                    color = new Color(0.90f, 0.30f, 0.30f);
                    what = "Recording " + Clock(elapsed + (Now - statusAt) * 1000.0);
                }
                else if (state == "paused")
                {
                    color = new Color(0.85f, 0.62f, 0.15f);
                    what = "Paused at " + Clock(elapsed);
                }
                else if (state == "idle")
                {
                    color = new Color(0.30f, 0.75f, 0.40f);
                    what = "Ready";
                }
                else
                {
                    color = new Color(0.85f, 0.62f, 0.15f);
                    what = state == "stopping" ? "Saving..." : state == "starting" ? "Starting..." : state == "countdown" ? "Counting down" : state;
                }
                if (Json.Bool(status.Data, "remote"))
                    what += " (this play session)";
                line = (app.Length > 0 ? app : "Harpia") + (version.Length > 0 ? " " + version : "") + " — " + what;
                string preset = Json.Str(status.Data, "preset");
                detail = (preset.Length > 0 ? "Preset \"" + preset + "\" · " : "") +
                         (HarpiaSettings.Enabled ? "Play Mode is recorded" : "Play Mode recording is off");
            }

            using (new EditorGUILayout.HorizontalScope())
            {
                GUILayout.Space(6);
                using (new EditorGUILayout.VerticalScope(GUILayout.Width(12)))
                {
                    GUILayout.Space(7);
                    Rect r = GUILayoutUtility.GetRect(12, 12, GUILayout.Width(12), GUILayout.Height(12));
                    DrawDot(r, color);
                }
                using (new EditorGUILayout.VerticalScope())
                {
                    GUILayout.Space(3);
                    GUILayout.Label(line, nameStyle);
                    GUILayout.Label(detail, infoStyle);
                    GUILayout.Space(2);
                }
            }
        }

        void DrawToolbar()
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                tab = GUILayout.Toolbar(tab, Tabs, EditorStyles.toolbarButton, GUILayout.Width(180));
                GUILayout.FlexibleSpace();
                bool on = GUILayout.Toggle(HarpiaSettings.Enabled,
                    new GUIContent("Record Play Mode", "Start a Harpia recording every time you enter Play Mode."),
                    EditorStyles.toolbarButton);
                if (on != HarpiaSettings.Enabled)
                    HarpiaSettings.Enabled = on;
            }
        }

        void Measure()
        {
            GameViewMeasure m;
            string why;
            if (GameViewArea.TryMeasure(HarpiaSettings.Area, out m, out why))
            {
                measureText = m.Describe() + "." + PlayModeRecorder.SizeHint(m);
                measureType = m.PixelForPixel || m.Shown >= 1f || HarpiaSettings.Area != RecordArea.GamePicture
                    ? MessageType.Info
                    : MessageType.Warning;
            }
            else
            {
                measureText = "Cannot measure the Game view: " + why + ".";
                measureType = MessageType.Warning;
            }
        }

        void DrawSettings()
        {
            settingsScroll = EditorGUILayout.BeginScrollView(settingsScroll);
            GUILayout.Space(6);
            HarpiaSettingsProvider.Draw(false);
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("What gets recorded (live)", EditorStyles.boldLabel);
            if (measureText.Length == 0)
                Measure();
            EditorGUILayout.HelpBox(measureText, measureType);
            EditorGUILayout.Space();
            EditorGUILayout.LabelField(
                "In Harpia: Settings > System > Unity must be on, with the same port. Video and sound " +
                "(quality, frame rate, codec, audio, output folder) come from Harpia's current preset.",
                EditorStyles.wordWrappedMiniLabel);
            EditorGUILayout.EndScrollView();
        }

        List<HarpiaClip> Filtered()
        {
            string s = (search ?? "").Trim();
            if (s.Length == 0)
                return clips;
            return clips.FindAll(c => c.Name.IndexOf(s, StringComparison.OrdinalIgnoreCase) >= 0 ||
                                      c.Preset.IndexOf(s, StringComparison.OrdinalIgnoreCase) >= 0);
        }

        HarpiaClip Selected(List<HarpiaClip> shown)
        {
            return shown.Find(c => c.Path == selectedPath);
        }

        void DrawRecordings()
        {
            List<HarpiaClip> shown = Filtered();
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                search = searchField.OnToolbarGUI(search, GUILayout.MinWidth(80));
                string count = shown.Count != clips.Count ? shown.Count + " of " + clips.Count
                             : total > clips.Count ? "newest " + clips.Count + " of " + total
                             : clips.Count + (clips.Count == 1 ? " recording" : " recordings");
                GUILayout.Label(count, EditorStyles.miniLabel, GUILayout.ExpandWidth(false));
                if (GUILayout.Button(new GUIContent("Refresh", "Ask Harpia for the list again."),
                        EditorStyles.toolbarButton, GUILayout.Width(56)))
                {
                    pendingAsks = 0;
                    RefreshList();
                }
            }
            if (listProblem.Length > 0)
                EditorGUILayout.HelpBox(listProblem, MessageType.Warning);

            HandleKeys(shown);
            Rect area = GUILayoutUtility.GetRect(GUIContent.none, GUIStyle.none,
                GUILayout.ExpandWidth(true), GUILayout.ExpandHeight(true));
            if (Event.current.type == EventType.Repaint)
                listHeight = area.height;
            if (shown.Count > 0)
            {
                DrawList(area, shown);
            }
            else
            {
                string why = !listLoaded && !Reachable && status != null ? "Start Harpia to see its recordings."
                           : !listLoaded ? "Loading..."
                           : clips.Count == 0 ? "No recordings yet.\nEnter Play Mode to make one."
                           : "No recording matches \"" + search.Trim() + "\".";
                GUI.Label(area, why, EditorStyles.centeredGreyMiniLabel);
                thumbsWanted = false;
            }
            DrawActions(Selected(shown));
        }

        void HandleKeys(List<HarpiaClip> shown)
        {
            Event e = Event.current;
            if (e.type != EventType.KeyDown || GUIUtility.keyboardControl != 0)
                return; // typing in the search field
            HarpiaClip sel = Selected(shown);
            if ((e.keyCode == KeyCode.Delete || (e.keyCode == KeyCode.Backspace && e.command)) && sel != null)
            {
                e.Use();
                DeleteClip(sel);
            }
            else if ((e.keyCode == KeyCode.Return || e.keyCode == KeyCode.KeypadEnter) && sel != null)
            {
                e.Use();
                Play(sel);
            }
            else if ((e.keyCode == KeyCode.DownArrow || e.keyCode == KeyCode.UpArrow) && shown.Count > 0)
            {
                int index = sel == null ? -1 : shown.IndexOf(sel);
                int next = index < 0 ? 0 : Mathf.Clamp(index + (e.keyCode == KeyCode.DownArrow ? 1 : -1), 0, shown.Count - 1);
                selectedPath = shown[next].Path;
                float top = next * RowHeight;
                if (top < scroll.y)
                    scroll.y = top;
                else if (top + RowHeight > scroll.y + listHeight)
                    scroll.y = top + RowHeight - listHeight;
                e.Use();
                Repaint();
            }
        }

        void DrawList(Rect area, List<HarpiaClip> shown)
        {
            float height = shown.Count * RowHeight;
            var content = new Rect(0, 0, area.width - (height > area.height ? 14f : 0f), height);
            scroll = GUI.BeginScrollView(area, scroll, content);
            // Only the rows on screen: drawn, and asked for their thumbnails.
            int first = Mathf.Max(0, (int)(scroll.y / RowHeight));
            int last = Mathf.Min(shown.Count - 1, (int)((scroll.y + area.height) / RowHeight));
            bool wanted = false;
            for (int i = first; i <= last; i++)
                DrawRow(new Rect(0, i * RowHeight, content.width, RowHeight), shown[i], ref wanted);
            GUI.EndScrollView();
            if (Event.current.type == EventType.Repaint)
                thumbsWanted = wanted;
        }

        void DrawRow(Rect row, HarpiaClip c, ref bool wanted)
        {
            Event e = Event.current;
            bool selected = c.Path == selectedPath;
            if (e.type == EventType.Repaint)
            {
                if (selected)
                    EditorGUI.DrawRect(row, EditorGUIUtility.isProSkin ? new Color(0.17f, 0.36f, 0.53f) : new Color(0.23f, 0.45f, 0.80f, 0.35f));
                else if (row.Contains(e.mousePosition))
                    EditorGUI.DrawRect(row, EditorGUIUtility.isProSkin ? new Color(1f, 1f, 1f, 0.05f) : new Color(0f, 0f, 0f, 0.06f));
                EditorGUI.DrawRect(new Rect(row.x, row.yMax - 1, row.width, 1), new Color(0f, 0f, 0f, 0.18f));
            }

            float thumbHeight = ThumbWidth * 9f / 16f;
            var thumb = new Rect(row.x + 6, row.y + (RowHeight - thumbHeight) / 2f, ThumbWidth, thumbHeight);
            DrawThumb(thumb, c, ref wanted);

            float x = thumb.xMax + 8;
            float w = Mathf.Max(10f, row.xMax - x - 6);
            GUI.Label(new Rect(x, row.y + 6, w, 18), c.Name, nameStyle);
            string kind = c.IsAudio ? " · audio" : c.IsGif ? " · GIF" : "";
            GUI.Label(new Rect(x, row.y + 24, w, 16), (c.Duration.Length > 0 ? c.Duration : "--:--") + " · " + c.Size + kind, infoStyle);
            GUI.Label(new Rect(x, row.y + 39, w, 16), c.Age + (c.Preset.Length > 0 ? " · " + c.Preset : ""), infoStyle);

            if (!row.Contains(e.mousePosition))
                return;
            if (e.type == EventType.MouseDown)
            {
                selectedPath = c.Path;
                GUIUtility.keyboardControl = 0; // Delete and the arrows now mean the list, not the search field
                if (e.button == 0)
                {
                    if (e.clickCount == 2)
                        Play(c);
                    e.Use();
                }
                Repaint();
            }
            else if (e.type == EventType.ContextClick)
            {
                selectedPath = c.Path;
                ShowMenu(c);
                e.Use();
            }
        }

        void DrawThumb(Rect r, HarpiaClip c, ref bool wanted)
        {
            if (Event.current.type != EventType.Repaint)
                return;
            EditorGUI.DrawRect(r, new Color(0.11f, 0.11f, 0.11f));
            Texture2D tex;
            if (thumbs.TryGetValue(c.Key, out tex) && tex != null)
            {
                GUI.DrawTexture(r, tex, ScaleMode.ScaleToFit);
                return;
            }
            string label = "audio";
            if (!c.IsAudio)
            {
                ThumbJob job;
                bool failed = thumbJobs.TryGetValue(c.Key, out job) && job.Failed;
                label = failed ? "no preview" : "...";
                if (!failed)
                {
                    wanted = true;
                    WantThumb(c);
                }
            }
            GUI.Label(r, label, EditorStyles.centeredGreyMiniLabel);
        }

        void DrawActions(HarpiaClip c)
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                using (new EditorGUI.DisabledScope(c == null))
                {
                    if (GUILayout.Button(new GUIContent("Play", "Open it in your video player (double-click, Enter)."), EditorStyles.toolbarButton))
                        Play(c);
                    if (GUILayout.Button(RevealLabel, EditorStyles.toolbarButton))
                        Reveal(c);
                }
                using (new EditorGUI.DisabledScope(c == null || c.IsGif || !Reachable))
                {
                    if (GUILayout.Button(new GUIContent("Edit in Harpia", "Open it in Harpia's editor (trim, crop, export)."), EditorStyles.toolbarButton))
                        EditInHarpia(c);
                }
                GUILayout.FlexibleSpace();
                using (new EditorGUI.DisabledScope(c == null || !Reachable))
                {
                    if (GUILayout.Button(new GUIContent("Delete", "Move it to the " + Bin + " (Delete key)."), EditorStyles.toolbarButton))
                        DeleteClip(c);
                }
            }
        }

        void DrawDot(Rect r, Color color)
        {
            if (Event.current.type != EventType.Repaint)
                return;
            if (dot == null)
                dot = MakeDot();
            Color old = GUI.color;
            GUI.color = color;
            GUI.DrawTexture(r, dot);
            GUI.color = old;
        }

        static Texture2D MakeDot()
        {
            const int n = 24;
            var t = new Texture2D(n, n, TextureFormat.RGBA32, false);
            t.hideFlags = HideFlags.HideAndDontSave;
            t.filterMode = FilterMode.Bilinear;
            var px = new Color32[n * n];
            for (int y = 0; y < n; y++)
            {
                for (int x = 0; x < n; x++)
                {
                    float dx = x + 0.5f - n / 2f, dy = y + 0.5f - n / 2f;
                    float a = Mathf.Clamp01(n / 2f - Mathf.Sqrt(dx * dx + dy * dy));
                    px[y * n + x] = new Color32(255, 255, 255, (byte)(a * 255));
                }
            }
            t.SetPixels32(px);
            t.Apply();
            return t;
        }
    }
}
