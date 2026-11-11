#pragma once

#include <QString>
#include <algorithm>

namespace harpia::cover {

// Export-only presentation: file names and project text are independent.
struct Numbering {
	bool enabled = false;
	bool includeNumber = true;
	QString prefix, suffix;
	int start = 1;
	int digits = 1;
	bool firstFrame = false;

	QString textFor(qint64 version1) const
	{
		if (!enabled) {
			return {};
		}
		const quint64 number = quint64(std::max(0, start)) + quint64(version1 > 1 ? version1 - 1 : 0);
		return prefix +
		       (includeNumber
				? QString::number(number).rightJustified(std::clamp(digits, 1, 8), QLatin1Char('0'))
				: QString()) +
		       suffix;
	}
};

} // namespace harpia::cover
