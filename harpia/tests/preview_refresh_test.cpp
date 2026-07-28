// Removing a clip must repaint the preview.
//
// The case that showed it up is an effect clip: it grades every track under it,
// so deleting one should visibly change the picture at the playhead. Nothing
// re-rendered, so the old graded frame stayed on screen. Dragging and trimming
// hid the same gap because the mouse scrubs while it drags.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/EditorWidgets.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w) { std::printf("  %s %s\n", c?"PASS":"FAIL", w); if(!c) ++failures; }

// The preview is rendered through a paced timer, so give it real time to run
// rather than spinning processEvents (which advances no clock).
static void settle(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}
// setPlayhead() is a programmatic setter and emits no scrub, so it renders
// nothing. Clicking the ruler is what actually moves the playhead AND asks for
// a frame -- and it is what a user does.
static void scrubTo(TimelineView *tv, qint64 ms) {
    const int x = tv->xForMs(ms);
    const QPoint p(x, 14); // inside the time ruler, above the first lane
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), tv->mapToGlobal(p),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tv, &press);
    QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(p), tv->mapToGlobal(p),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tv, &rel);
}
static double meanLuma(const QImage &f) {
    if (f.isNull()) return -1;
    double s = 0; int n = 0;
    for (int y = 0; y < f.height(); y += 3)
        for (int x = 0; x < f.width(); x += 3) { s += QColor(f.pixel(x,y)).lightness(); ++n; }
    return n ? s / n : -1;
}
int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    const QString media = QStringLiteral(SRC_MEDIA);
    if (!QFile::exists(media)) { std::printf("missing %s\n", qPrintable(media)); return 2; }

    VideoEditorWindow w(media);
    w.resize(1400, 900); w.show();
    QApplication::processEvents();

    // The preview only composites in Full editing; in the other modes it shows
    // one source frame, and every measurement below would be identical.
    QPushButton *fullBtn = nullptr;
    for (QPushButton *b : w.findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Full Editing"))
            fullBtn = b;
    ok(fullBtn != nullptr, "the Full Editing mode button is there");
    if (fullBtn) { fullBtn->click(); settle(300); }

    TimelineView *tv = w.findChild<TimelineView *>();
    ok(tv != nullptr, "the timeline view exists");
    if (!tv) return 1;
    PreviewCanvas *pc = w.findChild<PreviewCanvas *>();
    ok(pc != nullptr, "the preview canvas exists");
    if (!pc) return 1;

    // A picture track, and an effect track over it holding one strong effect.
    TimelineModel m;
    // A TEXT clip as the picture, not a video clip: text renders through the
    // compositor with no decoder, so the test does not depend on guessing the
    // editor's internal source id (which is what made the first version
    // composite a black frame).
    TlTrack vid; vid.kind = TlTrack::Kind::Video; vid.name = QStringLiteral("V1");
    TlClip c; c.type = TlClip::Type::Text;
    c.srcStartMs = 0; c.srcEndMs = 3000; c.outStartMs = 0;
    c.text.text = QStringLiteral("HARPIA");
    c.text.fontPx = 220;
    c.text.boxOpacity = 1.0;             // a solid caption box fills the frame
    c.text.boxPadX = 900; c.text.boxPadY = 500;
    vid.clips.append(c);
    TlTrack fx; fx.kind = TlTrack::Kind::Effect; fx.name = QStringLiteral("FX");
    TlClip e; e.type = TlClip::Type::Effect; e.outStartMs = 0;
    e.srcStartMs = 0; e.srcEndMs = 3000;
    e.fx.type = FxType::Brightness; e.fx.enabled = true;
    e.fx.params = fxDefaults(FxType::Brightness);
    e.fx.params[QStringLiteral("amount")] = 0.9; // unmistakable
    fx.clips.append(e);
    // Index 0 is the top lane, and an effect track grades what is below it.
    m.tracks.append(fx);
    m.tracks.append(vid);
    tv->setModel(m);
    // setModel is a programmatic replace and emits no clipsChanged, so move the
    // playhead to force the first composite.
    scrubTo(tv, 1000);
    settle(700);
    const QImage graded = pc->currentFrame().copy();
    const double lumaGraded = meanLuma(graded);
    std::printf("     with the effect:    mean luma %.1f\n", lumaGraded);
    ok(!graded.isNull(), "the preview has a frame");

    // Sanity: the effect must actually be doing something, or a "changed"
    // assertion below would pass for the wrong reason.
    TimelineModel plain = m;
    plain.tracks.removeFirst();
    tv->setModel(plain);
    scrubTo(tv, 1000);
    settle(700);
    const double lumaPlain = meanLuma(pc->currentFrame());
    std::printf("     without the effect: mean luma %.1f\n", lumaPlain);
    ok(std::abs(lumaGraded - lumaPlain) > 8.0,
       "the effect visibly changes the frame, so this test can tell them apart");

    // Back to the graded state, then delete the effect clip the way a user
    // does: select it on the timeline and press Delete.
    tv->setModel(m);
    scrubTo(tv, 1000);
    settle(700);
    const double before = meanLuma(pc->currentFrame());
    std::printf("     restored:           mean luma %.1f\n", before);

    tv->selectClip(0, 0);          // the effect clip on the effect track
    tv->deleteSelected();
    settle(700);
    const double after = meanLuma(pc->currentFrame());
    std::printf("     after deleting it:  mean luma %.1f\n", after);
    ok(std::abs(after - before) > 8.0, "deleting the effect clip repaints the preview");
    ok(std::abs(after - lumaPlain) < 6.0, "and what is left matches the ungraded picture");

    std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
    return failures ? 1 : 0;
}
