#pragma once

// What an Audio Only recording looks like in the library, where every other
// entry has a picture.
//
// A frame cannot be decoded out of an .m4a, so the card draws a waveform
// instead. It is not the file's real waveform -- reading one would mean
// decoding the whole recording to build a thumbnail nobody asked for -- but it
// is derived from the file's own path, so a given recording always looks the
// same, and two recordings side by side do not look like copies of each other.
// The point is a card that is recognisably audio and recognisably THIS audio,
// not a measurement.
//
// The bar heights are pure and live in the header so audiotap_test can pin the
// properties that matter: stable for a path, in range, and different across
// files.

#include <QString>
#include <QVector>

#include <cmath>
#include <cstdint>

class QPainter;
class QRect;
class QImage;
class QSize;

namespace harpia {

// Bar heights for the card's waveform, each 0.15..1.0 of the available height.
// Deterministic in `seed` (the file path): the same recording draws the same
// shape in the recent strip, in the library, and after a restart.
//
// The floor is 0.15 rather than 0: a bar of height zero reads as a gap in the
// recording, which would be a claim about the audio that this function is in no
// position to make.
inline QVector<float> audioCardBars(const QString &seed, int count)
{
	QVector<float> bars;
	if (count <= 0)
		return bars;
	bars.reserve(count);

	// A small xorshift, seeded from the path. Chosen over qHash so the shape
	// cannot change between Qt versions or runs -- qHash is deliberately
	// randomised per process for QString.
	uint32_t state = 0x811c9dc5u;
	for (QChar ch : seed) {
		state ^= uint32_t(ch.unicode());
		state *= 16777619u;
	}
	if (state == 0)
		state = 0x1234567u;

	for (int i = 0; i < count; ++i) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		// A gentle envelope so the shape reads as a clip with a beginning and
		// an end rather than as noise across the whole card.
		const double t = count > 1 ? double(i) / double(count - 1) : 0.5;
		const double envelope = 0.55 + 0.45 * std::sin(t * 3.14159265358979);
		const double r = double(state % 1000u) / 1000.0;
		bars.push_back(float(0.15 + 0.85 * r * envelope));
	}
	return bars;
}

// Paint the card: a dark panel with the waveform across the middle. Used by the
// recent strip and by the library's card delegate, so an audio recording looks
// the same wherever it is listed.
void paintAudioCard(QPainter &p, const QRect &rect, const QString &path);

// The same thing as a standalone image, for the places that want an icon rather
// than a paint callback.
QImage audioCardImage(const QString &path, QSize size);

} // namespace harpia
