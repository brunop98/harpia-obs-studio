// The slider+spin pair behind every effect parameter.
//
// The three ways this kind of control goes wrong: the halves disagree, an edit
// echoes back and fights the drag, and clicking the groove pages towards the
// click instead of going there. All three are checked with real mouse events.
#include "editor/ParamSlider.hpp"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSlider>
#include <cstdio>
using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w) { std::printf("  %s %s\n", c?"PASS":"FAIL", w); if(!c) ++failures; }
static void eq(double got, double want, double tol, const char *w) {
    const bool g = std::abs(got-want) <= tol;
    std::printf("  %s %s (got %.4f, want %.4f)\n", g?"PASS":"FAIL", w, got, want);
    if (!g) ++failures;
}
static void clickAt(QSlider *s, int x) {
    const QPoint p(x, s->height()/2);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), s->mapToGlobal(p),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(s, &press);
    QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(p), s->mapToGlobal(p),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(s, &rel);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Saturation's real range, which is asymmetric -- a symmetric one would let
    // an off-by-half-a-range mapping bug pass.
    ParamSlider ps(-1.0, 2.0, 3);
    ps.resize(300, 28); ps.show();
    QApplication::processEvents();
    auto *sl = ps.findChild<QSlider *>();
    auto *sb = ps.findChild<QDoubleSpinBox *>();
    ok(sl && sb, "it has a slider and a spin box");
    if (!sl || !sb) return 1;

    std::printf("\n-- the two halves agree --\n");
    ps.setValue(0.5);
    eq(ps.value(), 0.5, 1e-9, "value() reports what was set");
    eq(sb->value(), 0.5, 1e-9, "the spin box shows it");
    // 0.5 sits halfway through -1..2.
    eq(sl->value(), 500, 1, "and the slider is at the matching position");

    ps.setValue(-1.0);
    eq(sl->value(), 0, 1, "the low end maps to the far left");
    ps.setValue(2.0);
    eq(sl->value(), 1000, 1, "the high end to the far right");

    std::printf("\n-- setValue does NOT emit; a user edit does --\n");
    {
        QSignalSpy spy(&ps, &ParamSlider::valueChanged);
        ps.setValue(0.25);
        ok(spy.count() == 0, "pushing a model value in emits nothing");
        sb->setValue(1.5);
        ok(spy.count() == 1, "typing emits once");
        eq(spy.count() ? spy.at(0).at(0).toDouble() : -99, 1.5, 1e-9, "with the typed value");
        eq(sl->value(), 833, 2, "and the slider follows");
    }

    std::printf("\n-- no echo: one edit is one signal --\n");
    {
        QSignalSpy spy(&ps, &ParamSlider::valueChanged);
        sl->setValue(200);
        // -1 + 0.2*3 = -0.4
        ok(spy.count() == 1, "moving the slider emits exactly once, not twice");
        eq(ps.value(), -0.4, 0.005, "and the spin box took the slider's value");
    }

    std::printf("\n-- out of range is clamped, not accepted --\n");
    ps.setValue(99.0);
    eq(ps.value(), 2.0, 1e-9, "above the top clamps to the top");
    ps.setValue(-99.0);
    eq(ps.value(), -1.0, 1e-9, "below the bottom clamps to the bottom");

    std::printf("\n-- clicking the groove goes THERE, not one page towards it --\n");
    {
        ps.setValue(-1.0);            // hard left
        const int before = sl->value();
        clickAt(sl, sl->width() * 3 / 4);
        const int after = sl->value();
        std::printf("     slider %d -> %d (of %d)\n", before, after, sl->maximum());
        // A paging click would move by one pageStep (100 of 1000). A jump lands
        // near three quarters.
        ok(after > 600, "one click reaches the far side rather than stepping");
    }
    std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
    return failures ? 1 : 0;
}
