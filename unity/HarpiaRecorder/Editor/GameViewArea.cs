// Where the game's picture is on screen, in the physical desktop pixels Harpia
// records -- the picture only: no Game view toolbar, no tab, no letterbox bars.
//
// Read from Unity's own Game view by reflection. Every member used here was
// checked against Unity's C# reference source
// (github.com/Unity-Technologies/UnityCsReference, branches 2021.3, 2022.3,
// 6000.0, 6000.3 and master), where they are the same:
//
//   PlayModeView.GetMainPlayModeView()  the view Play Mode renders into (no side effects)
//   GameView.viewInWindow               the area under the Game view's toolbar, in its GUI (points)
//   GameView.m_ZoomArea                 the zoom: drawRect, the base range (the picture in
//                                       content space) and DrawingToViewTransformPoint/Vector
//   EditorWindow.m_Parent               the HostView (dock area) the Game view is in
//   HostView.borderSize                 its tab bar and borders
//   GUIView.GetBackingScaleFactor()     that view's display scaling
//   View.screenPosition                 where the host is, in Unity's screen points
//   EditorWindow.position               where the Game view's GUI starts, in the same points
//                                       (the dock area sets it to GUIToScreenPoint(0,0) as it draws)
//
// The picture is where GameView.OnGUI draws it: GUI.BeginGroup(m_ZoomArea.drawRect),
// then DrawTexture at targetInView, the zoom transform of the base range, in the
// Game view's GUI. That GUI starts inside its host at EditorWindow.position minus
// the host's screen position -- or, if that looks stale, where
// GameView.viewInParent puts it: (borderSize.left, borderSize.top + borderSize.bottom).
//
// GameView.targetInView itself is not used: before Unity 6 it converts with
// EditorGUIUtility.pixelsPerPoint, which outside the Game view's own OnGUI is
// whichever view drew last -- the wrong scaling when displays differ
// ("pixelsPerPoint only reliable in OnGUI()", says GameView.cs). The base range
// is set by the Game view in its own OnGUI, so it is right everywhere.
//
// From points to physical pixels. Inside one view a point is exactly its
// backing scale in pixels. Where the view is on the desktop, Unity says only in
// its own screen points, and how those map to pixels across displays at
// different scalings is up to Unity's native code (not in the reference). So on
// Windows the host view's own window is found (Unity gives each view a child
// window) and its position read from Windows, in physical pixels. Elsewhere, or
// if no window matches, Unity's screen points are scaled -- exact when every
// display has the same scaling.

using System;
using System.Reflection;
using UnityEditor;
using UnityEngine;
#if UNITY_EDITOR_WIN
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
#endif

namespace Harpia.PlayModeRecorder
{
    public enum RecordArea
    {
        GamePicture = 0,   // just the rendered game, without the Game view's toolbar or letterbox bars
        WholeGameView = 1, // everything under the Game view's toolbar
    }

    public struct PixelRect
    {
        public int X, Y, Width, Height;

        public override string ToString()
        {
            return Width + "x" + Height + " at (" + X + ", " + Y + ")";
        }
    }

    // What GameViewArea found.
    public struct GameViewMeasure
    {
        public PixelRect Area;   // what Harpia records: physical pixels of the whole desktop
        public int GameWidth;    // the game's own render resolution (0 when unknown)
        public int GameHeight;
        public float Scaling;    // the Game view's display scaling (1.5 = 150%)
        public bool Exact;       // the desktop position came from Windows, not Unity's screen points

        // How big the picture is on screen next to the game's resolution (1 = pixel for pixel).
        public float Shown
        {
            get { return GameWidth > 0 ? (float)Area.Width / GameWidth : 1f; }
        }

        public bool PixelForPixel
        {
            get { return GameWidth <= 0 || (Math.Abs(Area.Width - GameWidth) <= 2 && Math.Abs(Area.Height - GameHeight) <= 2); }
        }

        public string Describe()
        {
            string s = Area.ToString();
            if (GameWidth > 0)
                s += PixelForPixel
                    ? ", the game's full " + GameWidth + "x" + GameHeight
                    : "; the game renders " + GameWidth + "x" + GameHeight + ", shown at " + Mathf.RoundToInt(Shown * 100f) + "%";
            s += "; display scaling " + Mathf.RoundToInt(Scaling * 100f) + "%";
            s += Exact ? "; position from Windows" : "; position estimated from Unity's screen coordinates";
            return s;
        }
    }

    public static class GameViewArea
    {
        const float FallbackToolbarPoints = 21f; // EditorGUI.kWindowToolbarHeight
        const BindingFlags Static = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
        const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

        static readonly Type GameViewType = typeof(EditorWindow).Assembly.GetType("UnityEditor.GameView");
        static readonly Type PlayModeViewType = typeof(EditorWindow).Assembly.GetType("UnityEditor.PlayModeView");

        // The view Play Mode renders into: the maximized one during "Play Maximized",
        // else the last focused. Null (and why) when it is not a Game view.
        public static EditorWindow FindGameView(out string why)
        {
            why = "";
            if (GameViewType == null)
            {
                why = "this Unity version's Game view could not be found";
                return null;
            }
            EditorWindow main = CallStatic(PlayModeViewType, "GetMainPlayModeView") ??
                                CallStatic(PlayModeViewType, "GetLastFocusedPlayModeView");
            if (main != null && GameViewType.IsInstanceOfType(main))
                return main;
            if (main != null)
            {
                why = "Play Mode is showing in \"" + main.titleContent.text + "\" (the Device Simulator?); switch it to the Game view";
                return null;
            }
            UnityEngine.Object[] all = Resources.FindObjectsOfTypeAll(GameViewType);
            if (all.Length > 0)
                return all[0] as EditorWindow;
            why = "no Game view is open";
            return null;
        }

        public static EditorWindow FindGameView()
        {
            string why;
            return FindGameView(out why);
        }

        public static bool TryGet(RecordArea mode, out PixelRect px, out string why)
        {
            GameViewMeasure m;
            bool ok = TryMeasure(mode, out m, out why);
            px = m.Area;
            return ok;
        }

        public static bool TryMeasure(RecordArea mode, out GameViewMeasure m, out string why)
        {
            m = default(GameViewMeasure);
            EditorWindow view = FindGameView(out why);
            if (view == null)
                return false;
            object host = GetField(typeof(EditorWindow), view, "m_Parent");
            if (host == null)
            {
                why = "the Game view is not in a window";
                return false;
            }

            float scale = BackingScale(host);
            Rect window = view.position;
            Rect viewInWindow = GetRect(view, "viewInWindow") ??
                                new Rect(0, FallbackToolbarPoints, window.width, window.height - FallbackToolbarPoints);
            Rect hostScreen = GetRect(host, "screenPosition") ?? new Rect(window.x, window.y, window.width, window.height);

            // Where the Game view's GUI starts inside its host: both positions are
            // Unity screen points of the same window, so their difference is exact.
            // A tab bar and borders are a few dozen points; anything else is stale.
            var paneInHost = new Vector2(window.x - hostScreen.x, window.y - hostScreen.y);
            if (paneInHost.x < -1f || paneInHost.x > 64f || paneInHost.y < -1f || paneInHost.y > 64f)
            {
                var border = GetProperty(host, "borderSize") as RectOffset;
                paneInHost = border != null ? new Vector2(border.left, border.top + border.bottom) : new Vector2(0f, 0f);
            }

            // The area under the toolbar, and the picture in it; in host points.
            var area = new Rect(paneInHost.x + viewInWindow.x, paneInHost.y + viewInWindow.y, viewInWindow.width, viewInWindow.height);
            Rect chosen = area;
            Rect picture;
            Vector2 content;
            if (TryPicture(view, viewInWindow, out picture, out content))
            {
                m.GameWidth = Mathf.RoundToInt(content.x * scale);
                m.GameHeight = Mathf.RoundToInt(content.y * scale);
                if (mode == RecordArea.GamePicture)
                {
                    // Zoomed in, the picture runs past the view: what shows is the overlap.
                    var placed = new Rect(paneInHost.x + picture.x, paneInHost.y + picture.y, picture.width, picture.height);
                    chosen = Intersect(placed, area);
                }
            }
            else if (mode == RecordArea.GamePicture)
            {
                Vector2 game = Handles.GetMainGameViewSize();
                if (game.x > 0 && game.y > 0)
                {
                    chosen = FitAspect(area, game.x / game.y);
                    m.GameWidth = Mathf.RoundToInt(game.x);
                    m.GameHeight = Mathf.RoundToInt(game.y);
                }
            }

            // The host's top-left on the desktop, in physical pixels.
            Vector2 hostPx = new Vector2(hostScreen.x * scale, hostScreen.y * scale);
#if UNITY_EDITOR_WIN
            Vector2 found;
            if (HostWindow.TryFind(new Vector2(hostScreen.width * scale, hostScreen.height * scale), hostPx, scale, out found))
            {
                hostPx = found;
                m.Exact = true;
            }
#endif
            int x0 = Mathf.RoundToInt(hostPx.x + chosen.xMin * scale), y0 = Mathf.RoundToInt(hostPx.y + chosen.yMin * scale);
            int x1 = Mathf.RoundToInt(hostPx.x + chosen.xMax * scale), y1 = Mathf.RoundToInt(hostPx.y + chosen.yMax * scale);
            m.Area = new PixelRect { X = x0, Y = y0, Width = x1 - x0, Height = y1 - y0 };
            m.Scaling = scale;
            if (m.Area.Width < 16 || m.Area.Height < 16)
            {
                why = "the Game view is too small to record";
                return false;
            }
            return true;
        }

        // The picture in the Game view's GUI (points), and its size in content
        // space (points of the render target), from the zoom area the Game view
        // configures in its own OnGUI.
        static bool TryPicture(EditorWindow view, Rect viewInWindow, out Rect picture, out Vector2 content)
        {
            picture = default(Rect);
            content = default(Vector2);
            object zoom = GetField(GameViewType, view, "m_ZoomArea");
            if (zoom == null)
                return false;
            float hMin, hMax, vMin, vMax;
            if (!GetFloat(zoom, "hBaseRangeMin", out hMin) || !GetFloat(zoom, "hBaseRangeMax", out hMax) ||
                !GetFloat(zoom, "vBaseRangeMin", out vMin) || !GetFloat(zoom, "vBaseRangeMax", out vMax))
                return false;
            if (!(hMax - hMin > 1f && vMax - vMin > 1f))
                return false; // not drawn yet
            Vector2 at, size;
            if (!Transform(zoom, "DrawingToViewTransformPoint", new Vector2(hMin, vMin), out at) ||
                !Transform(zoom, "DrawingToViewTransformVector", new Vector2(hMax - hMin, vMax - vMin), out size))
                return false;
            // A negative size (y up) is the same rectangle the other way round.
            float x0 = Math.Min(at.x, at.x + size.x), x1 = Math.Max(at.x, at.x + size.x);
            float y0 = Math.Min(at.y, at.y + size.y), y1 = Math.Max(at.y, at.y + size.y);
            if (x1 - x0 < 1f || y1 - y0 < 1f)
                return false;
            // OnGUI draws it inside GUI.BeginGroup(m_ZoomArea.drawRect).
            Rect? draw = GetRect(zoom, "drawRect");
            Vector2 origin = draw.HasValue && draw.Value.width > 0 ? draw.Value.position : viewInWindow.position;
            picture = new Rect(origin.x + x0, origin.y + y0, x1 - x0, y1 - y0);
            content = new Vector2(hMax - hMin, vMax - vMin);
            // Stale (the view changed and has not drawn since) when none of it would show.
            return Overlaps(picture, viewInWindow);
        }

        static float BackingScale(object host)
        {
            MethodInfo m = FindMethod(host.GetType(), "GetBackingScaleFactor", Type.EmptyTypes);
            if (m != null)
            {
                try
                {
                    float s = (float)m.Invoke(host, null);
                    if (s > 0.25f && s < 8f)
                        return s;
                }
                catch (Exception)
                {
                }
            }
            return EditorGUIUtility.pixelsPerPoint;
        }

        // ---- Reflection, walking up the class hierarchy -------------------------

        static EditorWindow CallStatic(Type type, string method)
        {
            if (type == null)
                return null;
            MethodInfo m = type.GetMethod(method, Static, null, Type.EmptyTypes, null);
            if (m == null)
                return null;
            try
            {
                return m.Invoke(null, null) as EditorWindow;
            }
            catch (Exception)
            {
                return null;
            }
        }

        static object GetField(Type declaring, object target, string name)
        {
            for (Type t = declaring ?? target.GetType(); t != null; t = t.BaseType)
            {
                FieldInfo f = t.GetField(name, Instance);
                if (f != null)
                {
                    try
                    {
                        return f.GetValue(target);
                    }
                    catch (Exception)
                    {
                        return null;
                    }
                }
            }
            return null;
        }

        static object GetProperty(object target, string name)
        {
            for (Type t = target.GetType(); t != null; t = t.BaseType)
            {
                PropertyInfo p = t.GetProperty(name, Instance);
                if (p != null && p.GetIndexParameters().Length == 0)
                {
                    try
                    {
                        return p.GetValue(target, null);
                    }
                    catch (Exception)
                    {
                        return null;
                    }
                }
            }
            return null;
        }

        static Rect? GetRect(object target, string name)
        {
            object v = GetProperty(target, name);
            return v is Rect ? (Rect)v : (Rect?)null;
        }

        static bool GetFloat(object target, string name, out float value)
        {
            object v = GetProperty(target, name);
            value = v is float ? (float)v : 0f;
            return v is float;
        }

        static MethodInfo FindMethod(Type type, string name, Type[] parameters)
        {
            for (Type t = type; t != null; t = t.BaseType)
            {
                MethodInfo m = t.GetMethod(name, Instance, null, parameters, null);
                if (m != null)
                    return m;
            }
            return null;
        }

        static bool Transform(object zoom, string method, Vector2 v, out Vector2 result)
        {
            result = default(Vector2);
            MethodInfo m = FindMethod(zoom.GetType(), method, new[] { typeof(Vector2) });
            if (m == null || m.ReturnType != typeof(Vector2))
                return false;
            try
            {
                result = (Vector2)m.Invoke(zoom, new object[] { v });
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // ---- Rectangles ------------------------------------------------------------

        static bool Overlaps(Rect a, Rect b)
        {
            return a.xMin < b.xMax && b.xMin < a.xMax && a.yMin < b.yMax && b.yMin < a.yMax;
        }

        static Rect Intersect(Rect a, Rect b)
        {
            float x0 = Mathf.Max(a.xMin, b.xMin), y0 = Mathf.Max(a.yMin, b.yMin);
            float x1 = Mathf.Min(a.xMax, b.xMax), y1 = Mathf.Min(a.yMax, b.yMax);
            return x1 > x0 && y1 > y0 ? Rect.MinMaxRect(x0, y0, x1, y1) : b;
        }

        // The largest rectangle of `aspect` centred in `area`.
        static Rect FitAspect(Rect area, float aspect)
        {
            float w = area.width, h = area.width / aspect;
            if (h > area.height)
            {
                h = area.height;
                w = area.height * aspect;
            }
            return new Rect(area.x + (area.width - w) / 2f, area.y + (area.height - h) / 2f, w, h);
        }

#if UNITY_EDITOR_WIN
        // The desktop position, in physical pixels, of a Unity view's own window.
        // Unity gives each view (dock area, maximized view...) a child window the
        // size of the view; the one whose size matches is it. Several the same
        // size: the nearest to where Unity thinks it is, preferring Unity's view
        // windows at the right display scaling.
        static class HostWindow
        {
            [StructLayout(LayoutKind.Sequential)]
            struct RECT
            {
                public int Left, Top, Right, Bottom;
            }

            [StructLayout(LayoutKind.Sequential)]
            struct POINT
            {
                public int X, Y;
            }

            delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

            [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
            [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lParam);
            [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
            [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
            [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
            [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
            [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hWnd, StringBuilder name, int max);
            [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr hWnd);
            [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

            static readonly IntPtr PerMonitorAwareV2 = new IntPtr(-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            const float Tolerance = 3f; // px: Unity rounds a view's size to whole pixels

            public static bool TryFind(Vector2 sizePx, Vector2 nearPx, float scale, out Vector2 originPx)
            {
                originPx = Vector2.zero;
                IntPtr previous = IntPtr.Zero;
                try
                {
                    // Physical pixels from GetClientRect / ClientToScreen, whatever this thread was set to.
                    previous = SetThreadDpiAwarenessContext(PerMonitorAwareV2);
                }
                catch (Exception)
                {
                }
                try
                {
                    uint me = (uint)Process.GetCurrentProcess().Id;
                    float best = float.MaxValue;
                    Vector2 bestOrigin = Vector2.zero;
                    var name = new StringBuilder(64);
                    EnumProc consider = (hWnd, _) =>
                    {
                        RECT r;
                        if (!IsWindowVisible(hWnd) || !GetClientRect(hWnd, out r))
                            return true;
                        if (Math.Abs(r.Right - r.Left - sizePx.x) > Tolerance || Math.Abs(r.Bottom - r.Top - sizePx.y) > Tolerance)
                            return true;
                        var p = new POINT();
                        if (!ClientToScreen(hWnd, ref p))
                            return true;
                        float score = Mathf.Abs(p.X - nearPx.x) + Mathf.Abs(p.Y - nearPx.y);
                        name.Length = 0;
                        GetClassName(hWnd, name, name.Capacity);
                        if (name.ToString().IndexOf("GUIView", StringComparison.OrdinalIgnoreCase) < 0)
                            score += 1e6f;
                        try
                        {
                            uint dpi = GetDpiForWindow(hWnd);
                            if (dpi > 0 && Mathf.Abs(dpi / 96f - scale) > 0.02f)
                                score += 1e5f;
                        }
                        catch (Exception)
                        {
                        }
                        if (score < best)
                        {
                            best = score;
                            bestOrigin = new Vector2(p.X, p.Y);
                        }
                        return true;
                    };
                    EnumWindows((top, _) =>
                    {
                        uint pid;
                        GetWindowThreadProcessId(top, out pid);
                        if (pid == me && IsWindowVisible(top))
                        {
                            consider(top, IntPtr.Zero);
                            EnumChildWindows(top, consider, IntPtr.Zero);
                        }
                        return true;
                    }, IntPtr.Zero);
                    GC.KeepAlive(consider);
                    if (best == float.MaxValue)
                        return false;
                    originPx = bestOrigin;
                    return true;
                }
                catch (Exception)
                {
                    return false;
                }
                finally
                {
                    if (previous != IntPtr.Zero)
                    {
                        try
                        {
                            SetThreadDpiAwarenessContext(previous);
                        }
                        catch (Exception)
                        {
                        }
                    }
                }
            }
        }
#endif
    }
}
