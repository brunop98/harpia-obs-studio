// GameViewArea (where the game's picture is, in physical pixels) run against
// FakeUnityEditor.cs: Unity's internals as the reference source has them.
//
//   cd unity
//   mcs -out:GameViewAreaTests.exe Tests~/GameViewAreaTests.cs Tests~/FakeUnityEditor.cs \
//       HarpiaRecorder/Editor/GameViewArea.cs
//   mono GameViewAreaTests.exe
//
// Docked and maximized Game views, with and without the toolbar; Free Aspect,
// a letterboxed 1920x1080, zoomed in and out; 150% display scaling while the
// view that drew last was at 100% (the case GameView.targetInView gets wrong
// before Unity 6); a stale pane position; a Game view that has not drawn yet;
// the Device Simulator; none open; too small. The reflection must find every
// member where Unity declares it (base classes, the Vector2 overloads).
// Positions come from Unity's screen points here (no Windows): "estimated".

using System;
using Harpia.PlayModeRecorder;
using UnityEditor;
using UnityEngine;

static class GameViewAreaTests
{
    static int failures;

    static void Ok(bool c, string what)
    {
        Console.WriteLine("  " + (c ? "PASS " : "FAIL ") + what);
        if (!c)
            failures++;
    }

    // Within a pixel: the edges are rounded to whole pixels.
    static bool Near(PixelRect r, int x, int y, int w, int h)
    {
        return Math.Abs(r.X - x) <= 1 && Math.Abs(r.Y - y) <= 1 && Math.Abs(r.Width - w) <= 1 && Math.Abs(r.Height - h) <= 1;
    }

    static GameView Place(HostView host, float screenX, float screenY, float width, float height, float backing)
    {
        host.m_ScreenOrigin = new Vector2(screenX, screenY);
        host.position = new Rect(0, 0, width, height);
        host.m_BackingScale = backing;
        var gv = new GameView { m_Parent = host };
        PlayModeView.s_LastFocused = gv;
        return gv;
    }

    static GameViewMeasure Measure(RecordArea mode, string what)
    {
        GameViewMeasure m;
        string why;
        bool ok = GameViewArea.TryMeasure(mode, out m, out why);
        if (!ok)
            Console.WriteLine("    (" + what + ": " + why + ")");
        return m;
    }

    static int Main()
    {
        // The zoom path must be the one measuring: the fallback would say 640x480.
        Handles.GameViewSize = new Vector2(640, 480);
        EditorGUIUtility.pixelsPerPoint = 1f;

        Console.WriteLine("\n-- docked, 100% --");
        // The host at (100, 230) on screen, 800 x 624 points. Its pane starts at
        // (+1, +24+2) and is 798 x 598; the view under the 21-point toolbar is
        // 798 x 577 at (101, 277).
        var dock = new DockArea();
        GameView gv = Place(dock, 100, 230, 800, 624, 1f);
        gv.Draw(new Vector2(798, 577), 1f, Vector2.zero);
        GameViewMeasure m = Measure(RecordArea.GamePicture, "free aspect");
        Ok(Near(m.Area, 101, 277, 798, 577), "Free Aspect: the picture is the whole view under the toolbar: " + m.Area);
        Ok(m.GameWidth == 798 && m.GameHeight == 577 && m.PixelForPixel, "pixel for pixel, " + m.GameWidth + "x" + m.GameHeight);
        Ok(!m.Exact && m.Scaling == 1f, "position from Unity's points, scaling 100%");
        Ok(m.Describe().Contains("the game's full 798x577"), "described: " + m.Describe());

        gv.Draw(new Vector2(1920, 1080), 1f, Vector2.zero);
        m = Measure(RecordArea.GamePicture, "1080p");
        // fit = min(798/1920, 577/1080) = 0.4156: 798 x 448.9, centred: top 277 + 64.1
        Ok(Near(m.Area, 101, 341, 798, 449), "1920x1080 in it: letterboxed, the bars left out: " + m.Area);
        Ok(m.GameWidth == 1920 && m.GameHeight == 1080 && !m.PixelForPixel && Math.Abs(m.Shown - 0.4156f) < 0.001f,
           "the game renders 1920x1080, shown at 42%");
        Ok(m.Describe().Contains("shown at 42%"), "described: " + m.Describe());
        m = Measure(RecordArea.WholeGameView, "whole");
        Ok(Near(m.Area, 101, 277, 798, 577), "Whole Game View: bars included: " + m.Area);

        gv.Draw(new Vector2(798, 577), 2f, new Vector2(300, 0));
        m = Measure(RecordArea.GamePicture, "zoom in");
        Ok(Near(m.Area, 101, 277, 798, 577), "zoomed in 2x and panned: what shows is the view: " + m.Area);
        gv.Draw(new Vector2(798, 577), 0.5f, Vector2.zero);
        m = Measure(RecordArea.GamePicture, "zoom out");
        Ok(Near(m.Area, 301, 421, 399, 289), "zoomed out to 0.5x: the smaller picture, centred: " + m.Area);

        Console.WriteLine("\n-- 150% display scaling --");
        // The view that drew last was at 100%; the Game view's own is 150%.
        EditorGUIUtility.pixelsPerPoint = 1f;
        var dock150 = new DockArea { m_AtLeftEdge = true };
        // Host at (1000, 101) points = (1500, 151.5) px; view 1280 x 720 points at (0, 47) in it.
        gv = Place(dock150, 1000, 101, 1281, 767, 1.5f);
        gv.Draw(new Vector2(1920, 1080), 1f, Vector2.zero);
        m = Measure(RecordArea.GamePicture, "150%");
        Ok(Near(m.Area, 1500, 222, 1920, 1080), "1920x1080 at 150% fills a 1280x720-point view: " + m.Area);
        Ok(m.PixelForPixel && m.Scaling == 1.5f, "pixel for pixel, with the Game view's own scaling (not the last view's)");

        Console.WriteLine("\n-- other layouts --");
        gv = Place(new DockArea(), 100, 230, 800, 624, 1f);
        gv.m_ShowToolbar = false;
        gv.Draw(new Vector2(798, 598), 1f, Vector2.zero);
        m = Measure(RecordArea.GamePicture, "no toolbar");
        Ok(Near(m.Area, 101, 256, 798, 598), "no toolbar: the view starts under the tab: " + m.Area);

        gv = Place(new DockArea { m_BottomTab = true }, 100, 230, 800, 622, 1f);
        gv.Draw(new Vector2(798, 577), 1f, Vector2.zero);
        m = Measure(RecordArea.GamePicture, "bottom tab");
        Ok(Near(m.Area, 101, 275, 798, 577), "docked along the window's bottom (no bottom border): " + m.Area);

        gv = Place(new MaximizedHostView(), 0, 0, 1920, 1040, 1f);
        gv.Draw(new Vector2(1920, 1040 - 28 - 21), 1f, Vector2.zero);
        m = Measure(RecordArea.GamePicture, "maximized");
        Ok(Near(m.Area, 0, 49, 1920, 991), "Play Maximized: under its tab (24 + 4) and the toolbar: " + m.Area);

        gv = Place(new DockArea(), 100, 230, 800, 624, 1f);
        gv.Draw(new Vector2(798, 577), 1f, Vector2.zero);
        gv.position = new Rect(5000, 5000, 798, 598); // stale: from before a layout change
        m = Measure(RecordArea.GamePicture, "stale pane");
        Ok(Near(m.Area, 101, 277, 798, 577), "a stale pane position: placed by the borders instead: " + m.Area);

        gv = Place(new DockArea(), 100, 230, 800, 624, 1f);
        gv.Draw(new Vector2(798, 577), 1f, Vector2.zero);
        gv.Undrawn();
        Handles.GameViewSize = new Vector2(1920, 1080);
        m = Measure(RecordArea.GamePicture, "undrawn");
        Ok(Near(m.Area, 101, 341, 798, 449), "not drawn yet: the game's aspect, fitted: " + m.Area);
        Handles.GameViewSize = new Vector2(640, 480);

        Console.WriteLine("\n-- refusals --");
        string why;
        GameViewMeasure none;
        PlayModeView.s_LastFocused = new SimulatorWindow { titleContent = new GUIContent("Simulator") };
        Ok(!GameViewArea.TryMeasure(RecordArea.GamePicture, out none, out why) && why.Contains("Simulator"),
           "Play Mode in the Device Simulator: says so (" + why + ")");
        PlayModeView.s_LastFocused = null;
        Ok(!GameViewArea.TryMeasure(RecordArea.GamePicture, out none, out why) && why.Contains("no Game view"),
           "no Game view at all: says so");
        gv = Place(new DockArea(), 0, 0, 800, 624, 1f);
        gv.Draw(new Vector2(798, 577), 1f, Vector2.zero);
        PlayModeView.s_LastFocused = null;
        Resources.All.Add(gv);
        Ok(GameViewArea.TryMeasure(RecordArea.GamePicture, out none, out why), "never focused, but open: found anyway");
        Resources.All.Clear();
        gv = Place(new DockArea(), 0, 0, 30, 50, 1f);
        gv.Draw(new Vector2(28, 3), 1f, Vector2.zero);
        Ok(!GameViewArea.TryMeasure(RecordArea.GamePicture, out none, out why) && why.Contains("too small"),
           "a sliver: too small to record");

        Console.WriteLine("\n" + (failures > 0 ? "FAILURES" : "ALL PASSED (0 failures)"));
        return failures > 0 ? 1 : 0;
    }
}
