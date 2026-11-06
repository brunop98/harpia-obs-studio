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

	QString textFor(int version1) const
	{
		if (!enabled) {
			return {};
		}
		const qint64 number = qint64(std::max(0, start)) + std::max<qint64>(0, qint64(version1) - 1);
		return prefix +
		       (includeNumber
				? QString::number(number).rightJustified(std::clamp(digits, 1, 8), QLatin1Char('0'))
				: QString()) +
		       suffix;
	}
};

} // namespace harpia::cover
