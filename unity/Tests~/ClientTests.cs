// The Unity scripts' Unity-free core (HarpiaClient, HarpiaClip, Json,
// CommandQueue), run under Mono against the REAL Harpia server code:
// harpia/tests/remotehost_tool, which wraps core/RemoteControl,
// core/RemoteSession and core/RemoteClips with a pretend recorder. Unity
// ignores this folder (the "~"), even if it is copied into a project.
//
//   cd unity
//   mcs -out:ClientTests.exe Tests~/ClientTests.cs HarpiaRecorder/Editor/HarpiaClient.cs \
//       HarpiaRecorder/Editor/HarpiaClip.cs HarpiaRecorder/Editor/Json.cs HarpiaRecorder/Editor/CommandQueue.cs
//   mono ClientTests.exe <path to remotehost_tool> <free port>
//
// What a Play Mode session sends, in order, and what Harpia makes of it:
// start (with the area, client and scene), pause, resume, stop (with the
// minimum length); repeats that are harmless; a start refused while one is
// running; "not running" when nothing listens; JSON that survives quotes.
// Then the recordings window: the list (durations arriving on the second
// ask), a thumbnail (pending, then PNG bytes), edit, and delete -- which
// removes a listed recording and refuses any other file.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;
using Harpia.PlayModeRecorder;

static class ClientTests
{
    static int failures;

    static void Ok(bool c, string what)
    {
        Console.WriteLine("  " + (c ? "PASS " : "FAIL ") + what);
        if (!c)
            failures++;
    }

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.WriteLine("usage: ClientTests.exe <remotehost_tool> <port>");
            return 2;
        }
        int port = int.Parse(args[1]);

        Console.WriteLine("\n-- JSON --");
        string tricky = "Say \"hi\"\\ é\n";
        string quoted = Json.Quote(tricky);
        Ok(Json.GetString("{\"label\":" + quoted + "}", "label") == tricky, "a string with quotes, backslash, accents and a newline survives");
        bool b;
        Ok(Json.TryGetBool("{\"ok\": true}", "ok", out b) && b, "reads a bool");
        Ok(!Json.TryGetBool("{\"nope\":true}", "ok", out b), "a missing key is missing");
        Ok(Json.GetString("{\"error\":\"line\\u0021\"}", "error") == "line!", "\\u escapes");
        var parsed = Json.ParseObject("{\"a\":[1,2.5,-3e2,{\"b\":null}],\"c\":{\"d\":\"\\u00e9\"},\"e\":false}");
        List<object> arr = Json.List(parsed, "a");
        Ok(arr.Count == 4 && (double)arr[0] == 1 && (double)arr[1] == 2.5 && (double)arr[2] == -300 &&
           ((Dictionary<string, object>)arr[3])["b"] == null, "lists, numbers, null");
        Ok(Json.Str((Dictionary<string, object>)parsed["c"], "d") == "\u00e9" && parsed["e"] is bool && !(bool)parsed["e"],
           "nested objects and false");
        Ok(Json.GetString("{\"x\":{\"state\":\"inner\"},\"state\":\"outer\"}", "state") == "outer",
           "a key is read at the top, not from inside another object");
        Ok(Json.ParseObject("{\"a\":").Count == 0 && Json.ParseObject("not json").Count == 0 && Json.ParseObject("[1]").Count == 0,
           "broken or non-object JSON reads as an empty object");
        bool threw = false;
        try { Json.Parse("{\"a\":1} trailing"); } catch (FormatException) { threw = true; }
        Ok(threw, "text after the value is an error");

        Console.WriteLine("\n-- nothing listening --");
        var lonely = new HarpiaClient(port, "Unity test", 1000);
        HarpiaReply r = lonely.Status();
        Ok(!r.Ok && r.Unreachable, "Harpia not running: unreachable");
        Ok(r.Error.Contains("isn't running") && r.Error.Contains(port.ToString()), "and the message says so, with the port: " + r.Error);

        // The recordings: a folder of pretend files, oldest to newest.
        string folder = Path.Combine(Path.GetTempPath(), "harpia-client-test-" + Process.GetCurrentProcess().Id);
        Directory.CreateDirectory(folder);
        string[] names = { "Arena #1 (night).mp4", "Boss fight.mkv", "voice.m4a", "notes.txt" };
        for (int i = 0; i < names.Length; i++)
        {
            string file = Path.Combine(folder, names[i]);
            File.WriteAllText(file, new string('x', 100 * (i + 1)));
            File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddMinutes(-10 + i));
        }

        Process host = Process.Start(new ProcessStartInfo(args[0], port + " 30 \"" + folder + "\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
        });
        string first = host.StandardOutput.ReadLine();
        Ok(first != null && first.StartsWith("listening"), "the test host is up");
        try
        {
            var client = new HarpiaClient(port, "Unity 2022.3.10f1 (Demo)");

            Console.WriteLine("\n-- a Play Mode session --");
            r = client.Status();
            Ok(r.Ok && r.State == "idle", "status: idle");
            Ok(Json.GetString(r.Body, "preset") == "Test \"preset\"", "the preset name comes through, quotes and all");

            r = client.Start(1920 + 64, 120, 1280, 720, "Arena \"Night\"");
            Ok(r.Ok && r.State == "recording", "enter Play Mode: start -> recording");
            string status = client.Status().Body;
            Ok(status.Contains("\"x\":1984") && status.Contains("\"width\":1280") && status.Contains("\"height\":720"),
               "the area arrived as sent");
            Ok(Json.GetString(status, "lastClient") == "Unity 2022.3.10f1 (Demo)" &&
               Json.GetString(status, "lastLabel") == "Arena \"Night\"", "with the client and the scene name");

            r = client.Start(0, 0, 640, 360, "again");
            Ok(r.Ok && r.Note == "already recording", "a second start is harmless");

            r = client.Pause();
            Ok(r.Ok && r.State == "paused", "Pause -> paused");
            r = client.Pause();
            Ok(r.Ok && r.Note == "already paused", "Pause again: harmless");
            r = client.Resume();
            Ok(r.Ok && r.State == "recording", "Unpause -> recording");

            r = client.Stop(3000);
            Ok(r.Ok, "leave Play Mode: stop");
            status = client.Status().Body;
            Ok(status.Contains("\"lastDiscardMs\":3000"), "with the minimum length");
            Ok(client.Status().State == "idle", "and Harpia is idle again");

            r = client.Stop(3000);
            Ok(!r.Ok && r.Status == 409, "a stop with nothing of ours running: 409 (" + r.Error + ")");
            r = client.Pause();
            Ok(!r.Ok && r.Status == 409, "a pause likewise");

            r = client.ShowArea(10, 20, 300, 200);
            Ok(r.Ok && client.Status().Body.Contains("\"width\":300"), "show area");

            Console.WriteLine("\n-- in order, off the main thread --");
            var queue = new CommandQueue();
            var seen = new System.Collections.Generic.List<string>();
            queue.Enqueue(() => client.Start(0, 0, 800, 450, "Q"), x => seen.Add("start:" + x.State));
            queue.Enqueue(() => client.Pause(), x => seen.Add("pause:" + x.State));
            queue.Enqueue(() => client.Resume(), x => seen.Add("resume:" + x.State));
            queue.Enqueue(() => client.Stop(0), x => seen.Add("stop:" + (x.Ok ? "ok" : x.Error)));
            Ok(queue.WaitIdle(5000), "four commands queued at once all finish");
            Ok(seen.Count == 0, "callbacks wait for the main thread");
            Ok(queue.DrainFinished() == 4, "and run when it drains them");
            Ok(string.Join(",", seen) == "start:recording,pause:paused,resume:recording,stop:ok",
               "in the order they were sent: " + string.Join(",", seen));
            Ok(queue.WaitIdle(10), "an empty queue is idle at once");

            Console.WriteLine("\n-- the recordings --");
            r = client.Recordings(100);
            HarpiaRecordingList list = HarpiaRecordingList.From(r);
            Ok(r.Ok && list.Clips.Count == 3 && list.Total == 3, "three recordings (the .txt is not one)");
            Ok(list.Clips.Count == 3 && list.Clips[0].Name == "voice.m4a" && list.Clips[2].Name == "Arena #1 (night).mp4",
               "newest first");
            Ok(list.Pending && list.Clips.TrueForAll(c => c.DurationMs == 0), "the first ask: durations still being read");
            list = HarpiaRecordingList.From(client.Recordings(100));
            Ok(!list.Pending && list.Clips.TrueForAll(c => c.DurationMs == 4200 && c.Duration.Length > 0),
               "the next: durations in");
            HarpiaClip arena = list.Clips.Find(c => c.Name.StartsWith("Arena"));
            HarpiaClip boss = list.Clips.Find(c => c.Name.StartsWith("Boss"));
            HarpiaClip voice = list.Clips.Find(c => c.Name == "voice.m4a");
            Ok(arena != null && File.Exists(arena.Path) && arena.Preset == "Test preset" && arena.SizeBytes == 100 &&
               arena.Size.Length > 0 && arena.Age.Length > 0 && arena.Kind == "video",
               "a clip: its path (a real file), preset, size, age, kind");
            Ok(arena != null && Math.Abs((arena.ModifiedUtc - DateTime.UtcNow.AddMinutes(-10)).TotalSeconds) < 30,
               "and when it was made");
            Ok(voice != null && voice.IsAudio, "an audio recording says so");
            Ok(HarpiaRecordingList.From(client.Recordings(1)).Clips.Count == 1 &&
               HarpiaRecordingList.From(client.Recordings(1)).Total == 3, "a limit: the newest only, and the total");

            r = client.Thumbnail(arena.Path);
            Ok(!r.Ok && r.Pending && r.Status == 202, "a thumbnail not made yet: pending");
            r = client.Thumbnail(arena.Path);
            Ok(r.Ok && r.ContentType == "image/png" && r.Bytes != null && r.Bytes.Length > 8 &&
               r.Bytes[0] == 0x89 && r.Bytes[1] == (byte)'P' && r.Bytes[2] == (byte)'N' && r.Bytes[3] == (byte)'G',
               "asked again: PNG bytes (a path with spaces, # and brackets)");
            r = client.Thumbnail(voice.Path);
            Ok(!r.Ok && r.Status == 404, "audio has no thumbnail");
            r = client.Thumbnail(Path.Combine(folder, "notes.txt"));
            Ok(!r.Ok && r.Status == 404, "a file that is not a recording: 404");

            r = client.Edit(boss.Path);
            Ok(r.Ok && Json.GetString(client.Status().Body, "lastEdit") == boss.Path, "edit: Harpia opens it");
            r = client.Edit(Path.Combine(folder, "notes.txt"));
            Ok(!r.Ok && r.Status == 404, "edit something else: 404");

            string stranger = Path.Combine(folder, "notes.txt");
            r = client.Delete(stranger);
            Ok(!r.Ok && r.Status == 404 && File.Exists(stranger), "delete a file Harpia did not list: refused, still there");
            r = client.Delete(arena.Path, stranger);
            Ok(!r.Ok && r.Status == 409 && !File.Exists(arena.Path) && File.Exists(stranger),
               "a recording and a stranger: the recording goes, the stranger stays");
            Ok(Json.List(r.Data, "deleted").Count == 1 && Json.List(r.Data, "failed").Count == 1, "the answer says which");
            r = client.Delete(boss.Path);
            Ok(r.Ok && !File.Exists(boss.Path) && Json.List(r.Data, "deleted").Count == 1, "delete one: gone, ok");
            list = HarpiaRecordingList.From(client.Recordings(100));
            Ok(list.Clips.Count == 1 && list.Clips[0].Name == "voice.m4a", "and the list no longer has them");
        }
        finally
        {
            try { host.Kill(); } catch (Exception) { }
            try { Directory.Delete(folder, true); } catch (Exception) { }
        }

        Console.WriteLine("\n" + (failures > 0 ? "FAILURES" : "ALL PASSED (0 failures)"));
        return failures > 0 ? 1 : 0;
    }
}
