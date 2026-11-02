#pragma once

// One file per selected picture clip, with only its linked audio.
// Export copies retain the clip's own range; the project's end marker does not
// constrain a library clip, and unrelated layers, music and voiceover stay out.
#include "TimelineSlice.hpp"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace harpia::separate_clips {

inline bool exportable(const TimelineModel &m, const QPair<int, int> &p)
{
	return p.first >= 0 && p.first < m.tracks.size() && p.second >= 0 &&
	       p.second < m.tracks[p.first].clips.size() &&
	       m.tracks[p.first].kind == TlTrack::Kind::Video &&
	       m.tracks[p.first].clips[p.second].type != TlClip::Type::Effect;
}

inline QVector<QPair<int, int>> targets(const TimelineModel &m, const QVector<QPair<int, int>> &selection)
{
	QVector<QPair<int, int>> out;
	for (const auto &p : selection)
		if (exportable(m, p) && !out.contains(p))
			out.append(p);
	std::sort(out.begin(), out.end(), [&m](const auto &a, const auto &b) {
		const qint64 as = m.tracks[a.first].clips[a.second].outStartMs;
		const qint64 bs = m.tracks[b.first].clips[b.second].outStartMs;
		return as != bs ? as < bs : a < b;
	});
	return out;
}

inline TimelineModel isolate(const TimelineModel &original, const QPair<int, int> &p)
{
	if (!exportable(original, p))
		return {};
	const TlClip &primary = original.tracks[p.first].clips[p.second];
	TimelineModel isolated = original;
	for (TlTrack &track : isolated.tracks) {
		track.clips.clear();
		track.solo = false; // an unrelated soloed track cannot mute this export
	}
	isolated.tracks[p.first].hidden = false;
	isolated.tracks[p.first].clips.append(primary);
	isolated.tracks[p.first].clips[0].transition.enabled = false;
	for (const auto &partner : original.linkedClips(primary.link)) {
		const TlTrack &track = original.tracks[partner.first];
		if (track.kind == TlTrack::Kind::Audio) {
			isolated.tracks[partner.first].hidden = false;
			isolated.tracks[partner.first].clips.append(track.clips[partner.second]);
		}
	}
	// No project sound rules: only the clip's own Sound components can add
	// derived sounds when the window prepares this isolated render.
	isolated.soundRules.clear();
	TimelineModel out = sliceTimeline(isolated, primary.outStartMs, primary.outEndMs());
	out.tags = original.tags;
	return out;
}

inline QString nameError(const QStringList &names)
{
	static const QRegularExpression forbidden(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
	static const QRegularExpression reserved(
		QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
		QRegularExpression::CaseInsensitiveOption);
	QSet<QString> used;
	for (int i = 0; i < names.size(); ++i) {
		const QString &name = names[i];
		if (name.isEmpty() || name != name.trimmed() || name.startsWith(QLatin1Char('.')) ||
		    name.endsWith(QLatin1Char('.')) || name.contains(forbidden) || reserved.match(name).hasMatch())
			return QStringLiteral("Clip %1 needs a valid filename without folders or a file extension.").arg(i + 1);
		const QString key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
		if (used.contains(key))
			return QStringLiteral("Each clip needs a unique filename. \"%1\" is used more than once.").arg(name);
		used.insert(key);
	}
	return {};
}

} // namespace harpia::separate_clips
