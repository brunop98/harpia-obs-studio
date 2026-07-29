// A track's controls must stay inside the gutter, whatever its width.
//
// The header's lock/hide/mute toggles are laid out at fixed offsets from the
// gutter's left edge. Set the gutter narrower than they need — the Dev panel
// lets you — and they are not squeezed: the rightmost one is pushed out into
// the content area, where the clips are painted afterwards and cover it. The
// Mute toggle simply disappeared under the first clip's thumbnail.
//
// Two things are checked, because either alone is weak:
//
//   - the GEOMETRY, over every gutter width the Dev panel can produce. A pixel
//     test only ever exercises the one width it happens to run at.
//   - the PIXELS, so "the rect is inside the gutter" is not satisfied by a
//     layout that is right on paper while something still paints over it.
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QImage>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A loud filmstrip: pure magenta, which nothing else in the timeline uses. Any
// of it over a toggle is the bug.
static QVector<QImage> magentaThumbs(int n)
{
	QVector<QImage> out;
	for (int i = 0; i < n; ++i) {
		QImage t(80, 45, QImage::Format_RGBA8888);
		t.fill(QColor(255, 0, 255));
		out.append(t);
	}
	return out;
}
static bool hasMagenta(const QImage &img, const QRect &r)
{
	const QRect c = r.intersected(img.rect());
	for (int y = c.top(); y <= c.bottom(); ++y)
		for (int x = c.left(); x <= c.right(); ++x) {
			const QColor p = img.pixelColor(x, y);
			if (p.red() > 200 && p.blue() > 200 && p.green() < 90)
				return true;
		}
	return false;
}

static void buildOneTrack(TimelineView &v)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video; // video has all three toggles: the widest case
	t.name = QStringLiteral("V1");
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 7;
	c.srcStartMs = 0;
	c.srcEndMs = 26900;
	c.outStartMs = 0;
	t.clips.append(c);
	m.tracks.append(t);
	v.setModel(m);
	v.setSourceThumbs(7, magentaThumbs(10), 26900);
	QApplication::processEvents();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	TimelineView v;
	v.resize(430, 200);
	v.show();
	QApplication::processEvents();
	buildOneTrack(v);

	std::printf("\n-- the toggles stay in the gutter at every width --\n");
	{
		// The Dev panel's whole range, not just the default. A floor that only
		// holds at 124 is not a floor.
		int worst = -1, worstAt = 0;
		for (int w = 20; w <= 300; w += 2) {
			TimelineViewParams lp = v.layoutParams();
			lp.gutterW = w;
			v.setLayoutParams(lp);
			const QRect mute = v.headerToggleRectForTest(0, 2);
			const int edge = v.contentRectForTest().x();
			const int over = mute.right() - (edge - 1);
			if (over > worst) {
				worst = over;
				worstAt = w;
			}
		}
		std::printf("     worst overhang past the gutter: %d px (at gutterW=%d)\n", worst,
			    worstAt);
		ok(worst <= 0, "the Mute toggle never reaches into the content area");
	}

	std::printf("\n-- and nothing paints over them --\n");
	{
		// A width that genuinely overlaps. Picked by arithmetic, not by feel:
		// the Mute toggle ends at margin + 8 + 2*(16+4) + 16, so anything below
		// that puts it under the clip. 66 was NOT enough -- the toggle ended at
		// 69 and the content began at 72 -- and the check passed on the broken
		// build until this was worked out.
		TimelineViewParams lp = v.layoutParams();
		lp.gutterW = 50;
		v.setLayoutParams(lp);
		QApplication::processEvents();

		QImage buf(v.size(), QImage::Format_RGB32);
		v.render(&buf);

		const QRect content = v.contentRectForTest();
		std::printf("     asked for gutterW=50, content starts at x=%d\n", content.x());
		ok(hasMagenta(buf, content), "the filmstrip is drawn (so this proves something)");

		for (int slot = 0; slot < 3; ++slot) {
			const QRect r = v.headerToggleRectForTest(0, slot);
			const char *name = slot == 0 ? "Lock" : (slot == 1 ? "Hide" : "Mute");
			const bool covered = hasMagenta(buf, r);
			std::printf("     %-5s at x=%d..%d  covered: %s\n", name, r.left(), r.right(),
				    covered ? "YES" : "no");
			ok(!covered, QByteArray(name).append(" is not painted over").constData());
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
