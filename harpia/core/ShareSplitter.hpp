#pragma once

#include <QStringList>

#include <atomic>
#include <functional>

namespace harpia::share {

// Decimal MB: stays below either a decimal or a binary 50 MB upload limit.
constexpr qint64 discordPartBytes = 49'000'000;

struct SplitResult {
	bool ok = false;
	bool canceled = false;
	bool shorterGopNeeded = false;
	QString error;
	QStringList files;
};

// Split an H.264/AAC sharing copy at keyframes into independently playable MP4
// files. Check FINAL muxed sizes and retry smaller ranges when needed. Existing
// files are never overwritten; failure/cancel removes only files made here.
SplitResult splitMp4(const QString &input, const QString &directory, qint64 maxBytes,
		     const std::atomic<bool> &cancel,
		     const std::function<void(int, qint64)> &progress = {});

} // namespace harpia::share
