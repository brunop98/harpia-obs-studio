// Talks to Harpia Recorder and Editor's local remote control (127.0.0.1).
//
// Plain .NET, no UnityEngine: the same file is compiled and run against the
// real Harpia server by unity/Tests~ (in the Harpia repository), outside Unity.
//
// One HTTP request per command, over a raw socket with hard timeouts, written
// and read synchronously. No HttpClient: its async plumbing and Unity's main
// thread do not mix well when a command has to be sent RIGHT NOW -- leaving
// Play Mode reloads the scripts a moment later, and an in-flight request would
// die with them.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Text;

namespace Harpia.PlayModeRecorder
{
    public sealed class HarpiaReply
    {
        public bool Ok;
        public int Status;          // HTTP status; 0 when Harpia could not be reached
        public bool Unreachable;    // nothing listening (Harpia not running, or the port differs)
        public string Error = "";   // Harpia's reason, or what went wrong on the way
        public string State = "";   // "idle", "recording", "paused", "stopping", ...
        public string Note = "";    // e.g. "already recording"
        public bool Pending;        // "ask again shortly" (a thumbnail being made, durations being read)
        public string Body = "";    // the raw JSON
        public Dictionary<string, object> Data = new Dictionary<string, object>(); // Body, parsed
        public string ContentType = "";
        public byte[] Bytes;        // a body that is not JSON (a thumbnail's PNG)

        public override string ToString()
        {
            if (Ok)
                return "ok" + (State.Length > 0 ? " (" + State + ")" : "") + (Note.Length > 0 ? ": " + Note : "");
            return Error;
        }
    }

    public sealed class HarpiaClient
    {
        public readonly int Port;
        public readonly string ClientName;
        public int TimeoutMs;

        public HarpiaClient(int port, string clientName, int timeoutMs = 2500)
        {
            Port = port;
            ClientName = string.IsNullOrEmpty(clientName) ? "Unity" : clientName;
            TimeoutMs = timeoutMs;
        }

        public HarpiaReply Status()
        {
            return Send("GET", "/status", null);
        }

        // The area is in physical pixels of the whole desktop.
        public HarpiaReply Start(int x, int y, int width, int height, string label)
        {
            var json = new StringBuilder("{");
            AppendArea(json, x, y, width, height);
            json.Append(",\"client\":").Append(Json.Quote(ClientName));
            json.Append(",\"label\":").Append(Json.Quote(label ?? ""));
            json.Append('}');
            return Send("POST", "/record/start", json.ToString());
        }

        public HarpiaReply Pause()
        {
            return Send("POST", "/record/pause", "{}");
        }

        public HarpiaReply Resume()
        {
            return Send("POST", "/record/resume", "{}");
        }

        // Runs shorter than this (paused time not counted) are deleted, not saved.
        public HarpiaReply Stop(int discardShorterThanMs)
        {
            return Send("POST", "/record/stop",
                "{\"discardIfShorterThanMs\":" + Math.Max(0, discardShorterThanMs).ToString(CultureInfo.InvariantCulture) + "}");
        }

        // Harpia outlines the area on screen for a moment.
        public HarpiaReply ShowArea(int x, int y, int width, int height)
        {
            var json = new StringBuilder("{");
            AppendArea(json, x, y, width, height);
            json.Append('}');
            return Send("POST", "/area/show", json.ToString());
        }

        // Harpia's recordings, newest first (read them with HarpiaRecordingList.From).
        public HarpiaReply Recordings(int limit)
        {
            return Send("GET", "/recordings?limit=" + Math.Max(1, limit).ToString(CultureInfo.InvariantCulture), null);
        }

        // A recording's thumbnail as PNG bytes (reply.Bytes). Pending while
        // Harpia is still making it: ask again in a moment.
        public HarpiaReply Thumbnail(string path)
        {
            return Send("GET", "/recordings/thumbnail?path=" + Uri.EscapeDataString(path ?? ""), null);
        }

        // To the recycle bin. Only recordings Harpia listed; the reply's Data
        // has "deleted" and "failed" (each {"path","error"}).
        public HarpiaReply Delete(params string[] paths)
        {
            var json = new StringBuilder("{\"paths\":[");
            for (int i = 0; i < paths.Length; i++)
                json.Append(i > 0 ? "," : "").Append(Json.Quote(paths[i]));
            json.Append("]}");
            return Send("POST", "/recordings/delete", json.ToString());
        }

        // Opens the recording in Harpia's editor.
        public HarpiaReply Edit(string path)
        {
            return Send("POST", "/recordings/edit", "{\"path\":" + Json.Quote(path) + "}");
        }

        static void AppendArea(StringBuilder json, int x, int y, int width, int height)
        {
            json.Append("\"x\":").Append(x.ToString(CultureInfo.InvariantCulture));
            json.Append(",\"y\":").Append(y.ToString(CultureInfo.InvariantCulture));
            json.Append(",\"width\":").Append(width.ToString(CultureInfo.InvariantCulture));
            json.Append(",\"height\":").Append(height.ToString(CultureInfo.InvariantCulture));
        }

        HarpiaReply Send(string method, string path, string body)
        {
            var reply = new HarpiaReply();
            byte[] payload = Encoding.UTF8.GetBytes(body ?? "");
            var head = new StringBuilder();
            head.Append(method).Append(' ').Append(path).Append(" HTTP/1.1\r\n");
            head.Append("Host: 127.0.0.1:").Append(Port.ToString(CultureInfo.InvariantCulture)).Append("\r\n");
            head.Append("X-Harpia-Client: ").Append(HeaderSafe(ClientName)).Append("\r\n");
            head.Append("Connection: close\r\n");
            if (body != null)
            {
                head.Append("Content-Type: application/json\r\n");
                head.Append("Content-Length: ").Append(payload.Length.ToString(CultureInfo.InvariantCulture)).Append("\r\n");
            }
            head.Append("\r\n");

            byte[] raw;
            try
            {
                using (var tcp = new TcpClient())
                {
                    IAsyncResult connecting = tcp.BeginConnect("127.0.0.1", Port, null, null);
                    if (!connecting.AsyncWaitHandle.WaitOne(TimeoutMs) || !tcp.Connected)
                    {
                        try { tcp.EndConnect(connecting); } catch (Exception) { }
                        return Unreachable(reply);
                    }
                    tcp.EndConnect(connecting);
                    tcp.SendTimeout = TimeoutMs;
                    tcp.ReceiveTimeout = TimeoutMs;
                    using (NetworkStream stream = tcp.GetStream())
                    {
                        byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
                        stream.Write(headBytes, 0, headBytes.Length);
                        if (payload.Length > 0)
                            stream.Write(payload, 0, payload.Length);
                        stream.Flush();
                        raw = ReadAll(stream);
                    }
                }
            }
            catch (SocketException e)
            {
                if (e.SocketErrorCode == SocketError.ConnectionRefused)
                    return Unreachable(reply);
                reply.Error = "Could not talk to Harpia: " + e.Message;
                return reply;
            }
            catch (IOException e)
            {
                reply.Error = "Harpia did not answer in time (" + e.Message + ")";
                return reply;
            }
            catch (ObjectDisposedException e)
            {
                reply.Error = "Could not talk to Harpia: " + e.Message;
                return reply;
            }
            return Parse(raw, reply);
        }

        HarpiaReply Unreachable(HarpiaReply reply)
        {
            reply.Unreachable = true;
            reply.Error = "Harpia isn't running (nothing is listening on 127.0.0.1:" +
                          Port.ToString(CultureInfo.InvariantCulture) + ")";
            return reply;
        }

        static byte[] ReadAll(NetworkStream stream)
        {
            var buffer = new MemoryStream();
            var chunk = new byte[4096];
            int n;
            while ((n = stream.Read(chunk, 0, chunk.Length)) > 0)
                buffer.Write(chunk, 0, n);
            return buffer.ToArray();
        }

        // "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\n...\r\n\r\n{json}"
        // or, for a thumbnail, "Content-Type: image/png" and the PNG's bytes.
        public static HarpiaReply Parse(byte[] raw, HarpiaReply reply)
        {
            raw = raw ?? new byte[0];
            int headEnd = IndexOf(raw, new byte[] { 13, 10, 13, 10 });
            if (headEnd < 0)
                return Unreadable(reply);
            string[] head = Encoding.ASCII.GetString(raw, 0, headEnd).Split(new[] { "\r\n" }, StringSplitOptions.None);
            string[] statusLine = head[0].Split(' ');
            int status;
            if (statusLine.Length < 2 || !int.TryParse(statusLine[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out status))
                return Unreadable(reply);
            reply.Status = status;
            for (int i = 1; i < head.Length; i++)
            {
                int colon = head[i].IndexOf(':');
                if (colon > 0 && string.Equals(head[i].Substring(0, colon).Trim(), "Content-Type", StringComparison.OrdinalIgnoreCase))
                    reply.ContentType = head[i].Substring(colon + 1).Trim();
            }
            int bodyStart = headEnd + 4;
            if (reply.ContentType.Length > 0 && !reply.ContentType.StartsWith("application/json", StringComparison.OrdinalIgnoreCase))
            {
                reply.Bytes = new byte[raw.Length - bodyStart];
                Array.Copy(raw, bodyStart, reply.Bytes, 0, reply.Bytes.Length);
                reply.Ok = status == 200;
                if (!reply.Ok)
                    reply.Error = "Harpia answered " + status.ToString(CultureInfo.InvariantCulture);
                return reply;
            }
            reply.Body = Encoding.UTF8.GetString(raw, bodyStart, raw.Length - bodyStart);
            reply.Data = Json.ParseObject(reply.Body);
            reply.Ok = Json.Bool(reply.Data, "ok") && status == 200;
            reply.State = Json.Str(reply.Data, "state");
            reply.Note = Json.Str(reply.Data, "note");
            reply.Pending = Json.Bool(reply.Data, "pending");
            if (!reply.Ok)
            {
                string error = Json.Str(reply.Data, "error");
                reply.Error = error.Length > 0 ? error : "Harpia answered " + status.ToString(CultureInfo.InvariantCulture);
            }
            return reply;
        }

        static HarpiaReply Unreadable(HarpiaReply reply)
        {
            reply.Error = "Harpia sent an answer that could not be read";
            return reply;
        }

        static int IndexOf(byte[] haystack, byte[] needle)
        {
            for (int i = 0; i + needle.Length <= haystack.Length; i++)
            {
                int j = 0;
                while (j < needle.Length && haystack[i + j] == needle[j])
                    j++;
                if (j == needle.Length)
                    return i;
            }
            return -1;
        }

        static string HeaderSafe(string s)
        {
            var b = new StringBuilder();
            foreach (char c in s)
                b.Append(c >= 32 && c < 127 ? c : '?');
            return b.ToString();
        }
    }
}
