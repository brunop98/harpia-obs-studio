#pragma once

#include "TimelineModel.hpp"

#include <QSet>
#include <utility>

namespace harpia::random_clip_instances {

inline QVector<TlClipOption> options(const TlClip &clip)
{
	QVector<TlClipOption> out;
	for (int i = 0; i < clip.clipOptions.size(); ++i) {
		out.append(clip.clipOptionAt(i));
	}
	return out;
}

inline void applyOptions(TlClip &clip, const QVector<TlClipOption> &pool, int shown)
{
	if (pool.isEmpty()) {
		return;
	}
	const qint64 length = clip.srcLenMs();
	shown = std::clamp(shown, 0, int(pool.size()) - 1);
	if (clip.sourceId != pool[shown].sourceId) {
		clip.peaks.clear();
	}
	clip.clipOptions = pool;
	clip.clipShown = shown;
	clip.sourceId = pool[shown].sourceId;
	clip.srcStartMs = pool[shown].srcStartMs;
	clip.srcEndMs = clip.srcStartMs + length;
}

inline QString fitError(const QVector<TlClipOption> &pool, qint64 length, const std::function<qint64(int)> &durationOf)
{
	for (const TlClipOption &option : pool) {
		const qint64 total = durationOf ? durationOf(option.sourceId) : 0;
		if (option.srcStartMs < 0 || length <= 0 ||
		    (total > 0 && (option.srcStartMs > total || length > total - option.srcStartMs))) {
			return QStringLiteral(
				"This source moment cannot fill every linked Random Clip's duration. "
				"Choose an earlier start, shorten the longer copy, or make it independent.");
		}
	}
	return {};
}

// A small cache of linked pools, not another source of project data. Comparing
// before notifying the editor catches Inspector edits, Slip and timeline trims
// alike, and keeps propagation in the same undo step as the originating edit.
class Synchronizer {
	struct Family {
		QVector<TlClipOption> pool;
		QHash<quint64, TlClip> members;
	};
	QHash<QString, Family> families_;

public:
	void reset(TimelineModel &model)
	{
		families_.clear();
		sync(model, {});
	}

	QString sync(TimelineModel &model, const std::function<qint64(int)> &durationOf, quint64 preferred = 0)
	{
		QHash<QString, QVector<QPair<int, int>>> groups;
		QSet<quint64> seen;
		for (int ti = 0; ti < model.tracks.size(); ++ti) {
			for (int ci = 0; ci < std::as_const(model).tracks[ti].clips.size(); ++ci) {
				const TlClip &read = std::as_const(model).tracks[ti].clips[ci];
				// Ordinary Duplicate, track paste and splits must not accidentally
				// create an instance just because they copied the struct.
				if (seen.contains(read.objectId)) {
					TlClip &clip = model.tracks[ti].clips[ci];
					clip.objectId = TlClip::nextObjectId();
					clip.randomClipFamily.clear();
				}
				const TlClip &clip = std::as_const(model).tracks[ti].clips[ci];
				seen.insert(clip.objectId);
				if ((clip.type != TlClip::Type::Video || !clip.isRandomClip()) &&
				    !clip.randomClipFamily.isEmpty()) {
					model.tracks[ti].clips[ci].randomClipFamily.clear();
					continue;
				}
				if (!clip.randomClipFamily.isEmpty()) {
					groups[clip.randomClipFamily].append({ti, ci});
				}
			}
		}
		QHash<QString, Family> next;
		QString error;
		for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
			const auto old = families_.constFind(it.key());
			const auto &places = it.value();
			auto chosen = places.front();
			if (old != families_.cend()) {
				for (const auto &place : places) {
					const TlClip &clip = model.tracks[place.first].clips[place.second];
					if (options(clip) != old->pool) {
						chosen = place;
						if (clip.objectId == preferred) {
							break;
						}
					}
				}
			}
			const TlClip &editor = model.tracks[chosen.first].clips[chosen.second];
			const quint64 editorId = editor.objectId;
			const auto pool = options(editor);
			qint64 longest = 0;
			for (const auto &place : places) {
				longest = std::max(longest, model.tracks[place.first].clips[place.second].srcLenMs());
			}
			const QString invalid = fitError(pool, longest, durationOf);
			if (!invalid.isEmpty() && old != families_.cend()) {
				for (const auto &place : places) {
					TlClip &clip = model.tracks[place.first].clips[place.second];
					const auto previous = old->members.constFind(clip.objectId);
					if (previous != old->members.cend()) {
						clip = previous.value();
					}
				}
				error = invalid;
			} else {
				for (const auto &place : places) {
					TlClip &clip = model.tracks[place.first].clips[place.second];
					if (clip.objectId == editorId) {
						continue;
					}
					int shown = clip.clipShown;
					// Keep watching the same choice when another member inserts
					// or removes an option before it.
					if (clip.clipOptions.size() != pool.size()) {
						const int found = pool.indexOf(clip.clipOptionAt(shown));
						if (found >= 0) {
							shown = found;
						}
					}
					if (options(clip) != pool) {
						applyOptions(clip, pool, shown);
					}
				}
			}
			Family state;
			for (const auto &place : places) {
				const TlClip &clip = model.tracks[place.first].clips[place.second];
				state.members.insert(clip.objectId, clip);
				state.pool = options(clip);
			}
			next.insert(it.key(), state);
		}
		families_ = next;
		return error;
	}
};

} // namespace harpia::random_clip_instances
