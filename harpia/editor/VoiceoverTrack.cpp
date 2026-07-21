#include "VoiceoverTrack.hpp"

#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace harpia {

namespace {
constexpr int kMargin = 8;
constexpr int kCaptionH = 16;
constexpr int kTrackH = 40;
constexpr int kMinClipW = 6;

const QColor kBarBg(0x20, 0x22, 0x25);
const QColor kBarBorder(0x30, 0x33, 0x38);
const QColor kCaption(0x9a, 0x9f, 0xa8);
const QColor kClipFill(0x2c, 0x50, 0x45);
const QColor kClipFillSel(0x37, 0x74, 0x63);
const QColor kWave(0x6f, 0xd0, 0xb0);
const QColor kAccent(0x00, 0xae, 0xef);
const QColor kPlayhead(0xe5, 0x48, 0x4d);
} // namespace

VoiceoverTrack::VoiceoverTrack(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(QStringLiteral("Voiceover — drag a clip to move it, Del to remove"));
}

QSize VoiceoverTrack::sizeHint() const
{
	return QSize(480, minimumSizeHint().height());
}

QSize VoiceoverTrack::minimumSizeHint() const
{
	return QSize(240, 2 * kMargin + kCaptionH + kTrackH);
}

void VoiceoverTrack::setOutputDuration(qint64 ms)
{
	outputMs_ = std::max<qint64>(0, ms);
	update();
}

QVector<float> VoiceoverTrack::loadPeaks(const QString &path, int buckets)
{
	QVector<float> peaks;
	if (buckets <= 0)
		return peaks;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return peaks;
	QByteArray all = f.readAll();
	f.close();
	if (all.size() <= 44)
		return peaks;

	// Parse the minimal WAV header we wrote (canonical 44-byte PCM).
	auto u16 = [&](int off) { return quint16((quint8)all[off] | ((quint8)all[off + 1] << 8)); };
	const int channels = std::max<int>(1, u16(22));
	const int bits = u16(34);
	if (bits != 16)
		return peaks; // only the 16-bit PCM we record

	const char *pcm = all.constData() + 44;
	const qint64 pcmBytes = all.size() - 44;
	const qint64 frames = pcmBytes / (2 * channels); // one frame = all channels
	if (frames <= 0)
		return peaks;

	peaks.resize(buckets);
	const auto *s = reinterpret_cast<const qint16 *>(pcm);
	for (int b = 0; b < buckets; ++b) {
		const qint64 f0 = frames * b / buckets;
		const qint64 f1 = std::max<qint64>(f0 + 1, frames * (b + 1) / buckets);
		int peak = 0;
		for (qint64 fr = f0; fr < f1 && fr < frames; ++fr) {
			// Max across channels for this frame.
			for (int c = 0; c < channels; ++c) {
				const int v = std::abs((int)s[fr * channels + c]);
				peak = std::max(peak, v);
			}
		}
		peaks[b] = float(std::clamp(peak / 32768.0, 0.0, 1.0));
	}
	return peaks;
}

int VoiceoverTrack::addClip(VoiceoverClip clip)
{
	if (clip.peaks.isEmpty() && !clip.path.isEmpty())
		clip.peaks = loadPeaks(clip.path, 600);
	clips_.append(std::move(clip));
	selected_ = clips_.size() - 1;
	emit clipsChanged();
	emit clipSelected(selected_);
	update();
	return selected_;
}

void VoiceoverTrack::removeSelected()
{
	if (selected_ < 0 || selected_ >= clips_.size())
		return;
	clips_.remove(selected_);
	selected_ = -1;
	emit clipsChanged();
	emit clipSelected(-1);
	update();
}

void VoiceoverTrack::clearAll()
{
	clips_.clear();
	selected_ = -1;
	emit clipsChanged();
	emit clipSelected(-1);
	update();
}

void VoiceoverTrack::setPlayhead(qint64 outMs)
{
	playheadMs_ = outMs;
	update();
}

void VoiceoverTrack::clearPlayhead()
{
	playheadMs_ = -1;
	update();
}

QRect VoiceoverTrack::trackRect() const
{
	return QRect(kMargin, kMargin + kCaptionH, width() - 2 * kMargin, kTrackH);
}

int VoiceoverTrack::msToX(qint64 ms) const
{
	const QRect r = trackRect();
	if (outputMs_ <= 0)
		return r.x();
	return r.x() + int(double(ms) / double(outputMs_) * r.width());
}

qint64 VoiceoverTrack::xToMs(int x) const
{
	const QRect r = trackRect();
	if (outputMs_ <= 0 || r.width() <= 0)
		return 0;
	const double t = double(x - r.x()) / double(r.width());
	return std::clamp<qint64>(qint64(std::llround(t * outputMs_)), 0, outputMs_);
}

QVector<QRect> VoiceoverTrack::clipRects() const
{
	QVector<QRect> rects;
	const QRect r = trackRect();
	for (const VoiceoverClip &c : clips_) {
		const int x1 = msToX(c.outStartMs);
		const int x2 = msToX(c.outStartMs + c.durationMs);
		rects.append(QRect(x1, r.y() + 2, std::max(kMinClipW, x2 - x1), r.height() - 4));
	}
	return rects;
}

int VoiceoverTrack::clipAt(const QPoint &p) const
{
	const QVector<QRect> rects = clipRects();
	// Topmost last-added wins when clips overlap.
	for (int i = rects.size() - 1; i >= 0; --i) {
		if (rects[i].contains(p))
			return i;
	}
	return -1;
}

void VoiceoverTrack::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QFont capFont = font();
	capFont.setPixelSize(11);
	p.setFont(capFont);

	const QRect r = trackRect();

	p.setPen(kCaption);
	p.drawText(QRect(r.x(), kMargin, r.width(), kCaptionH), Qt::AlignVCenter | Qt::AlignLeft,
		   QStringLiteral("Voiceover"));

	p.setPen(kBarBorder);
	p.setBrush(kBarBg);
	p.drawRoundedRect(r, 5, 5);

	if (clips_.isEmpty()) {
		p.setPen(kCaption);
		p.drawText(r, Qt::AlignCenter,
			   QStringLiteral("Record narration to add it here"));
	}

	const QVector<QRect> rects = clipRects();
	for (int i = 0; i < rects.size(); ++i) {
		const QRect cr = rects[i];
		const bool sel = (i == selected_);
		QPainterPath clip;
		clip.addRoundedRect(cr, 4, 4);
		p.setPen(Qt::NoPen);
		p.setBrush(sel ? kClipFillSel : kClipFill);
		p.drawPath(clip);

		// Waveform centered vertically.
		const QVector<float> &pk = clips_[i].peaks;
		if (!pk.isEmpty() && cr.width() > 2) {
			p.save();
			p.setClipPath(clip);
			p.setPen(QPen(kWave, 1));
			const int midY = cr.center().y();
			const int halfH = cr.height() / 2 - 2;
			for (int x = cr.left() + 1; x < cr.right() - 1; ++x) {
				const double t = double(x - cr.left()) / std::max(1, cr.width());
				const int b = std::clamp<int>(int(t * pk.size()), 0, int(pk.size()) - 1);
				const int h = int(pk[b] * halfH);
				p.drawLine(x, midY - h, x, midY + h);
			}
			p.restore();
		}

		p.setPen(sel ? QPen(kAccent, 2) : QPen(kBarBorder, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(cr, 4, 4);
	}

	// Output playhead.
	if (playheadMs_ >= 0 && outputMs_ > 0) {
		const int px = msToX(playheadMs_);
		p.setPen(QPen(kPlayhead, 2));
		p.drawLine(px, r.top() + 1, px, r.bottom() - 1);
	}
}

void VoiceoverTrack::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	const int idx = clipAt(e->pos());
	if (idx != selected_) {
		selected_ = idx;
		emit clipSelected(idx);
		update();
	}
	if (idx >= 0) {
		mode_ = Mode::Moving;
		pressPos_ = e->pos();
		dragOrigStart_ = clips_[idx].outStartMs;
		dragMoved_ = false;
	}
}

void VoiceoverTrack::mouseMoveEvent(QMouseEvent *e)
{
	if (mode_ == Mode::Moving && selected_ >= 0 && (e->buttons() & Qt::LeftButton)) {
		if (!dragMoved_ && std::abs(e->pos().x() - pressPos_.x()) > 3)
			dragMoved_ = true;
		if (dragMoved_) {
			const qint64 deltaMs = xToMs(e->pos().x()) - xToMs(pressPos_.x());
			const qint64 maxStart = std::max<qint64>(0, outputMs_ - clips_[selected_].durationMs);
			clips_[selected_].outStartMs =
				std::clamp<qint64>(dragOrigStart_ + deltaMs, 0, maxStart);
			update();
		}
	} else {
		setCursor(clipAt(e->pos()) >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
	}
}

void VoiceoverTrack::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	if (mode_ == Mode::Moving && dragMoved_)
		emit clipsChanged();
	mode_ = Mode::None;
	dragMoved_ = false;
}

void VoiceoverTrack::keyPressEvent(QKeyEvent *e)
{
	if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && selected_ >= 0) {
		removeSelected();
		return;
	}
	QWidget::keyPressEvent(e);
}

} // namespace harpia
