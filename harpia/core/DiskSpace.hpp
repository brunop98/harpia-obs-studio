#pragma once

// How much room is left, and whether that is enough to start.
//
// Running out of disk mid-recording is the worst failure this app has: you
// cannot re-take the thing you were recording, and by the time you find out it
// has already happened. It is also completely predictable beforehand, which is
// what makes not saying anything indefensible.
//
// So there are two jobs, and they are separate on purpose:
//   * the recording screen states the free space and the destination FOLDER at
//     all times, because the other half of this failure is recording somewhere
//     you did not mean to and looking for the file afterwards;
//   * starting below a threshold asks first.
//
// The arithmetic and the wording live here rather than in the window so the
// thresholds are stated once and can be checked without a screen.

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QString>

namespace harpia {

// Below this, starting a recording asks for confirmation. Two gigabytes is
// roughly a few minutes of 1080p at the bitrates this app writes -- not a
// figure to be precise about, but the point where "it will probably be fine"
// stops being true.
inline constexpr qint64 kLowDiskBytes = 2LL * 1024 * 1024 * 1024;
// And below THIS it is not a caution, it is a refusal waiting to happen: less
// than a minute at any useful quality.
inline constexpr qint64 kCriticalDiskBytes = 250LL * 1024 * 1024;

enum class DiskLevel {
	Unknown,  // no folder, or the drive could not be queried
	Ok,       // enough to get on with
	Low,      // under kLowDiskBytes: worth asking about
	Critical, // under kCriticalDiskBytes: recording will very likely fail
};

struct DiskStatus {
	DiskLevel level = DiskLevel::Unknown;
	qint64 freeBytes = -1; // -1 when unknown
	bool isValid() const { return level != DiskLevel::Unknown; }
};

inline DiskLevel diskLevelFor(qint64 freeBytes)
{
	if (freeBytes < 0)
		return DiskLevel::Unknown;
	if (freeBytes < kCriticalDiskBytes)
		return DiskLevel::Critical;
	if (freeBytes < kLowDiskBytes)
		return DiskLevel::Low;
	return DiskLevel::Ok;
}

// What is left on the drive holding `folder`. Unknown for an empty path or a
// drive that will not answer -- a network share that has gone away, say. Never
// guesses: a made-up number here would be worse than none, because it is
// precisely the number someone would rely on.
inline DiskStatus diskStatusFor(const QString &folder)
{
	DiskStatus s;
	if (folder.isEmpty())
		return s;
	const QStorageInfo st{folder};
	if (!st.isValid() || !st.isReady())
		return s;
	s.freeBytes = st.bytesAvailable();
	s.level = diskLevelFor(s.freeBytes);
	return s;
}

// "12.4 GB", "820 MB". Whole megabytes below a gigabyte -- a tenth of a
// megabyte is noise -- and one decimal above, because the difference between
// 2.1 and 2.9 GB is the difference this whole feature is about.
inline QString formatBytes(qint64 bytes)
{
	if (bytes < 0)
		return QStringLiteral("unknown");
	constexpr qint64 kMB = 1024 * 1024;
	constexpr qint64 kGB = 1024 * kMB;
	if (bytes >= kGB)
		return QStringLiteral("%1 GB").arg(double(bytes) / kGB, 0, 'f', 1);
	if (bytes >= kMB)
		return QStringLiteral("%1 MB").arg(bytes / kMB);
	return QStringLiteral("%1 KB").arg(std::max<qint64>(0, bytes / 1024));
}

// The one line the recording screen shows. Names the folder as well as the
// space, because "recorded to the wrong place" is the other half of this
// problem and it is silent too.
inline QString diskLine(const QString &folder, const DiskStatus &s)
{
	if (folder.isEmpty())
		return QStringLiteral("No output folder set");
	const QString where = QDir::toNativeSeparators(folder);
	if (!s.isValid())
		return QStringLiteral("Saving to %1").arg(where);
	return QStringLiteral("%1 free  ·  %2").arg(formatBytes(s.freeBytes), where);
}

// What the confirmation says when Record is pressed with little room left.
// Empty when there is nothing to warn about, which is also how the caller knows
// not to ask.
inline QString lowDiskPrompt(const DiskStatus &s)
{
	if (s.level == DiskLevel::Critical)
		return QStringLiteral(
			"Only %1 free on the output drive.\n\nA recording will almost certainly "
			"run out of space, and a recording cannot be re-taken. Record anyway?")
			.arg(formatBytes(s.freeBytes));
	if (s.level == DiskLevel::Low)
		return QStringLiteral(
			"Only %1 free on the output drive.\n\nThat is a few minutes at most. "
			"Record anyway?")
			.arg(formatBytes(s.freeBytes));
	return QString();
}

} // namespace harpia
