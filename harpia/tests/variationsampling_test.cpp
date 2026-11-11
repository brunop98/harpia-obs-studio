#include "editor/VariationSampling.hpp"
#include "editor/NumberedCover.hpp"
#include "editor/timeline/TextVariations.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>

using namespace harpia;
namespace {
int failures;
void check(bool passed, const char *message)
{
	std::printf("%s %s\n", passed ? "PASS" : "FAIL", message);
	failures += !passed;
}
} // namespace
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	using namespace variation_sampling;
	check(count({3, 4}).value == 12 && !count({3, 4}).overflow, "count the full combination space");
	const auto chosen = sample({3, 4}, 5, 42);
	QSet<QVector<int>> unique;
	bool valid = true;
	for (const auto &pick : chosen) {
		unique.insert(pick);
		valid &= pick.size() == 2 && pick[0] >= 0 && pick[0] < 3 && pick[1] >= 0 && pick[1] < 4;
		valid &= at({3, 4}, indexOf({3, 4}, pick)) == pick;
	}
	check(chosen.size() == 5 && unique.size() == 5 && valid, "sample exactly five distinct valid combinations");
	check(chosen == sample({3, 4}, 5, 42), "fixed seeds reproduce the selected combinations");
	check(sample({3, 4}, 100, 7).size() == 12, "requests above the population select every combination once");
	check(sample({}, 5).isEmpty() && sample({3, 0}, 5).isEmpty() && sample({3, 4}, 0).isEmpty(),
	      "empty, invalid and zero requests do no work");
	const auto distant = sample({1000000, 1000000, 1000000}, 20, 17);
	check(distant.size() == 20 && indexOf({1000000, 1000000, 1000000}, distant.front()) > 2000,
	      "sampling reaches the entire large pool instead of the first 2000 versions");
	QVector<int> huge(70, 2);
	check(count(huge).overflow && count(huge).value == std::numeric_limits<qint64>::max(),
	      "astronomical counts saturate without numeric overflow");
	QElapsedTimer timer;
	timer.start();
	const auto overflowSample = sample(huge, 100, 41);
	QSet<int> first, last;
	unique.clear();
	for (const auto &pick : overflowSample) {
		unique.insert(pick);
		first.insert(pick.front());
		last.insert(pick.back());
	}
	check(overflowSample.size() == 100 && unique.size() == 100 && first.size() == 2 && last.size() == 2 &&
		      timer.elapsed() < 2000,
	      "overflow-sized products remain fast and randomize early and late clips independently");
	const qint64 version = qint64(std::numeric_limits<int>::max()) + 42;
	check(text_variations::fileName("sample", version, version, {}).contains(QString::number(version)),
	      "filenames preserve variation numbers beyond 32 bits");
	cover::Numbering numbering;
	numbering.enabled = true;
	numbering.start = 7;
	check(numbering.textFor(version) == QString::number(version + 6) &&
		      numbering.textFor(std::numeric_limits<qint64>::max()) ==
			      QString::number(quint64(std::numeric_limits<qint64>::max()) + 6),
	      "numbered covers retain large version numbers without overflow");
	return failures ? 1 : 0;
}
