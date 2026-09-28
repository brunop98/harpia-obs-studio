// Styling every caption on a lane at once. With the Subtitles lane's header
// selected, the Inspector's Text controls go through editLaneCaptions: every
// text clip on that lane gets the change, nothing else on it does, a locked
// lane is left alone, and a dragged slider stays ONE undo step (clipsChanged,
// not editCommitted, exactly like a single caption's spin box).
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TlClip caption(qint64 at, const QString &words)
{
	TlClip c;
	c.type = TlClip::Type::Text;
	c.outStartMs = at;
	c.srcEndMs = 800;
	c.text.text = words;
	return c;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	TimelineModel m;
	TlTrack subs;
	subs.name = QStringLiteral("Subtitles");
	subs.clips = {caption(0, QStringLiteral("Era uma vez")), caption(1000, QStringLiteral("uma galinha")),
		      caption(2000, QStringLiteral("feliz"))};
	TlClip still; // a non-caption on the same lane must not be restyled
	still.type = TlClip::Type::Video;
	still.outStartMs = 3000;
	still.srcEndMs = 500;
	subs.clips.append(still);
	TlTrack v1;
	v1.name = QStringLiteral("V1");
	v1.clips = {caption(0, QStringLiteral("other lane"))};
	m.tracks = {subs, v1};

	TimelineView view;
	view.setModel(m);
	QSignalSpy changed(&view, &TimelineView::clipsChanged);
	QSignalSpy committed(&view, &TimelineView::editCommitted);

	std::printf("\n-- every caption on the lane --\n");
	ok(view.laneCaptionCount(0) == 3 && view.laneCaptionCount(1) == 1 && view.laneCaptionCount(9) == 0,
	   "counts the captions on a lane, and 0 for a bad index");
	const int n = view.editLaneCaptions(0, [](TlClip &c) {
		c.text.fontPx = 90;
		c.text.color = QColor(255, 220, 0);
		c.posY = 0.2;
	});
	const auto &t = view.model().tracks;
	ok(n == 3, "three captions changed");
	ok(t[0].clips[0].text.fontPx == 90 && t[0].clips[1].text.fontPx == 90 && t[0].clips[2].text.fontPx == 90 &&
		   t[0].clips[2].text.color == QColor(255, 220, 0) && t[0].clips[1].posY == 0.2,
	   "each got the new size, colour and position");
	ok(t[0].clips[0].text.text == QStringLiteral("Era uma vez") && t[0].clips[2].text.text == QStringLiteral("feliz"),
	   "and each kept its own words");
	ok(t[0].clips[3].posY != 0.2, "the video clip on that lane was left alone");
	ok(t[1].clips[0].text.fontPx != 90, "and so was the caption on the other lane");
	ok(changed.count() == 1 && committed.count() == 0,
	   "one clipsChanged and no commit: a dragged slider coalesces into one undo step");

	std::printf("\n-- a locked lane --\n");
	TimelineModel locked = view.model();
	locked.tracks[0].locked = true;
	view.setModel(locked);
	changed.clear();
	ok(view.editLaneCaptions(0, [](TlClip &c) { c.text.fontPx = 12; }) == 0 &&
		   view.model().tracks[0].clips[0].text.fontPx == 90 && changed.isEmpty(),
	   "is not restyled, and says nothing changed");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
