#pragma once

#include <QMetaType>
#include <QMimeData>
#include <QString>
#include <QUrl>

namespace harpia {

struct AudioLibRemoteServerSound {
	QString name;
	QString path;
	QUrl downloadUrl;
	qint64 sizeBytes = -1;
	double durationSeconds = -1;
	bool durationExact = false;
};

// Carries a selected catalog row between widgets in this process. The
// timeline can find an audio lane before the file has been downloaded.
class AudioLibRemoteServerSoundDrag final : public QMimeData {
public:
	explicit AudioLibRemoteServerSoundDrag(const AudioLibRemoteServerSound &entry) : sound(entry) { setData(format(), "XA1"); }

	static QString format() { return QStringLiteral("application/x-harpia-xa1-sound"); }
	const AudioLibRemoteServerSound sound;
};

} // namespace harpia

Q_DECLARE_METATYPE(harpia::AudioLibRemoteServerSound)
