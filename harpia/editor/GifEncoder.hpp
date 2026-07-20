#pragma once

#include <QString>

#include <cstdint>
#include <functional>

namespace harpia {

// Encodes a trimmed/cropped section of a video to a high-quality animated GIF
// using libavfilter's palettegen + paletteuse (a shared 256-colour palette with
// dithering) — the standard approach for clean GIFs. Isolated from the rest of
// the editor because it is the only translation unit that needs libavfilter.
class GifEncoder {
public:
	struct Params {
		qint64 startMs = 0;
		qint64 endMs = 0; // 0 == end of clip
		bool crop = false;
		int cropX = 0, cropY = 0, cropW = 0, cropH = 0; // source pixels
		int fps = 15;
		int width = 0;      // output width (0 == crop/source width — no downscale); height auto
		double speed = 1.0; // playback speed multiplier (2.0 = twice as fast)
	};

	// Returns true on success. `canceled()` is polled to abort; `progress()`
	// receives (percent, etaMs, outBytes). `error` gets a short message on
	// failure.
	static bool encode(const QString &inPath, const QString &outPath, const Params &p,
			   const std::function<bool()> &canceled,
			   const std::function<void(int, qint64, qint64)> &progress, QString *error);
};

} // namespace harpia
