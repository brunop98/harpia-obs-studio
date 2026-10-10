// Where the Game view is on screen, in the physical pixels Harpia records.
//
// Unity gives an editor window's position in points (pixels divided by the
// display scaling); Harpia works in physical pixels of the whole desktop, so
// the rectangle is multiplied by EditorGUIUtility.pixelsPerPoint. With every
// display at the same scaling that is exact; with mixed scaling it can be off
// -- "Show Recording Area" (Tools > Harpia Recorder) shows what Harpia sees.
//
// The rendered picture inside the Game view (without its toolbar and any
// letterbox bars) is read from the Game view's own layout through reflection
// (viewInWindow / targetInView, internal to Unity). If a Unity version renames
// them, the fallback works it out from the toolbar height and the game's
// aspect ratio.

using System;
using System.Reflection;
using UnityEditor;
using UnityEngine;

namespace Harpia.PlayModeRecorder
{
    public struct PixelRect
    {
        public int X, Y, Width, Height;

        public override string ToString()
        {
            return Width + "x" + Height + " at (" + X + ", " + Y + ")";
        }
    }

    public static class GameViewArea
    {
        const float FallbackToolbarPoints = 21f; // EditorGUI.kWindowToolbarHeight
        const BindingFlags Static = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
        const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

        static readonly Type GameViewType = typeof(EditorWindow).Assembly.GetType("UnityEditor.GameView");
        static readonly Type PlayModeViewType = typeof(EditorWindow).Assembly.GetType("UnityEditor.PlayModeView");

        // The Game view Unity plays into (the maximized one during "Maximize on Play").
        public static EditorWindow FindGameView()
        {
            if (GameViewType == null)
                return null;
            EditorWindow found =
                CallStatic(PlayModeViewType, "GetMainPlayModeView") ??
                CallStatic(GameViewType, "GetMainGameView") ??
                CallStatic(PlayModeViewType, "GetLastFocusedPlayModeView");
            if (found != null && GameViewType.IsInstanceOfType(found))
                return found;
            UnityEngine.Object[] all = Resources.FindObjectsOfTypeAll(GameViewType);
            return all.Length > 0 ? all[0] as EditorWindow : null;
        }

        public static bool TryGet(RecordArea mode, out PixelRect px, out string why)
        {
            px = default(PixelRect);
            why = "";
            if (GameViewType == null)
            {
                why = "this Unity version's Game view could not be found";
                return false;
            }
            EditorWindow gameView = FindGameView();
            if (gameView == null)
            {
                why = "no Game view is open";
                return false;
            }

            Rect window = gameView.position; // screen points, below the tab
            Rect view = GetRect(gameView, "viewInWindow") ??
                        new Rect(0, FallbackToolbarPoints, window.width, window.height - FallbackToolbarPoints);
            Rect points = new Rect(window.x + view.x, window.y + view.y, view.width, view.height);

            if (mode == RecordArea.GamePicture)
            {
                Rect? target = GetRect(gameView, "targetInView"); // relative to the view
                if (target.HasValue && target.Value.width > 1 && target.Value.height > 1)
                {
                    Rect t = target.Value;
                    // Zoomed in, the picture runs past the view: what shows is the overlap.
                    points = Intersect(new Rect(points.x + t.x, points.y + t.y, t.width, t.height), points);
                }
                else
                {
                    Vector2 game = Handles.GetMainGameViewSize();
                    if (game.x > 0 && game.y > 0)
                        points = FitAspect(points, game.x / game.y);
                }
            }

            float ppp = EditorGUIUtility.pixelsPerPoint;
            int x0 = Mathf.RoundToInt(points.xMin * ppp), y0 = Mathf.RoundToInt(points.yMin * ppp);
            int x1 = Mathf.RoundToInt(points.xMax * ppp), y1 = Mathf.RoundToInt(points.yMax * ppp);
            px = new PixelRect { X = x0, Y = y0, Width = x1 - x0, Height = y1 - y0 };
            if (px.Width < 16 || px.Height < 16)
            {
                why = "the Game view is too small to record";
                return false;
            }
            return true;
        }

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

        static Rect? GetRect(object target, string property)
        {
            PropertyInfo p = target.GetType().GetProperty(property, Instance);
            if (p == null || p.PropertyType != typeof(Rect))
                return null;
            try
            {
                return (Rect)p.GetValue(target, null);
            }
            catch (Exception)
            {
                return null;
            }
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
    }
}
