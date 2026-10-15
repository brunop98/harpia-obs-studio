// Linked clips (TlClip::link) move as one: a text-to-speech narration and its
// subtitles. Checked in the real timeline widget.
//
//   * Clicking any member selects the whole group; an unlinked clip stays
//     a selection of one.
//   * Dragging one member moves every member by the same amount, on their
//     own lanes; the clip that is not linked stays where it was.
//   * A nudge (arrow keys) moves the group too.
//   * Pasting a copy of the group links the copies to each other, not to
//     the originals.
//   * The link survives saving and loading the project.
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QMouseEvent>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void press(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
static void moveTo(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton, Qt::LeftButton,
		      Qt::ShiftModifier); // no magnet: the maths is the maths
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), Qt::LeftButton, Qt::NoButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

// A Subtitles lane with two captions and a video lane with an unrelated clip
// above an audio lane with the narration. The narration and the two captions
// are linked (id 1).
static TimelineModel narrated()
{
	TimelineModel m;
	TlTrack subs;
	subs.kind = TlTrack::Kind::Video;
	subs.name = QStringLiteral("Subtitles");
	for (int i = 0; i < 2; ++i) {
		TlClip c;
		c.type = TlClip::Type::Text;
		c.text.text = i == 0 ? QStringLiteral("Hello there") : QStringLiteral("and welcome");
		c.srcEndMs = 1500;
		c.outStartMs = 2000 + i * 1500;
		c.link = 1;
		subs.clips.append(c);
	}
	TlTrack video;
	video.kind = TlTrack::Kind::Video;
	TlClip v;
	v.sourceId = 3;
	v.srcEndMs = 12000;
	video.clips.append(v);
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio;
	TlClip a;
	a.sourceId = 9;
	a.srcEndMs = 3000;
	a.outStartMs = 2000;
	a.link = 1;
	audio.clips.append(a);
	m.tracks = {subs, video, audio};
	return m;
}

static qint64 at(const TimelineView &v, int t, int c)
{
	return v.model().tracks[t].clips[c].outStartMs;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	TimelineView v;
	v.resize(900, 260);
	v.show();
	QApplication::processEvents();
	v.setModel(narrated());
	QApplication::processEvents();

	std::printf("\n-- the model --\n");
	ok(v.model().linkedClips(1).size() == 3, "three clips share link 1");
	ok(v.model().linkedClips(0).isEmpty(), "link 0 is no group");
	ok(v.model().nextLinkId() == 2, "a new group gets id 2");

	std::printf("\n-- clicking one selects the group --\n");
	{
		const QRect r = v.clipRectForTest(0, 1); // the second caption
		press(&v, r.center());
		release(&v, r.center());
		QApplication::processEvents();
		ok(v.selectedPairs().size() == 3, "the caption, the other caption and the narration");
		const QRect u = v.clipRectForTest(1, 0); // the unrelated video
		press(&v, u.center());
		release(&v, u.center());
		ok(v.selectedPairs().size() == 1, "an unlinked clip: just itself");
	}

	std::printf("\n-- dragging one moves all --\n");
	{
		const qint64 s0 = at(v, 0, 0), s1 = at(v, 0, 1), a0 = at(v, 2, 0), v0 = at(v, 1, 0);
		// Grab a caption (the middle of an audio clip is its volume line,
		// which is a different gesture).
		const QRect r = v.clipRectForTest(0, 0);
		const QPoint from = r.center();
		press(&v, from);
		for (int dx = 20; dx <= 100; dx += 20)
			moveTo(&v, from + QPoint(dx, 0));
		release(&v, from + QPoint(100, 0));
		QApplication::processEvents();
		const qint64 d = at(v, 2, 0) - a0;
		std::printf("     caption moved %lld ms; the other caption %lld, the narration %lld; video %lld\n",
			    (long long)(at(v, 0, 0) - s0), (long long)(at(v, 0, 1) - s1), (long long)d,
			    (long long)(at(v, 1, 0) - v0));
		ok(at(v, 0, 0) - s0 > 0, "the caption moved");
		ok(at(v, 0, 1) - s1 == at(v, 0, 0) - s0 && d == at(v, 0, 0) - s0,
		   "the other caption and the narration moved with it, by the same amount");
		ok(at(v, 1, 0) == v0, "the video, not linked, stayed");
	}

	std::printf("\n-- a nudge moves the group --\n");
	{
		const QRect r = v.clipRectForTest(0, 0);
		press(&v, r.center());
		release(&v, r.center());
		const qint64 a0 = at(v, 2, 0), s1 = at(v, 0, 1);
		v.nudgeSelection(500);
		ok(at(v, 2, 0) - a0 == 500 && at(v, 0, 1) - s1 == 500, "every member by 500 ms");
	}

	std::printf("\n-- a pasted copy is a group of its own --\n");
	{
		const auto copied = v.copySelection();
		ok(copied.size() == 3, "the group copied");
		v.pasteAt(copied, 20000);
		const auto orig = v.model().linkedClips(1);
		int copyLink = 0;
		int copies = 0;
		for (const TlTrack &t : v.model().tracks)
			for (const TlClip &c : t.clips)
				if (c.outStartMs >= 20000 && c.link != 0) {
					copyLink = c.link;
					++copies;
				}
		ok(orig.size() == 3, "the originals are still three");
		ok(copies == 3 && copyLink != 1 && v.model().linkedClips(copyLink).size() == 3,
		   "the three copies share a new link");
	}

	std::printf("\n-- saved and loaded --\n");
	{
		QJsonObject root;
		timelineToProject(v.model(), root);
		const TimelineModel back = timelineFromProject(root);
		ok(back.linkedClips(1).size() == 3, "the link is in the project file");
		ok(back == v.model(), "and the model comes back equal");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
