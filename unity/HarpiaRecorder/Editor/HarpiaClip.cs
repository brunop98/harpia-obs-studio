// One of Harpia's recordings, as GET /recordings describes it, and the list.
//
// Plain .NET, no UnityEngine (tested outside Unity: unity/Tests~ in the Harpia
// repository).

using System;
using System.Collections.Generic;

namespace Harpia.PlayModeRecorder
{
    public sealed class HarpiaClip
    {
        public string Path = "";     // as Harpia spells it; send it back unchanged
        public string Name = "";
        public string Folder = "";
        public long SizeBytes;
        public string Size = "";     // "12.3 MB"
        public DateTime ModifiedUtc;
        public string Age = "";      // "5 minutes ago" (as of the list)
        public string Date = "";     // "11 Oct 2026 14:32"
        public long DurationMs;      // 0 = not known (yet)
        public string Duration = ""; // "" when not known
        public string Preset = "";
        public string Kind = "video"; // "video", "gif" or "audio"

        public bool IsAudio
        {
            get { return Kind == "audio"; }
        }

        public bool IsGif
        {
            get { return Kind == "gif"; }
        }

        // Stays the same while the file does: a key for its thumbnail.
        public string Key
        {
            get { return Path + "|" + ModifiedUtc.Ticks + "|" + SizeBytes; }
        }

        public static HarpiaClip From(Dictionary<string, object> o)
        {
            var c = new HarpiaClip
            {
                Path = Json.Str(o, "path"),
                Name = Json.Str(o, "name"),
                Folder = Json.Str(o, "folder"),
                SizeBytes = (long)Json.Num(o, "sizeBytes"),
                Size = Json.Str(o, "size"),
                Age = Json.Str(o, "age"),
                Date = Json.Str(o, "date"),
                DurationMs = (long)Json.Num(o, "durationMs"),
                Duration = Json.Str(o, "duration"),
                Preset = Json.Str(o, "preset"),
                Kind = Json.Str(o, "kind"),
            };
            if (c.Kind.Length == 0)
                c.Kind = "video";
            if (c.Name.Length == 0)
                c.Name = System.IO.Path.GetFileName(c.Path);
            c.ModifiedUtc = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddMilliseconds(Json.Num(o, "modifiedMs"));
            return c;
        }
    }

    public sealed class HarpiaRecordingList
    {
        public List<HarpiaClip> Clips = new List<HarpiaClip>();
        public int Total;     // all of them, when Clips is only the newest
        public bool Pending;  // some durations are still being read: ask again shortly

        public static HarpiaRecordingList From(HarpiaReply reply)
        {
            var list = new HarpiaRecordingList();
            if (reply == null || !reply.Ok)
                return list;
            foreach (object item in Json.List(reply.Data, "recordings"))
            {
                var o = item as Dictionary<string, object>;
                if (o != null && Json.Str(o, "path").Length > 0)
                    list.Clips.Add(HarpiaClip.From(o));
            }
            list.Total = Math.Max(list.Clips.Count, (int)Json.Num(reply.Data, "total"));
            list.Pending = Json.Bool(reply.Data, "pending");
            return list;
        }
    }
}
