// The package's Unity-free core (HarpiaClient, Json, CommandQueue), run under
// Mono against the REAL Harpia server code: harpia/tests/remotehost_tool,
// which wraps core/RemoteControl and core/RemoteSession with a pretend
// recorder. Unity ignores this folder (the "~").
//
//   mcs -out:ClientTests.exe Tests~/ClientTests.cs Editor/HarpiaClient.cs Editor/CommandQueue.cs
//   mono ClientTests.exe <path to remotehost_tool> <free port>
//
// What a Play Mode session sends, in order, and what Harpia makes of it:
// start (with the area, client and scene), pause, resume, stop (with the
// minimum length); repeats that are harmless; a start refused while one is
// running; "not running" when nothing listens; JSON that survives quotes.

using System;
using System.Diagnostics;
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

        Console.WriteLine("\n-- nothing listening --");
        var lonely = new HarpiaClient(port, "Unity test", 1000);
        HarpiaReply r = lonely.Status();
        Ok(!r.Ok && r.Unreachable, "Harpia not running: unreachable");
        Ok(r.Error.Contains("isn't running") && r.Error.Contains(port.ToString()), "and the message says so, with the port: " + r.Error);

        Process host = Process.Start(new ProcessStartInfo(args[0], port + " 30")
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
        }
        finally
        {
            try { host.Kill(); } catch (Exception) { }
        }

        Console.WriteLine("\n" + (failures > 0 ? "FAILURES" : "ALL PASSED (0 failures)"));
        return failures > 0 ? 1 : 0;
    }
}
