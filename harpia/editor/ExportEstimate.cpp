#include "ExportEstimate.hpp"

#include <algorithm>
#include <cmath>

namespace harpia {
namespace {

// BITS per pixel per frame at CRF 23, for x264 at the preset this build uses.
// 0.085 puts 1080p30 at about 5.3 Mbit/s, which is where general-purpose
// content lands at that quality. exportdialog_test brackets it against two real
// encodes -- a near-static pattern and pure noise -- so a change of preset or
// encoder shows up as a failure rather than as a number that quietly stops
// describing the encoder it is named after.
constexpr double kH264BppAtCrf23 = 0.085;
// VP9 at the same visual quality is roughly two thirds the bits.
constexpr double kVp9Factor = 0.68;
// Each 6 points of CRF is about a halving/doubling of bitrate. This is the one
// part of the model that is a genuine property of the encoders rather than a
// fit -- CRF is defined on a logarithmic scale.
constexpr double kCrfPerDoubling = 6.0;

constexpr double kAacBitsPerSecond = 128000.0;

// BYTES per pixel per frame for a 256-colour dithered GIF (measured from this
// build's palettegen/paletteuse output, not converted from the video figure). Much larger than
// video because a GIF has no motion compensation worth the name: every frame is
// LZW over an indexed image.
constexpr double kGifBppFull = 0.055;

} // namespace

qint64 estimateExportBytes(const ExportEstimateInput &in)
{
	if (in.seconds <= 0.0 || in.width <= 0 || in.height <= 0 || in.fps <= 0.0)
		return 0;

	const double pixels = double(in.width) * double(in.height);
	const double frames = in.fps * in.seconds;

	if (in.format == ClipExporter::Format::Gif) {
		// Palette size moves the file a long way, but not linearly: LZW codes
		// get shorter as the alphabet shrinks, so it follows the bit depth
		// rather than the colour count. 256 colours = 8 bits, 8 colours = 3.
		const int colors = std::clamp(in.gifColors, 2, 256);
		const double depth = std::log2(double(colors)); // 1..8
		double bpp = kGifBppFull * (depth / 8.0);
		// Dithering scatters neighbouring pixels, which is exactly what LZW
		// cannot compress. Turning it off is worth roughly a sixth.
		if (!in.gifDither)
			bpp *= 0.84;
		return qint64(pixels * frames * bpp);
	}

	double bpp = kH264BppAtCrf23 *
		     std::pow(2.0, (23.0 - double(in.videoCrf)) / kCrfPerDoubling);
	if (in.format == ClipExporter::Format::WebM)
		bpp *= kVp9Factor;
	// bpp is BITS per pixel per frame, so this is already bits -- multiplying
	// by 8 here (as an early draft did) inflates every estimate eightfold, and
	// the only thing that catches it is comparing against a real encode.
	const double videoBits = pixels * frames * bpp;

	// WebM is written silent by this exporter, so audio is not a choice there.
	const bool audio = in.keepAudio && in.format != ClipExporter::Format::WebM;
	const double audioBits = audio ? kAacBitsPerSecond * in.seconds : 0.0;

	// A container costs a few tens of KB of index on anything long enough to
	// care about; below that it is the dominant term and ignoring it makes a
	// two-second clip read as "0.0 MB".
	const double overheadBytes = 16000.0 + 1200.0 * in.seconds;
	return qint64((videoBits + audioBits) / 8.0 + overheadBytes);
}

QString humanFileSize(qint64 bytes)
{
	if (bytes <= 0)
		return QStringLiteral("—");
	const double kb = double(bytes) / 1000.0;
	if (kb < 1.0)
		return QStringLiteral("%1 B").arg(bytes);
	if (kb < 1000.0)
		return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
	const double mb = kb / 1000.0;
	if (mb < 1000.0)
		return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10.0 ? 2 : 1);
	return QStringLiteral("%1 GB").arg(mb / 1000.0, 0, 'f', 2);
}

} // namespace harpia
