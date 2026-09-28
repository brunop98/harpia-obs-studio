// Large stills without the lag. What counts as heavy enough to warn about;
// the working copy and the preview copy, never enlarged and aspect kept; and
// the one thing that has to stay exactly right when the picture handed to the
// compositor is smaller than its source: a crop cuts the SAME part of it.
#include "editor/StillWeight.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QApplication>
#include <QPainter>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A 400x200 source, left half red, right half blue -- served at full size or
// as a half-size copy, always reporting the original's size.
class Frames : public TimelineCompositor::FrameProvider {
public:
	explicit Frames(double scale) : scale_(scale) {}
	QSize sourceSize(int) override { return QSize(400, 200); }
	QImage frameFor(int, qint64) override
	{
		const QSize sz(int(400 * scale_), int(200 * scale_));
		QImage im(sz, QImage::Format_RGBA8888);
		QPainter p(&im);
		p.fillRect(QRect(0, 0, sz.width() / 2, sz.height()), QColor(255, 0, 0));
		p.fillRect(QRect(sz.width() / 2, 0, sz.width() - sz.width() / 2, sz.height()), QColor(0, 0, 255));
		return im;
	}

private:
	double scale_;
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	std::printf("\n-- heavy or not --\n");
	{
		ok(!stillWeight(QSize(4032, 3024), 3 * 1024 * 1024).heavy, "a 12 MP phone photo is fine");
		ok(!stillWeight(QSize(3840, 2160), 20 * 1024 * 1024).heavy, "a 4K frame is fine");
		const StillWeight big = stillWeight(QSize(12000, 8000), 30 * 1024 * 1024);
		ok(big.heavy && big.why.contains(QStringLiteral("12000 x 8000")) && big.why.contains(QStringLiteral("96 megapixels")),
		   "a 96 MP image is heavy, and the message says how big");
		ok(big.memoryBytes == qint64(12000) * 8000 * 4, "and how much memory it takes decoded");
		const StillWeight fat = stillWeight(QSize(4000, 3000), 90ll * 1024 * 1024);
		ok(fat.heavy && fat.why.contains(QStringLiteral("90 MB file")), "a 90 MB file is heavy even at 12 MP");
		ok(!stillWeight(QSize(), 0).heavy, "an unknown size is not flagged");
	}

	std::printf("\n-- the copies --\n");
	{
		ok(stillWorkingSize(QSize(12000, 8000)) == QSize(4096, 2730), "the working copy is capped at 4096 on the long side");
		ok(stillWorkingSize(QSize(1920, 1080)) == QSize(1920, 1080), "and never enlarged");
		ok(stillWorkingSize(QSize(3000, 9000)).height() == 4096, "a tall image is capped on its height");
		ok(stillPreviewSize(QSize(4096, 2731), QSize(960, 540)) == QSize(1214, 810), "the preview copy: fits 1.5x the render size (1440x810), aspect kept");
		ok(stillPreviewSize(QSize(800, 600), QSize(960, 540)) == QSize(800, 600), "a small image is used as it is");
		ok(stillPreviewSize(QSize(4096, 2731), QSize()) == QSize(4096, 2731), "no render size: nothing to fit to");
	}

	std::printf("\n-- a crop on a smaller copy --\n");
	{
		ok(cropForFrame(QRect(200, 0, 200, 200), QSize(400, 200), QSize(200, 100)) == QRect(100, 0, 100, 100),
		   "the right half of the original is the right half of a half-size copy");
		ok(cropForFrame(QRect(10, 10, 50, 50), QSize(400, 200), QSize(400, 200)) == QRect(10, 10, 50, 50),
		   "same size: untouched");
		const QRect odd = cropForFrame(QRect(1, 1, 3, 3), QSize(400, 200), QSize(100, 50));
		ok(odd.width() >= 1 && odd.height() >= 1, "a tiny crop never shrinks to nothing");

		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip c;
		c.type = TlClip::Type::Image;
		c.sourceId = 1;
		c.srcEndMs = 1000;
		c.crop = QRect(200, 0, 200, 200); // the blue half, in the ORIGINAL's pixels
		t.clips.append(c);
		m.tracks.append(t);
		Frames full(1.0), half(0.5);
		const QImage a = TimelineCompositor::compose(m, 500, QSize(200, 200), full);
		const QImage b = TimelineCompositor::compose(m, 500, QSize(200, 200), half);
		const QColor ca = a.pixelColor(100, 100), cb = b.pixelColor(100, 100);
		ok(ca.blue() > 200 && ca.red() < 40, "full-size frame: the crop shows the blue half");
		ok(cb.blue() > 200 && cb.red() < 40, "half-size copy: still the blue half, filling the same place");
		ok(b.pixelColor(10, 100).blue() > 200 && b.pixelColor(190, 100).blue() > 200,
		   "edge to edge, like the full-size one");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
