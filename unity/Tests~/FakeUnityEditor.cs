// A stand-in for the parts of Unity that GameViewArea reads, so its reflection
// and arithmetic run under Mono (GameViewAreaTests.cs).
//
// The internal classes have the same names, member kinds (field / property /
// method, with overloads), visibility and inheritance as Unity's own, and the
// geometry they compute is copied from Unity's C# reference source
// (github.com/Unity-Technologies/UnityCsReference, 2021.3 to master):
//
//   EditorWindow    internal HostView m_Parent; position = m_Pos
//   View            public position / screenPosition
//   GUIView         internal float GetBackingScaleFactor()
//   HostView        internal RectOffset borderSize => GetBorderSize()
//   DockArea        GetBorderSize: left 1 (not at the edge), top kTabHeight 24, bottom 2
//   MaximizedHostView  GetBorderSize: top 24, bottom 4
//   PlayModeView    internal static GetMainPlayModeView() / GetLastFocusedPlayModeView()
//   GameView        private Rect viewInWindow; [SerializeField] ZoomableArea m_ZoomArea;
//                   ConfigureZoomArea() sets the base range to targetInContent
//   ZoomableArea    base range properties, drawRect, DrawingToViewTransformPoint/Vector
//                   (Vector2 and Vector3 overloads, as in Unity)
//
// Only what the measurement touches is here; the public API surface the
// scripts compile against is in the same spirit (documented signatures).
using System;
using System.Collections.Generic;

namespace UnityEngine
{
    public class Object
    {
        public HideFlags hideFlags { get; set; }
        public static void DestroyImmediate(Object obj) { }
    }
    public enum HideFlags { None = 0, HideAndDontSave = 61 }
    public class ScriptableObject : Object { }
    public sealed class SerializeField : Attribute { }

    public struct Vector2
    {
        public float x, y;
        public Vector2(float x, float y) { this.x = x; this.y = y; }
        public static Vector2 zero { get { return new Vector2(0, 0); } }
        public override string ToString() { return "(" + x + ", " + y + ")"; }
    }

    public struct Vector3
    {
        public float x, y, z;
        public Vector3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
    }

    public struct Rect
    {
        public float x, y, width, height;
        public Rect(float x, float y, float w, float h) { this.x = x; this.y = y; width = w; height = h; }
        public Rect(Vector2 position, Vector2 size) { x = position.x; y = position.y; width = size.x; height = size.y; }
        public Vector2 position { get { return new Vector2(x, y); } set { x = value.x; y = value.y; } }
        public Vector2 size { get { return new Vector2(width, height); } }
        public float xMin { get { return x; } }
        public float yMin { get { return y; } }
        public float xMax { get { return x + width; } }
        public float yMax { get { return y + height; } }
        public static Rect MinMaxRect(float a, float b, float c, float d) { return new Rect(a, b, c - a, d - b); }
        public override string ToString() { return "(" + x + ", " + y + ", " + width + "x" + height + ")"; }
    }

    public class RectOffset
    {
        public int left { get; set; }
        public int right { get; set; }
        public int top { get; set; }
        public int bottom { get; set; }
    }

    public class GUIContent
    {
        public GUIContent(string text) { this.text = text; }
        public string text { get; set; }
    }

    public static class Mathf
    {
        public static int RoundToInt(float f) { return (int)Math.Round(f); }
        public static float Max(float a, float b) { return Math.Max(a, b); }
        public static float Min(float a, float b) { return Math.Min(a, b); }
        public static float Abs(float f) { return Math.Abs(f); }
    }

    public static class Resources
    {
        public static List<Object> All = new List<Object>();
        public static Object[] FindObjectsOfTypeAll(Type t) { return All.FindAll(t.IsInstanceOfType).ToArray(); }
    }
}

namespace UnityEditor
{
    using UnityEngine;

    public static class EditorGUIUtility
    {
        // The scaling of whichever view drew last: deliberately not the Game view's in the tests.
        public static float pixelsPerPoint { get; set; } = 1f;
    }

    public static class Handles
    {
        public static Vector2 GameViewSize;
        public static Vector2 GetMainGameViewSize() { return GameViewSize; }
    }

    public class EditorWindow : ScriptableObject
    {
        internal HostView m_Parent;
        internal Rect m_Pos;
        public Rect position { get { return m_Pos; } set { m_Pos = value; } }
        public GUIContent titleContent { get; set; } = new GUIContent("");
    }

    internal class View : ScriptableObject
    {
        Rect m_Position;
        internal Vector2 m_ScreenOrigin; // the container window's top-left (Unity screen points)
        public Rect position { get { return m_Position; } set { m_Position = value; } }
        public Rect windowPosition { get { return m_Position; } }
        public Rect screenPosition
        {
            get { return new Rect(m_ScreenOrigin.x + m_Position.x, m_ScreenOrigin.y + m_Position.y, m_Position.width, m_Position.height); }
        }
    }

    internal class GUIView : View
    {
        internal float m_BackingScale = 1f;
        internal float GetBackingScaleFactor() { return m_BackingScale; }
    }

    internal class HostView : GUIView
    {
        protected RectOffset m_BorderSize = new RectOffset();
        internal RectOffset borderSize => GetBorderSize();
        protected virtual RectOffset GetBorderSize() { return m_BorderSize; }
    }

    internal class DockArea : HostView
    {
        internal const float kTabHeight = 25 - 1;
        internal bool m_AtLeftEdge, m_BottomTab;
        protected override RectOffset GetBorderSize()
        {
            m_BorderSize.left = m_AtLeftEdge ? 0 : 1;
            m_BorderSize.right = 1;
            m_BorderSize.top = (int)kTabHeight;
            m_BorderSize.bottom = m_BottomTab ? 0 : 2;
            return m_BorderSize;
        }
    }

    internal class MaximizedHostView : HostView
    {
        protected override RectOffset GetBorderSize()
        {
            m_BorderSize.left = 0;
            m_BorderSize.right = 0;
            m_BorderSize.top = (int)DockArea.kTabHeight;
            m_BorderSize.bottom = 4;
            return m_BorderSize;
        }
    }

    internal class ZoomableArea
    {
        float m_HBaseRangeMin, m_HBaseRangeMax, m_VBaseRangeMin, m_VBaseRangeMax;
        Rect m_DrawArea;
        [SerializeField] internal Vector2 m_Scale = new Vector2(1, -1);
        [SerializeField] internal Vector2 m_Translation = new Vector2(0, 0);
        public float hBaseRangeMin { get { return m_HBaseRangeMin; } set { m_HBaseRangeMin = value; } }
        public float hBaseRangeMax { get { return m_HBaseRangeMax; } set { m_HBaseRangeMax = value; } }
        public float vBaseRangeMin { get { return m_VBaseRangeMin; } set { m_VBaseRangeMin = value; } }
        public float vBaseRangeMax { get { return m_VBaseRangeMax; } set { m_VBaseRangeMax = value; } }
        public Rect rect { get { return m_DrawArea; } set { m_DrawArea = value; } }
        public Rect drawRect { get { return m_DrawArea; } }
        public Vector2 scale { get { return m_Scale; } }
        public Vector2 DrawingToViewTransformPoint(Vector2 lhs)
        { return new Vector2(lhs.x * m_Scale.x + m_Translation.x, lhs.y * m_Scale.y + m_Translation.y); }
        public Vector3 DrawingToViewTransformPoint(Vector3 lhs)
        { return new Vector3(lhs.x * m_Scale.x + m_Translation.x, lhs.y * m_Scale.y + m_Translation.y, 0); }
        public Vector2 DrawingToViewTransformVector(Vector2 lhs)
        { return new Vector2(lhs.x * m_Scale.x, lhs.y * m_Scale.y); }
        public Vector3 DrawingToViewTransformVector(Vector3 lhs)
        { return new Vector3(lhs.x * m_Scale.x, lhs.y * m_Scale.y, 0); }
    }

    internal class PlayModeView : EditorWindow
    {
        internal static PlayModeView s_LastFocused;
        internal static PlayModeView GetMainPlayModeView() { return s_LastFocused; }
        internal static PlayModeView GetLastFocusedPlayModeView() { return s_LastFocused; }
    }

    internal class SimulatorWindow : PlayModeView { }

    internal class GameView : PlayModeView
    {
        const float kWindowToolbarHeight = 21f; // EditorGUI.kWindowToolbarHeight
        [SerializeField] ZoomableArea m_ZoomArea = new ZoomableArea();
        internal bool m_ShowToolbar = true;

        Rect viewInWindow
        {
            get
            {
                if (m_ShowToolbar)
                    return new Rect(0, kWindowToolbarHeight, position.width, position.height - kWindowToolbarHeight);
                return new Rect(0, 0, position.width, position.height);
            }
        }

        // What Unity does when the Game view draws (DockArea.OnGUI sets m_Pos to the
        // pane's GUI origin; GameView.OnGUI calls ConfigureZoomArea), for a render
        // target of `targetPixels` at `zoom` x the default "fit" scale, panned by `pan`.
        internal void Draw(Vector2 targetPixels, float zoom, Vector2 pan)
        {
            HostView host = m_Parent;
            RectOffset b = host.borderSize;
            Rect hs = host.screenPosition;
            m_Pos = new Rect(hs.x + b.left, hs.y + b.top + b.bottom, hs.width - b.left - b.right, hs.height - b.top - b.bottom);

            float backing = host.GetBackingScaleFactor();
            Rect view = viewInWindow;
            m_ZoomArea.rect = view;
            // targetInContent = PixelsToPoints(Rect(-0.5 * target, target), backingScale): centred on 0
            var content = new Rect(-0.5f * targetPixels.x / backing, -0.5f * targetPixels.y / backing,
                                   targetPixels.x / backing, targetPixels.y / backing);
            m_ZoomArea.hBaseRangeMin = content.xMin;
            m_ZoomArea.vBaseRangeMin = content.yMin;
            m_ZoomArea.hBaseRangeMax = content.xMax;
            m_ZoomArea.vBaseRangeMax = content.yMax;
            // ScaleThatFitsTargetInView, never above 1 by default (DefaultScaleForTargetInView)
            float fit = Math.Min(view.width / content.width, view.height / content.height);
            float s = Math.Min(fit, 1f) * zoom;
            m_ZoomArea.m_Scale = new Vector2(s, s);
            // Content centred in the view (relative to drawRect, as GUI.BeginGroup(drawRect) has it).
            m_ZoomArea.m_Translation = new Vector2(view.width / 2f + pan.x, view.height / 2f + pan.y);
        }

        internal void Undrawn()
        {
            m_ZoomArea = new ZoomableArea();
        }
    }
}
