// Runs Harpia commands one at a time, in order, off Unity's main thread, and
// hands each answer back to the main thread.
//
// In order matters: Pause pressed right after Play must reach Harpia after the
// Start, not overtake it on another connection. Off the main thread matters:
// starting a recording takes Harpia a moment, and the editor should not stall
// for it. WaitIdle is the one place that does block -- leaving Play Mode, where
// the Stop has to be out before Unity reloads the scripts.
//
// Plain .NET, no UnityEngine (tested outside Unity, see Tests~).

using System;
using System.Collections.Generic;
using System.Threading;

namespace Harpia.PlayModeRecorder
{
    public sealed class CommandQueue
    {
        struct Job
        {
            public Func<HarpiaReply> Run;
            public Action<HarpiaReply> Done;
        }

        readonly object gate = new object();
        readonly Queue<Job> jobs = new Queue<Job>();
        readonly Queue<KeyValuePair<Action<HarpiaReply>, HarpiaReply>> finished =
            new Queue<KeyValuePair<Action<HarpiaReply>, HarpiaReply>>();
        bool running;

        // `done` runs later, on whichever thread calls DrainFinished (the main thread).
        public void Enqueue(Func<HarpiaReply> run, Action<HarpiaReply> done)
        {
            lock (gate)
            {
                jobs.Enqueue(new Job { Run = run, Done = done });
                if (running)
                    return;
                running = true;
            }
            ThreadPool.QueueUserWorkItem(_ => Pump());
        }

        void Pump()
        {
            while (true)
            {
                Job job;
                lock (gate)
                {
                    if (jobs.Count == 0)
                    {
                        running = false;
                        Monitor.PulseAll(gate);
                        return;
                    }
                    job = jobs.Dequeue();
                }
                HarpiaReply reply;
                try
                {
                    reply = job.Run();
                }
                catch (Exception e)
                {
                    reply = new HarpiaReply { Error = "Could not talk to Harpia: " + e.Message };
                }
                lock (gate)
                {
                    if (job.Done != null)
                        finished.Enqueue(new KeyValuePair<Action<HarpiaReply>, HarpiaReply>(job.Done, reply));
                }
            }
        }

        // Blocks until every queued command has been sent and answered, or the
        // time is up. True when idle.
        public bool WaitIdle(int timeoutMs)
        {
            DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            lock (gate)
            {
                while (running || jobs.Count > 0)
                {
                    int left = (int)(until - DateTime.UtcNow).TotalMilliseconds;
                    if (left <= 0)
                        return false;
                    Monitor.Wait(gate, left);
                }
                return true;
            }
        }

        // Runs the answers' callbacks that are ready. Call from the main thread.
        public int DrainFinished()
        {
            var ready = new List<KeyValuePair<Action<HarpiaReply>, HarpiaReply>>();
            lock (gate)
            {
                while (finished.Count > 0)
                    ready.Add(finished.Dequeue());
            }
            foreach (var r in ready)
                r.Key(r.Value);
            return ready.Count;
        }
    }
}
