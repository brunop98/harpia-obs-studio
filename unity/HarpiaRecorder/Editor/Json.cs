// Just enough JSON for Harpia's answers: quote a string for a request, and
// read an answer into Dictionary<string, object> / List<object> / string /
// double / bool / null.
//
// Plain .NET, no UnityEngine (tested outside Unity: unity/Tests~ in the Harpia
// repository).
// UnityEngine.JsonUtility cannot read a list of objects without a class per
// shape, and cannot run in those tests.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Harpia.PlayModeRecorder
{
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

        // The whole text as one value. Throws FormatException when it is not JSON.
        public static object Parse(string json)
        {
            var p = new Parser(json);
            object value = p.Value(0);
            p.SkipSpace();
            if (!p.AtEnd)
                throw p.Bad("unexpected text after the value");
            return value;
        }

        // The text as an object, or an empty one when it is not a JSON object.
        public static Dictionary<string, object> ParseObject(string json)
        {
            try
            {
                return Parse(json) as Dictionary<string, object> ?? new Dictionary<string, object>();
            }
            catch (FormatException)
            {
                return new Dictionary<string, object>();
            }
        }

        // A top-level string of an object's text ("" when missing).
        public static string GetString(string json, string key)
        {
            return Str(ParseObject(json), key);
        }

        // A top-level bool of an object's text; false when missing.
        public static bool TryGetBool(string json, string key, out bool value)
        {
            object v;
            value = false;
            if (!ParseObject(json).TryGetValue(key, out v) || !(v is bool))
                return false;
            value = (bool)v;
            return true;
        }

        public static string Str(Dictionary<string, object> o, string key)
        {
            object v;
            return o != null && o.TryGetValue(key, out v) && v is string ? (string)v : "";
        }

        public static double Num(Dictionary<string, object> o, string key)
        {
            object v;
            return o != null && o.TryGetValue(key, out v) && v is double ? (double)v : 0.0;
        }

        public static bool Bool(Dictionary<string, object> o, string key)
        {
            object v;
            return o != null && o.TryGetValue(key, out v) && v is bool && (bool)v;
        }

        public static List<object> List(Dictionary<string, object> o, string key)
        {
            object v;
            return o != null && o.TryGetValue(key, out v) && v is List<object> ? (List<object>)v : new List<object>();
        }

        sealed class Parser
        {
            const int MaxDepth = 64;
            readonly string s;
            int i;

            public Parser(string text)
            {
                s = text ?? "";
            }

            public bool AtEnd
            {
                get { return i >= s.Length; }
            }

            public FormatException Bad(string what)
            {
                return new FormatException("JSON: " + what + " at " + i.ToString(CultureInfo.InvariantCulture));
            }

            public void SkipSpace()
            {
                while (i < s.Length && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                    i++;
            }

            void Expect(char c)
            {
                SkipSpace();
                if (i >= s.Length || s[i] != c)
                    throw Bad("expected '" + c + "'");
                i++;
            }

            public object Value(int depth)
            {
                if (depth > MaxDepth)
                    throw Bad("nested too deep");
                SkipSpace();
                if (i >= s.Length)
                    throw Bad("unexpected end");
                char c = s[i];
                if (c == '{')
                    return Object(depth);
                if (c == '[')
                    return Array(depth);
                if (c == '"')
                    return String();
                if (c == '-' || (c >= '0' && c <= '9'))
                    return Number();
                if (Word("true"))
                    return true;
                if (Word("false"))
                    return false;
                if (Word("null"))
                    return null;
                throw Bad("unexpected '" + c + "'");
            }

            bool Word(string w)
            {
                if (string.CompareOrdinal(s, i, w, 0, w.Length) != 0)
                    return false;
                i += w.Length;
                return true;
            }

            Dictionary<string, object> Object(int depth)
            {
                var o = new Dictionary<string, object>();
                i++; // {
                SkipSpace();
                if (i < s.Length && s[i] == '}')
                {
                    i++;
                    return o;
                }
                while (true)
                {
                    SkipSpace();
                    if (i >= s.Length || s[i] != '"')
                        throw Bad("expected a name");
                    string key = String();
                    Expect(':');
                    o[key] = Value(depth + 1);
                    SkipSpace();
                    if (i < s.Length && s[i] == ',')
                    {
                        i++;
                        continue;
                    }
                    Expect('}');
                    return o;
                }
            }

            List<object> Array(int depth)
            {
                var a = new List<object>();
                i++; // [
                SkipSpace();
                if (i < s.Length && s[i] == ']')
                {
                    i++;
                    return a;
                }
                while (true)
                {
                    a.Add(Value(depth + 1));
                    SkipSpace();
                    if (i < s.Length && s[i] == ',')
                    {
                        i++;
                        continue;
                    }
                    Expect(']');
                    return a;
                }
            }

            string String()
            {
                var b = new StringBuilder();
                i++; // "
                while (i < s.Length)
                {
                    char c = s[i++];
                    if (c == '"')
                        return b.ToString();
                    if (c != '\\')
                    {
                        b.Append(c);
                        continue;
                    }
                    if (i >= s.Length)
                        break;
                    char e = s[i++];
                    switch (e)
                    {
                        case 'n': b.Append('\n'); break;
                        case 'r': b.Append('\r'); break;
                        case 't': b.Append('\t'); break;
                        case 'b': b.Append('\b'); break;
                        case 'f': b.Append('\f'); break;
                        case 'u':
                            int code;
                            if (i + 4 > s.Length ||
                                !int.TryParse(s.Substring(i, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out code))
                                throw Bad("bad \\u escape");
                            b.Append((char)code); // a surrogate pair arrives as two escapes
                            i += 4;
                            break;
                        default: b.Append(e); break; // \" \\ \/
                    }
                }
                throw Bad("unterminated string");
            }

            double Number()
            {
                int start = i;
                while (i < s.Length && "+-0123456789.eE".IndexOf(s[i]) >= 0)
                    i++;
                double d;
                if (!double.TryParse(s.Substring(start, i - start), NumberStyles.Float, CultureInfo.InvariantCulture, out d))
                    throw Bad("bad number");
                return d;
            }
        }
    }
}
