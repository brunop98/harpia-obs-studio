// Talks to Harpia Recorder and Editor's local remote control (127.0.0.1).
//
// Plain .NET, no UnityEngine: the same file is compiled and run against the
// real Harpia server by the package's tests (Tests~), outside Unity.
//
// One HTTP request per command, over a raw socket with hard timeouts, written
// and read synchronously. No HttpClient: its async plumbing and Unity's main
// thread do not mix well when a command has to be sent RIGHT NOW -- leaving
// Play Mode reloads the scripts a moment later, and an in-flight request would
// die with them.

using System;
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
        public string Body = "";    // the raw JSON

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

        // "HTTP/1.1 409 Conflict\r\n...\r\n\r\n{json}"
        public static HarpiaReply Parse(byte[] raw, HarpiaReply reply)
        {
            string text = Encoding.UTF8.GetString(raw ?? new byte[0]);
            int lineEnd = text.IndexOf("\r\n", StringComparison.Ordinal);
            int headEnd = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
            if (lineEnd < 0 || headEnd < 0)
            {
                reply.Error = "Harpia sent an answer that could not be read";
                return reply;
            }
            string[] statusLine = text.Substring(0, lineEnd).Split(' ');
            int status;
            if (statusLine.Length < 2 || !int.TryParse(statusLine[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out status))
            {
                reply.Error = "Harpia sent an answer that could not be read";
                return reply;
            }
            reply.Status = status;
            reply.Body = text.Substring(headEnd + 4);
            bool ok;
            reply.Ok = Json.TryGetBool(reply.Body, "ok", out ok) && ok && status == 200;
            reply.State = Json.GetString(reply.Body, "state");
            reply.Note = Json.GetString(reply.Body, "note");
            if (!reply.Ok)
            {
                string error = Json.GetString(reply.Body, "error");
                reply.Error = error.Length > 0 ? error : "Harpia answered " + status.ToString(CultureInfo.InvariantCulture);
            }
            return reply;
        }

        static string HeaderSafe(string s)
        {
            var b = new StringBuilder();
            foreach (char c in s)
                b.Append(c >= 32 && c < 127 ? c : '?');
            return b.ToString();
        }
    }

    // Just enough JSON for Harpia's flat answers: quote a string, and read a
    // string or a bool by key from a flat object.
    public static class Json
    {
        public static string Quote(string s)
        {
            var b = new StringBuilder("\"");
            foreach (char c in s ?? "")
            {
                switch (c)
                {
                    case '"': b.Append("\\\""); break;
                    case '\\': b.Append("\\\\"); break;
                    case '\n': b.Append("\\n"); break;
                    case '\r': b.Append("\\r"); break;
                    case '\t': b.Append("\\t"); break;
                    default:
                        if (c < 32)
                            b.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                        else
                            b.Append(c);
                        break;
                }
            }
            return b.Append('"').ToString();
        }

        // The position just after "key": (and any spaces), or -1.
        static int ValueAt(string json, string key)
        {
            string needle = "\"" + key + "\"";
            int i = 0;
            while ((i = json.IndexOf(needle, i, StringComparison.Ordinal)) >= 0)
            {
                int j = i + needle.Length;
                while (j < json.Length && char.IsWhiteSpace(json[j])) j++;
                if (j < json.Length && json[j] == ':')
                {
                    j++;
                    while (j < json.Length && char.IsWhiteSpace(json[j])) j++;
                    return j;
                }
                i = j;
            }
            return -1;
        }

        public static bool TryGetBool(string json, string key, out bool value)
        {
            value = false;
            int j = ValueAt(json ?? "", key);
            if (j < 0)
                return false;
            if (string.CompareOrdinal(json, j, "true", 0, 4) == 0) { value = true; return true; }
            if (string.CompareOrdinal(json, j, "false", 0, 5) == 0) { value = false; return true; }
            return false;
        }

        public static string GetString(string json, string key)
        {
            int j = ValueAt(json ?? "", key);
            if (j < 0 || j >= json.Length || json[j] != '"')
                return "";
            var b = new StringBuilder();
            for (int i = j + 1; i < json.Length; i++)
            {
                char c = json[i];
                if (c == '"')
                    return b.ToString();
                if (c != '\\' || i + 1 >= json.Length)
                {
                    b.Append(c);
                    continue;
                }
                char e = json[++i];
                switch (e)
                {
                    case 'n': b.Append('\n'); break;
                    case 'r': b.Append('\r'); break;
                    case 't': b.Append('\t'); break;
                    case 'b': b.Append('\b'); break;
                    case 'f': b.Append('\f'); break;
                    case 'u':
                        int code;
                        if (i + 4 < json.Length &&
                            int.TryParse(json.Substring(i + 1, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out code))
                        {
                            b.Append((char)code);
                            i += 4;
                        }
                        break;
                    default: b.Append(e); break; // \" \\ \/
                }
            }
            return b.ToString();
        }
    }
}
