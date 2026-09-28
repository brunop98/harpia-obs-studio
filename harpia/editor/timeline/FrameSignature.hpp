#pragma once

// What a composited frame depends on, as 16 bytes -- the key of the preview's
// finished-frame cache (VideoEditorWindow::previewFrames_).
//
// The instant, the render and project sizes, a generation number the caller
// bumps when media, scripts or shaders change, and for every picture track its
// kind and visibility plus the FULL saved form of each clip on screen at that
// instant. clipToJson is what a project file keeps, which is by definition
// everything a clip is, so no field can be forgotten here and later make the
// cache serve a stale picture. Clips elsewhere on the timeline are not read: an
// edit at 1:00 leaves every cached frame at 0:10 valid.
//
// These are exactly the model fields TimelineCompositor::compose reads
// (tracks' kind, hidden and clips); a new one there belongs here too.

#include "TimelineJson.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSize>

namespace harpia {

inline QByteArray timelineFrameSignature(const TimelineModel &m, qint64 outMs, QSize render, QSize logical,
					 qint64 generation)
{
	QCryptographicHash h(QCryptographicHash::Md5);
	const auto addInt = [&h](qint64 v) { h.addData(QByteArrayView(reinterpret_cast<const char *>(&v), sizeof v)); };
	addInt(outMs);
	addInt(render.width());
	addInt(render.height());
	addInt(logical.width());
	addInt(logical.height());
	addInt(generation);
	for (int ti = 0; ti < m.tracks.size(); ++ti) {
		const TlTrack &t = m.tracks[ti];
		if (t.kind == TlTrack::Kind::Audio)
			continue; // no picture
		addInt(ti);
		addInt(int(t.kind));
		addInt(t.hidden ? 1 : 0);
		for (int ci = 0; ci < t.clips.size(); ++ci) {
			const TlClip &c = t.clips[ci];
			// Some slack either side: a transition reads both clips of an
			// overlap, and hashing a clip too many only costs a few bytes.
			if (c.outStartMs > outMs + 50 || c.outEndMs() < outMs - 50)
				continue;
			addInt(ci);
			h.addData(QJsonDocument(clipToJson(c)).toJson(QJsonDocument::Compact));
		}
	}
	return h.result();
}

} // namespace harpia
