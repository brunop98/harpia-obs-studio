// The preview's caches must be invisible: faster, never different.
//
// The finished-frame cache is keyed by timelineFrameSignature, so the rules
// that make it safe are rules about that key: the same timeline gives the same
// key; an edit to a clip on screen, a hidden track, another time, size or
// generation gives a different one; an edit to a clip somewhere else on the
// timeline does not (that is what lets the cache survive editing).
//
// The caption bitmap is checked against the direct drawing path on the same
// frame: a painter with a transform takes the direct path, so drawing the
// same caption both ways and comparing pixels shows the bitmap matches.
#include "editor/timeline/FrameSignature.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>

#include <cstdio>
#include <cstdlib>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TimelineModel sample()
{
	TimelineModel m;
	TlTrack subs;
	for (int i = 0; i < 50; ++i) {
		TlClip c;
		c.type = TlClip::Type::Text;
		c.outStartMs = i * 1000;
		c.srcEndMs = 900;
		c.text.text = QStringLiteral("caption %1").arg(i);
		subs.clips.append(c);
	}
	TlTrack v1;
	TlClip v;
	v.type = TlClip::Type::Video;
	v.sourceId = 1;
	v.srcEndMs = 60000;
	v1.clips.append(v);
	TlTrack music;
	music.kind = TlTrack::Kind::Audio;
	TlClip a;
	a.type = TlClip::Type::Video;
	a.sourceId = 2;
	a.srcEndMs = 60000;
	music.clips.append(a);
	m.tracks = {subs, v1, music};
	return m;
}

static int maxDiff(const QImage &a0, const QImage &b0)
{
	const QImage a = a0.convertToFormat(QImage::Format_RGBA8888), b = b0.convertToFormat(QImage::Format_RGBA8888);
	int mx = 0;
	for (int y = 0; y < a.height(); ++y) {
		const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y);
		for (int x = 0; x < a.width() * 4; ++x)
			mx = std::max(mx, std::abs(pa[x] - pb[x]));
	}
	return mx;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	const QSize r(960, 540), L(1920, 1080);

	std::printf("\n-- the finished-frame key --\n");
	{
		const TimelineModel m = sample();
		const QByteArray k = timelineFrameSignature(m, 5100, r, L, 1);
		ok(k.size() == 16 && k == timelineFrameSignature(sample(), 5100, r, L, 1), "the same timeline, the same key");
		ok(k != timelineFrameSignature(m, 5133, r, L, 1), "another instant, another key");
		ok(k != timelineFrameSignature(m, 5100, QSize(480, 270), L, 1), "another preview size, another key");
		ok(k != timelineFrameSignature(m, 5100, r, L, 2), "a new generation (media, scripts, shaders), another key");

		TimelineModel far = m;
		far.tracks[0].clips[30].text.text = QStringLiteral("edited far away");
		ok(k == timelineFrameSignature(far, 5100, r, L, 1), "editing a caption at 0:30 keeps the key at 0:05");
		TimelineModel near = m;
		near.tracks[0].clips[5].text.color = QColor(255, 0, 0);
		ok(k != timelineFrameSignature(near, 5100, r, L, 1), "restyling the caption on screen changes it");
		TimelineModel moved = m;
		moved.tracks[1].clips[0].posX = 0.4;
		ok(k != timelineFrameSignature(moved, 5100, r, L, 1), "moving the video on screen changes it");
		TimelineModel comp = m;
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.blur");
		ci.instanceId = QStringLiteral("b");
		comp.tracks[1].clips[0].components.append(ci);
		ok(k != timelineFrameSignature(comp, 5100, r, L, 1), "adding an effect to it changes it");
		TimelineModel hidden = m;
		hidden.tracks[1].hidden = true;
		ok(k != timelineFrameSignature(hidden, 5100, r, L, 1), "hiding a track changes it");
		TimelineModel audio = m;
		audio.tracks[2].clips[0].volume = 0.2;
		ok(k == timelineFrameSignature(audio, 5100, r, L, 1), "a music level has no picture, and keeps it");

		QElapsedTimer t;
		t.start();
		for (int i = 0; i < 1000; ++i)
			timelineFrameSignature(m, 5100 + i, r, L, 1);
		const double us = t.nsecsElapsed() / 1e3 / 1000;
		std::printf("     %.1f us per key\n", us);
		ok(us < 2000.0, "and working it out is far cheaper than drawing a frame");
	}

	std::printf("\n-- a caption from the bitmap cache matches one drawn directly --\n");
	{
		for (int variant = 0; variant < 3; ++variant) {
			TlClip c;
			c.type = TlClip::Type::Text;
			c.srcEndMs = 1000;
			c.text.text = variant == 1 ? QStringLiteral("Two lines\nwith a box") : QStringLiteral("Hello, captions");
			c.text.fontPx = 48 + variant * 20;
			c.text.boxEnabled = variant == 1;
			c.text.outlineWidth = variant == 2 ? 6.0 : 3.0;
			TlTransform tf;
			tf.posX = 0.4137;
			tf.posY = 0.6219;
			tf.scale = variant == 2 ? 1.3 : 1.0;
			QImage a(960, 540, QImage::Format_RGBA8888), b(960, 540, QImage::Format_RGBA8888);
			a.fill(QColor(40, 80, 120));
			b.fill(QColor(40, 80, 120));
			{
				QPainter p(&a); // identity: the bitmap path
				TimelineCompositor::drawClip(p, c, tf, QSize(960, 540), QImage());
			}
			{
				QPainter p(&b); // a (net zero) transform: the direct path
				p.translate(10.0, 0.0);
				p.translate(-10.0, 0.0);
				p.translate(0.0, 0.0);
				QTransform tr = p.transform();
				tr.translate(1e-9, 0); // not exactly identity
				p.setTransform(tr);
				TimelineCompositor::drawClip(p, c, tf, QSize(960, 540), QImage());
			}
			const int d = maxDiff(a, b);
			std::printf("     caption %d: largest channel difference %d/255\n", variant, d);
			ok(d <= 10, variant == 0 ? "plain caption: the same picture"
				   : variant == 1 ? "two lines on a box: the same picture"
						  : "scaled, thick outline: the same picture");
		}
		// And a turned caption still draws (the direct path), not nothing.
		TlClip c;
		c.type = TlClip::Type::Text;
		c.srcEndMs = 1000;
		c.text.text = QStringLiteral("Turned");
		TlTransform tf;
		tf.rotation = 20.0;
		QImage img(480, 270, QImage::Format_RGBA8888);
		img.fill(Qt::black);
		{
			QPainter p(&img);
			TimelineCompositor::drawClip(p, c, tf, QSize(480, 270), QImage());
		}
		int lit = 0;
		for (int y = 0; y < img.height(); ++y)
			for (int x = 0; x < img.width(); ++x)
				lit += qGray(img.pixel(x, y)) > 128;
		ok(lit > 100, "a turned caption is drawn directly, as before");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
