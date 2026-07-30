// Seeing where a clip was split.
//
// Two clips that touch used to be drawn edge to edge: their 1px borders landed
// on the same pixel and the 4px corner radius left a notch about a pixel deep,
// so a split was, in practice, invisible. Worse, nothing distinguished a split
// from two unrelated pieces of footage that happened to abut -- and those are
// completely different edits.
//
// Both halves of the fix are easy to get subtly wrong in ways a screenshot
// would not settle:
//
//   - insetting the drawn rect but ALSO insetting the hit rect, which turns the
//     new gap into a dead strip that swallows clicks between two clips;
//   - marking every boundary as a split, so the mark means nothing;
//   - missing a real split because the clips are not stored in output order
//     (splitAtPlayhead appends the right half to the END of the track);
//   - claiming a split across a speed change or a source change, where the two
//     halves would NOT rejoin seamlessly.
#include "editor/timeline/TimelineModel.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>
#include <random>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TlClip media(int src, qint64 s, qint64 e, qint64 at)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = src;
	c.srcStartMs = s;
	c.srcEndMs = e;
	c.outStartMs = at;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- a split is recognised, an abutment is not --\n");
	{
		TlTrack t;
		// One clip cut at 2000: source time runs straight through the join.
		t.clips.append(media(1, 0, 2000, 0));
		t.clips.append(media(1, 2000, 5000, 2000));
		QVector<qint64> seams = t.splitSeams();
		std::printf("     split pair -> %d seam(s) at %lld\n", int(seams.size()),
			    seams.isEmpty() ? -1LL : (long long)seams.first());
		ok(seams.size() == 1 && seams.first() == 2000, "the cut is found, at the cut");

		// Two different sources that merely touch. Same geometry on screen,
		// entirely different edit -- and the mark must not claim otherwise.
		TlTrack u;
		u.clips.append(media(1, 0, 2000, 0));
		u.clips.append(media(2, 0, 3000, 2000));
		std::printf("     two sources touching -> %d seam(s)\n", int(u.splitSeams().size()));
		ok(u.splitSeams().isEmpty(), "two unrelated clips that touch are NOT a split");

		// Same source, but the right half starts somewhere else in it: this is
		// a jump cut, not a split, and rejoining would change the picture.
		TlTrack v;
		v.clips.append(media(1, 0, 2000, 0));
		v.clips.append(media(1, 9000, 11000, 2000));
		std::printf("     same source, discontinuous -> %d seam(s)\n",
			    int(v.splitSeams().size()));
		ok(v.splitSeams().isEmpty(), "a jump cut in one source is not a split either");

		// Continuous in source but retimed: the halves no longer form one clip.
		TlTrack w;
		w.clips.append(media(1, 0, 2000, 0));
		TlClip fast = media(1, 2000, 5000, 2000);
		fast.speed = 2.0;
		w.clips.append(fast);
		std::printf("     speed change at the join -> %d seam(s)\n",
			    int(w.splitSeams().size()));
		ok(w.splitSeams().isEmpty(), "a speed change at the join is not a seamless split");

		// A gap between them is a gap, however continuous the source is.
		TlTrack g;
		g.clips.append(media(1, 0, 2000, 0));
		g.clips.append(media(1, 2000, 5000, 2500));
		ok(g.splitSeams().isEmpty(), "and clips with a gap between them are not a split");
	}

	std::printf("\n-- found however the clips are stored --\n");
	{
		// splitAtPlayhead APPENDS the right half, so after two splits a track's
		// clips are in no particular order. A neighbour-by-neighbour sweep over
		// the vector as stored would miss them.
		TlTrack t;
		t.clips.append(media(1, 4000, 6000, 4000)); // the last third, stored first
		t.clips.append(media(1, 0, 2000, 0));
		t.clips.append(media(1, 2000, 4000, 2000));
		const QVector<qint64> seams = t.splitSeams();
		std::printf("     out-of-order track -> seams at");
		for (qint64 m : seams)
			std::printf(" %lld", (long long)m);
		std::printf("\n");
		ok(seams.size() == 2, "both cuts in a twice-split clip are found");
		ok(seams.size() == 2 && seams[0] == 2000 && seams[1] == 4000,
		   "and reported in time order, not storage order");
	}

	std::printf("\n-- stills and captions, which have no source clock --\n");
	{
		// splitAtPlayhead rewrites a still's src range outright, so source
		// continuity cannot be the test for these; same file is.
		TlTrack t;
		TlClip a;
		a.type = TlClip::Type::Image;
		a.sourceId = 7;
		a.srcStartMs = 0;
		a.srcEndMs = 2000;
		a.outStartMs = 0;
		TlClip b = a;
		b.srcStartMs = 0;
		b.srcEndMs = 3000;
		b.outStartMs = 2000;
		t.clips = {a, b};
		ok(t.splitSeams().size() == 1, "a split still is recognised");

		TlTrack u;
		TlClip c;
		c.type = TlClip::Type::Text;
		c.text.text = QStringLiteral("hello");
		c.srcEndMs = 1000;
		c.outStartMs = 0;
		TlClip d = c;
		d.outStartMs = 1000;
		u.clips = {c, d};
		ok(u.splitSeams().size() == 1, "so is a split caption");
		// Two captions with different words are two captions, not one cut in
		// half -- and both have sourceId 0, so only the text can say so.
		TlTrack v;
		TlClip e = c, f = d;
		f.text.text = QStringLiteral("goodbye");
		v.clips = {e, f};
		ok(v.splitSeams().isEmpty(), "two DIFFERENT captions touching are not a split");
	}

	std::printf("\n-- the gap is drawn, but is not a dead strip --\n");
	{
		TimelineView tv;
		tv.resize(900, 300);
		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		t.clips.append(media(1, 0, 4000, 0));
		t.clips.append(media(1, 4000, 8000, 4000));
		m.tracks.append(t);
		tv.setModel(m);
		tv.zoomToFit();
		tv.show();
		QApplication::processEvents();

		const QRect a = tv.clipRectForTest(0, 0), b = tv.clipRectForTest(0, 1);
		const QRect pa = tv.clipPaintRectForTest(0, 0), pb = tv.clipPaintRectForTest(0, 1);
		std::printf("     hit rects   %d..%d | %d..%d\n", a.left(), a.right(), b.left(),
			    b.right());
		std::printf("     paint rects %d..%d | %d..%d\n", pa.left(), pa.right(), pb.left(),
			    pb.right());
		// The valley: the drawn shapes must not touch.
		ok(pb.left() - pa.right() >= 2, "the drawn clips leave a gap between them");
		// And the click targets must: every x between them belongs to one clip
		// or the other, or the gap becomes a place drags mysteriously fail.
		ok(b.left() - a.right() <= 1, "the hit rects still meet, so nothing is unclickable");
		for (int x = pa.right(); x <= pb.left(); ++x) {
			const QPoint pt(x, a.center().y());
			if (!a.contains(pt) && !b.contains(pt)) {
				ok(false, "a pixel in the gap belonged to neither clip");
				break;
			}
		}
		ok(true, "every pixel across the gap still hits a clip");

		// The seam is actually painted, in the seam colour, in the gap.
		QImage shot(tv.size(), QImage::Format_RGB32);
		shot.fill(Qt::black);
		{
			QPainter p(&shot);
			tv.render(&p);
		}
		const int seamX = (pa.right() + pb.left() + 1) / 2;
		const int y = a.center().y();
		int seamish = 0;
		for (int dx = -1; dx <= 1; ++dx) {
			const QColor c = shot.pixelColor(seamX + dx, y);
			// The seam is a light cool grey; the clip body is a dark blue and
			// the bare lane darker still.
			if (c.red() > 100 && c.green() > 120 && c.blue() > 140)
				++seamish;
		}
		std::printf("     at the seam: rgb(%d,%d,%d)\n", shot.pixelColor(seamX, y).red(),
			    shot.pixelColor(seamX, y).green(), shot.pixelColor(seamX, y).blue());
		ok(seamish > 0, "a light seam line is painted between the halves");

		// And it is NOT painted where two unrelated clips touch, or the mark
		// would say nothing.
		TimelineModel m2;
		TlTrack t2;
		t2.kind = TlTrack::Kind::Video;
		t2.clips.append(media(1, 0, 4000, 0));
		t2.clips.append(media(2, 0, 4000, 4000));
		m2.tracks.append(t2);
		tv.setModel(m2);
		tv.zoomToFit();
		QApplication::processEvents();
		QImage shot2(tv.size(), QImage::Format_RGB32);
		shot2.fill(Qt::black);
		{
			QPainter p(&shot2);
			tv.render(&p);
		}
		int lit = 0;
		for (int dx = -1; dx <= 1; ++dx) {
			const QColor c = shot2.pixelColor(seamX + dx, y);
			if (c.red() > 100 && c.green() > 120 && c.blue() > 140)
				++lit;
		}
		std::printf("     at the same x with two SOURCES: rgb(%d,%d,%d)\n",
			    shot2.pixelColor(seamX, y).red(), shot2.pixelColor(seamX, y).green(),
			    shot2.pixelColor(seamX, y).blue());
		ok(lit == 0, "and no seam is painted where two unrelated clips merely touch");
	}

	std::printf("\n-- several clips ending at the same instant --\n");
	{
		// The bug this section exists for. splitSeams() indexes clips by their
		// END time to avoid an O(n^2) scan, and it used a plain QHash --
		// whose insert() REPLACES on a duplicate key. Two clips ending at the
		// same instant therefore left only the last of them in the index, and
		// when the one it kept was not the split's other half, the seam simply
		// was not drawn: a real split with no valley on it.
		//
		// Not a contrived arrangement. Clips overlap by design wherever there
		// is a transition, so coinciding end times are routine.
		TlTrack t;
		TlClip a; // the split's LEFT half: src 0..100
		a.type = TlClip::Type::Video;
		a.sourceId = 2;
		a.srcStartMs = 0;
		a.srcEndMs = 100;
		a.outStartMs = 400;
		TlClip decoy = a; // ends at the same instant, different source time
		decoy.srcStartMs = 100;
		decoy.srcEndMs = 200;
		TlClip b; // the RIGHT half: source runs straight on from `a`
		b.type = TlClip::Type::Video;
		b.sourceId = 2;
		b.srcStartMs = 100;
		b.srcEndMs = 500;
		b.outStartMs = 500;
		// `decoy` stored last, so a replacing hash keeps IT and misses the pair.
		t.clips = {a, decoy, b};
		const QVector<qint64> seams = t.splitSeams();
		std::printf("     seams: [");
		for (qint64 s : seams)
			std::printf(" %lld", (long long)s);
		std::printf(" ]\n");
		ok(seams.size() == 1 && seams[0] == 500,
		   "the seam is found even though another clip ends at the same time");

		// And it is reported once, not once per clip ending there.
		TlTrack t2;
		t2.clips = {a, decoy, b, b};
		std::printf("     with the right half duplicated: %d seam(s)\n",
			    int(t2.splitSeams().size()));
		ok(t2.splitSeams().size() == 1, "and reported once, however many clips coincide");
	}

	std::printf("\n-- the fast sweep agrees with the obvious slow version --\n");
	{
		// splitSeams() and overlapsBefore() are both one-pass rewrites of an
		// O(n^2) question, done because they run on every repaint and every
		// mouse-move. A rewrite like that is exactly where a silent behaviour
		// change hides -- the one above went unnoticed until this comparison
		// existed -- so both are checked against the obvious version they
		// replaced, over random tracks rather than hand-picked ones.
		std::mt19937 rng(12345);
		int seamBad = 0, overlapBad = 0;
		for (int iter = 0; iter < 4000; ++iter) {
			TlTrack t;
			const int n = 1 + int(rng() % 6);
			for (int i = 0; i < n; ++i) {
				TlClip c;
				c.type = TlClip::Type::Video;
				c.sourceId = 1 + int(rng() % 2);
				c.srcStartMs = qint64(rng() % 5) * 100;
				c.srcEndMs = c.srcStartMs + 100 + qint64(rng() % 5) * 100;
				c.outStartMs = qint64(rng() % 8) * 100;
				c.speed = (rng() % 4 == 0) ? 2.0 : 1.0;
				t.clips.append(c);
			}
			const QVector<qint64> fastOv = t.overlapsBefore();
			for (int i = 0; i < t.clips.size(); ++i)
				if (fastOv[i] != t.overlapBefore(i))
					++overlapBad;

			QVector<qint64> brute;
			for (int x = 0; x < t.clips.size(); ++x)
				for (int y = 0; y < t.clips.size(); ++y)
					if (x != y && TlTrack::isSplitPair(t.clips[x], t.clips[y]))
						brute.append(t.clips[y].outStartMs);
			std::sort(brute.begin(), brute.end());
			brute.erase(std::unique(brute.begin(), brute.end()), brute.end());
			if (brute != t.splitSeams())
				++seamBad;
		}
		std::printf("     4000 random tracks: %d seam, %d overlap disagreement(s)\n", seamBad,
			    overlapBad);
		ok(seamBad == 0, "splitSeams() matches a pairwise scan on every one");
		ok(overlapBad == 0, "and overlapsBefore() matches overlapBefore()");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
