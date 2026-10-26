#pragma once

#include <QSettings>

#include <algorithm>
#include <optional>

namespace harpia {

// A recording keeps one encoder running while the muxer rotates its files.
// These are recording limits, rather than exact upload-size guarantees: the
// next video keyframe is the first safe place to start a playable part.
struct AutoSplitSettings {
	bool enabled = false;
	enum class Mode { Time, Size };
	Mode mode = Mode::Time;
	int minutes = 30;
	int sizeMB = 500;
};

namespace auto_split_settings {

inline constexpr int maxMinutes = 7 * 24 * 60;
inline constexpr int maxSizeMB = 1024 * 1024;

inline AutoSplitSettings validated(AutoSplitSettings value)
{
	if (value.mode != AutoSplitSettings::Mode::Time && value.mode != AutoSplitSettings::Mode::Size)
		value.mode = AutoSplitSettings::Mode::Time;
	value.minutes = std::clamp(value.minutes, 1, maxMinutes);
	value.sizeMB = std::clamp(value.sizeMB, 1, maxSizeMB);
	return value;
}

// Legacy app-wide "Always" preferences. PresetStore imports these when an old
// profile has no Auto Split fields; current recordings use the active profile.
inline AutoSplitSettings load(QSettings &settings)
{
	AutoSplitSettings value;
	value.enabled = settings.value(QStringLiteral("recording/autoSplit/enabled"), false).toBool();
	value.mode = settings.value(QStringLiteral("recording/autoSplit/mode"), QStringLiteral("time"))
				     .toString() == QStringLiteral("size")
			     ? AutoSplitSettings::Mode::Size
			     : AutoSplitSettings::Mode::Time;
	bool valid = false;
	const int minutes = settings.value(QStringLiteral("recording/autoSplit/minutes"), value.minutes)
				    .toString().toInt(&valid);
	if (valid)
		value.minutes = minutes;
	const int size = settings.value(QStringLiteral("recording/autoSplit/sizeMB"), value.sizeMB)
				 .toString().toInt(&valid);
	if (valid)
		value.sizeMB = size;
	return validated(value);
}

inline void save(QSettings &settings, const AutoSplitSettings &input)
{
	const AutoSplitSettings value = validated(input);
	settings.setValue(QStringLiteral("recording/autoSplit/enabled"), value.enabled);
	settings.setValue(QStringLiteral("recording/autoSplit/mode"),
			  value.mode == AutoSplitSettings::Mode::Size ? QStringLiteral("size")
								  : QStringLiteral("time"));
	settings.setValue(QStringLiteral("recording/autoSplit/minutes"), value.minutes);
	settings.setValue(QStringLiteral("recording/autoSplit/sizeMB"), value.sizeMB);
}

} // namespace auto_split_settings

// Separate the saved preference from a one-recording override. A canceled
// countdown or failed start leaves the override ready for the next attempt.
class AutoSplitState {
public:
	explicit AutoSplitState(const AutoSplitSettings &defaults = {})
		: defaults_(auto_split_settings::validated(defaults))
	{
	}

	const AutoSplitSettings &next() const { return override_ ? *override_ : defaults_; }
	const AutoSplitSettings &defaults() const { return defaults_; }
	bool hasOverride() const { return override_.has_value(); }

	void apply(const AutoSplitSettings &settings, bool always)
	{
		if (always) {
			defaults_ = auto_split_settings::validated(settings);
			override_.reset();
		} else {
			override_ = auto_split_settings::validated(settings);
		}
	}

	void recordingStarted() { override_.reset(); }

private:
	AutoSplitSettings defaults_;
	std::optional<AutoSplitSettings> override_;
};

} // namespace harpia
