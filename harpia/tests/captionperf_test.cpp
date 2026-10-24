// Reusing compiled whitespace patterns and native word clocks must not retain
// a previous caption's content, change Unicode boundaries, or mutate timings.
#include "editor/component/TextSubtitle.hpp"

#include <QCoreApplication>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const QVector<ClipWordTime> native = {{QStringLiteral("one"), -100, 200},
		{QStringLiteral("two"), 400, 800}, {QStringLiteral("three"), 800, 1200}};
	const QString plain = QStringLiteral("one two three");
	const auto shared = timedCaptionWords(plain, native, 1200);
	check(shared == native && shared.constData() == native.constData(),
	      "unchanged native words reuse their complete immutable clock");
	const auto corrected = timedCaptionWords(QStringLiteral("ONE two three"), native, 1200);
	check(corrected[0].text == QStringLiteral("ONE") && corrected[0].startMs == -100 &&
	      corrected[1] == native[1] && native[0].text == QStringLiteral("one"),
	      "correcting spelling detaches while preserving every spoken timestamp");
	auto editable = shared;
	editable[0].text = QStringLiteral("edited");
	editable[0].startMs = 99;
	check(native[0].text == QStringLiteral("one") && native[0].startMs == -100,
	      "editing a reused result cannot modify its original clip clock");
	const QString unicode = QString::fromUtf8("one\u00a0two\u2003three");
	check(subtitleWordsOf(unicode) == QStringList{unicode},
	      "legacy reveal modes retain their original ASCII-whitespace behavior");
	check(timedCaptionWords(unicode, native, 1200) == native,
	      "visible-word animation recognizes Unicode spaces without changing native timing");
	const QString rich = QStringLiteral("<b>one</b><br>two <color=#ff8800>three</color>");
	check(timedCaptionWords(rich, native, 1200) == native,
	      "markup and explicit line breaks still reconcile to the visible words");
	auto changedClock = native;
	changedClock[1].startMs = 350;
	check(timedCaptionWords(plain, changedClock, 1200) == changedClock,
	      "the same caption immediately sees edited timing rather than cached content");
	check(timedCaptionWords(QStringLiteral("a replacement"), {}, 1000) ==
	      QVector<ClipWordTime>{{QStringLiteral("a"), 0, 500}, {QStringLiteral("replacement"), 500, 1000}},
	      "changing text and word count produces a fresh evenly paced clock");
	check(subtitleWordsOf(QStringLiteral(" \t\r\n")).isEmpty() &&
	      timedCaptionWords(QStringLiteral("<b></b>"), native, 1200).isEmpty(),
	      "empty captions do not reuse words from the preceding evaluation");

	std::atomic<bool> consistent{true};
	std::vector<std::thread> workers;
	for (int thread = 0; thread < 4; ++thread)
		workers.emplace_back([&, thread]() {
			for (int i = 0; i < 1000; ++i) {
				const QString first = QStringLiteral("thread%1_%2").arg(thread).arg(i);
				const QString caption = first + QStringLiteral("\tsecond\nthird");
				const auto legacy = subtitleWordsOf(caption);
				const auto visible = timedCaptionWords(caption, {}, 900);
				if (legacy != QStringList{first, QStringLiteral("second"), QStringLiteral("third")} ||
				    visible.size() != 3 || visible[0].text != first ||
				    visible[1].startMs != 300 || visible[2].endMs != 900)
					consistent.store(false);
			}
		});
	for (auto &worker : workers)
		worker.join();
	check(consistent.load(), "concurrent preview/export evaluations keep independent caption content");
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
