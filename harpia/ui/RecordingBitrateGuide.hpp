#pragma once

#include <QLocale>
#include <QString>

namespace harpia {

// Numeric recording bitrates are CBR targets. Decimal MB/GB estimates cover
// the video stream only; Auto is quality-based and has no fixed size target.
inline qint64 recordingVideoBytesPerMinute(int kbps)
{
	return kbps > 0 ? qint64(kbps) * 1000 * 60 / 8 : 0;
}

inline QString recordingBitrateUse(int kbps)
{
	if (kbps < 4000)
		return QStringLiteral("Small capture regions or low-resolution, mostly static content. "
				      "Full-screen text and motion may look soft.");
	if (kbps <= 4000)
		return QStringLiteral("Smaller files for slides, tutorials and mostly static screens. "
				      "Fast scrolling and gameplay may lose detail.");
	if (kbps <= 6000)
		return QStringLiteral("Compact 1080p at 30 fps for desktop demos, meetings and tutorials. "
				      "A useful starting point when storage matters.");
	if (kbps <= 8000)
		return QStringLiteral("General 1080p at 30 fps: a useful balance of detail and file size "
				      "for everyday screen recording.");
	if (kbps <= 10000)
		return QStringLiteral("Detailed 1080p screens or moderate-motion 1080p at 60 fps. "
				      "More room for scrolling and movement.");
	if (kbps <= 12000)
		return QStringLiteral("General 1080p at 60 fps, or mostly static 1440p at 30 fps. "
				      "Useful for smoother demos and light gameplay.");
	if (kbps <= 16000)
		return QStringLiteral("Detailed 1440p at 30 fps or high-motion 1080p at 60 fps. "
				      "Useful when fine text and movement both matter.");
	if (kbps <= 20000)
		return QStringLiteral("1440p at 60 fps or demanding 1080p at 60 fps. "
				      "Large files; ordinary desktop tutorials can often use less.");
	if (kbps <= 30000)
		return QStringLiteral("4K at 30 fps or demanding 1440p at 60 fps. "
				      "Useful for higher-resolution recordings kept for editing.");
	if (kbps <= 40000)
		return QStringLiteral("Detailed 4K at 30 fps or moderate-motion 4K at 60 fps. "
				      "Very large files for preserving more detail.");
	if (kbps <= 50000)
		return QStringLiteral("Demanding 4K at 60 fps, including fast gameplay. "
				      "Best when quality matters more than storage.");
	if (kbps <= 80000)
		return QStringLiteral("Very demanding 4K or high-frame-rate footage for further editing. "
				      "Usually excessive for everyday desktop recording.");
	return QStringLiteral("Exceptional detail, high-resolution or high-frame-rate editing masters. "
			      "Huge files: make a short test to check that the extra bitrate visibly helps.");
}

inline QString recordingBitrateGuide(int kbps)
{
	if (kbps <= 0)
		return QStringLiteral("Auto — quality-based recording\n\n"
			"The encoder adjusts bitrate to maintain picture quality. Static screens often use less "
			"space; busy scenes can use much more. File size cannot be predicted from Auto.\n\n"
			"For more predictable file sizes, choose a numeric bitrate. For ordinary 1080p desktop "
			"recording at 30 fps, try 6,000–8,000 kbps and check a short sample.");

	const QLocale locale;
	const double mbPerMinute = recordingVideoBytesPerMinute(kbps) / 1000000.0;
	const double gbPerHour = recordingVideoBytesPerMinute(kbps) * 60 / 1000000000.0;
	return QStringLiteral("%1 Mbps — about %2 MB per minute / %3 GB per hour\n\n"
		"Useful for: %4\n\n"
		"These are H.264 starting points; resolution, frame rate, motion and small text affect quality. "
		"HEVC/AV1 may achieve similar quality at a lower bitrate. At the same fixed bitrate, "
		"changing codec or fps does not reduce this size estimate.\n\n"
		"Video-only estimates in decimal MB/GB. Audio and container overhead add more; "
		"actual size can vary by encoder.")
		.arg(locale.toString(kbps / 1000.0, 'g', 6),
		     locale.toString(mbPerMinute, 'g', 6),
		     locale.toString(gbPerHour, 'g', 6), recordingBitrateUse(kbps));
}

} // namespace harpia
