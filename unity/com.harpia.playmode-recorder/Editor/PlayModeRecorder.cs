// Records Play Mode with Harpia Recorder and Editor.
//
//   Enter Play Mode   -> Harpia starts recording the Game view (its current preset's settings)
//   Pause / Unpause   -> the recording pauses / resumes
//   Leave Play Mode   -> the recording stops and is saved; runs shorter than the
//                        minimum in Preferences are deleted instead
//
// If Harpia isn't running, a warning says so and Play Mode goes on as normal.
// A recording you started by hand in Harpia is never touched.

using UnityEditor;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Harpia.PlayModeRecorder
{
    [InitializeOnLoad]
    public static class PlayModeRecorder
    {
        const string Tag = "[Harpia] ";
        // Survives the script reload a recompile during Play Mode causes, so the
        // Stop is still sent when Play Mode ends.
        const string StartSentKey = "Harpia.PlayModeRecorder.StartSent";

        static readonly CommandQueue Queue = new CommandQueue();
        static bool startPending;
        static double startAt;

        static bool StartSent
        {
            get { return SessionState.GetBool(StartSentKey, false); }
            set { SessionState.SetBool(StartSentKey, value); }
        }

        static PlayModeRecorder()
        {
            EditorApplication.playModeStateChanged += OnPlayModeChanged;
            EditorApplication.pauseStateChanged += OnPauseChanged;
            EditorApplication.update += OnUpdate;
            EditorApplication.quitting += SendStopNow;
        }

        public static HarpiaClient Client()
        {
            return new HarpiaClient(HarpiaSettings.Port, "Unity " + Application.unityVersion + " (" + Application.productName + ")");
        }

        static void OnPlayModeChanged(PlayModeStateChange change)
        {
            switch (change)
            {
                case PlayModeStateChange.EnteredPlayMode:
                    if (!HarpiaSettings.Enabled)
                        return;
                    startPending = true;
                    startAt = EditorApplication.timeSinceStartup + HarpiaSettings.StartDelayMs / 1000.0;
                    break;
                case PlayModeStateChange.ExitingPlayMode:
                    startPending = false;
                    SendStopNow();
                    break;
            }
        }

        static void OnPauseChanged(PauseState state)
        {
            if (!StartSent)
                return; // not recording (yet): a start checks the pause itself
            HarpiaClient client = Client();
            if (state == PauseState.Paused)
                Queue.Enqueue(() => client.Pause(), r => Report("Pause", r));
            else
                Queue.Enqueue(() => client.Resume(), r => Report("Resume", r));
        }

        static void OnUpdate()
        {
            Queue.DrainFinished();
            if (startPending && EditorApplication.timeSinceStartup >= startAt)
            {
                startPending = false;
                StartRecording();
            }
        }

        static void StartRecording()
        {
            if (!EditorApplication.isPlaying)
                return;
            PixelRect area;
            string why;
            if (!GameViewArea.TryGet(HarpiaSettings.Area, out area, out why))
            {
                Debug.LogWarning(Tag + "Not recording this play session: " + why + ".");
                return;
            }
            HarpiaClient client = Client();
            string label = SceneManager.GetActiveScene().name;
            StartSent = true;
            Queue.Enqueue(() => client.Start(area.X, area.Y, area.Width, area.Height, label), r =>
            {
                if (!r.Ok)
                {
                    StartSent = false;
                    Debug.LogWarning(Tag + "Not recording this play session: " + r.Error +
                                     (r.Unreachable
                                         ? ". Start Harpia Recorder and Editor, with Unity control on (Settings > System)."
                                         : "."));
                    return;
                }
                if (HarpiaSettings.LogEachRecording)
                    Debug.Log(Tag + "Recording the Game view, " + area + ".");
                // Paused before the start got there ("Pause on Play", or a quick click).
                if (EditorApplication.isPlaying && EditorApplication.isPaused)
                    Queue.Enqueue(() => client.Pause(), p => Report("Pause", p));
            });
        }

        // Leaving Play Mode (or quitting): the Stop has to be out before Unity
        // reloads the scripts, so this waits for it -- briefly.
        static void SendStopNow()
        {
            if (!StartSent)
                return;
            StartSent = false;
            HarpiaClient client = Client();
            float minSeconds = HarpiaSettings.MinSeconds;
            int minMs = Mathf.RoundToInt(minSeconds * 1000f);
            Queue.Enqueue(() => client.Stop(minMs), r =>
            {
                if (r.Ok && HarpiaSettings.LogEachRecording)
                    Debug.Log(Tag + "Recording stopped and saved by Harpia" +
                              (minMs > 0 ? " (runs under " + minSeconds.ToString("0.#") + " s are not kept)." : "."));
                else if (!r.Ok)
                    Report("Stop", r);
            });
            if (!Queue.WaitIdle(4000))
                Debug.LogWarning(Tag + "Harpia did not confirm the stop in time; check whether it is still recording.");
            Queue.DrainFinished();
        }

        static void Report(string what, HarpiaReply r)
        {
            if (r.Ok)
                return;
            // Stopped (or started) by hand in Harpia meanwhile: nothing to worry about.
            if (r.Status == 409)
            {
                if (HarpiaSettings.LogEachRecording)
                    Debug.Log(Tag + what + ": " + r.Error + ".");
                return;
            }
            Debug.LogWarning(Tag + what + " failed: " + r.Error + ".");
        }
    }
}
