// Caption highlights and word pops use the same pure time functions when
// scrubbing, playing and exporting. Stored text and its spoken times remain
// editable; rich-text tags never become extra timed words.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentJson.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/TextSubtitle.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QGuiApplication>
#include <QImage>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static bool samePose(const TlTransform &a, const TlTransform &b)
{
	return a.posX == b.posX && a.posY == b.posY && a.scale == b.scale &&
	       a.rotation == b.rotation && a.opacity == b.opacity;
}
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static QVector<QColor> wordColors(const QString &caption)
{
	QVector<QColor> colors;
	bool inWord = false;
	for (const auto &line : rich_text::parse(caption, 64)) {
		for (const auto &run : line) {
			for (const auto &ch : run.text) {
				if (ch.isSpace()) {
					inWord = false;
				} else if (!inWord) {
					colors.append(run.style.color);
					inWord = true;
				}
			}
		}
		inWord = false;
	}
	return colors;
}

static QString highlighted(const QString &caption, const QVector<ClipWordTime> &words, qint64 at,
			   SubtitleMode mode = SubtitleMode::HighlightCurrent,
			   const QColor &color = QColor(Qt::yellow), qint64 duration = 2000)
{
	return subtitleTextAt(caption, words, at, mode, duration, color);
}

class NoFrames : public TimelineCompositor::FrameProvider {
	QImage frameFor(int, qint64) override { return {}; }
};

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	const QVector<ClipWordTime> words = {{QStringLiteral("First"), 100, 300},
		{QStringLiteral("second"), 500, 750}, {QStringLiteral("third"), 1000, 1200},
		{QStringLiteral("last"), 1500, 1800}};
	const QString caption = QStringLiteral("  <b>First</b> \t<color=#ff0000>sec<i>ond</i></color>\nthird<br>last ");
	const auto resolved = timedCaptionWords(caption, words, 2000);
	check(resolved.size() == 4 && resolved[1].text == QStringLiteral("second") && resolved[1].startMs == 500,
	      "visible words retain their native timing despite inline tags and line breaks");
	check(highlighted(caption, words, -1) == caption, "the full phrase is visible before the first spoken word");
	check(highlighted(caption, words, 300) == caption, "a gap after a word does not highlight an unspoken word");
	check(highlighted(caption, words, 1800) == caption, "the last word's end clears current-word highlighting");
	const QString second = highlighted(caption, words, 600);
	check(rich_text::plainText(second) == rich_text::plainText(caption),
	      "highlighting preserves every visible character, tab and line break");
	const auto colors = wordColors(second);
	check(colors.size() == 4 && !colors[0].isValid() && colors[1] == QColor(Qt::yellow) && !colors[2].isValid(),
	      "the spoken word alone gets the selected colour even inside a keyword colour tag");
	check(rich_text::parse(second, 64)[0][1].style.bold,
	      "highlighting keeps the caption's inline bold styling");
	const auto firstColors = wordColors(highlighted(caption, words, 100));
	check(firstColors[0] == QColor(Qt::yellow) && firstColors[1] == QColor(Qt::red),
	      "inactive keywords keep their own custom colours");
	const auto spokenColors = wordColors(highlighted(caption, words, 1100, SubtitleMode::HighlightSpoken));
	check(spokenColors.size() == 4 && spokenColors[0] == QColor(Qt::yellow) &&
	      spokenColors[1] == QColor(Qt::yellow) && spokenColors[2] == QColor(Qt::yellow) && !spokenColors[3].isValid(),
	      "karaoke mode highlights the words that have started while retaining the whole phrase");
	const auto lastColors = wordColors(highlighted(caption, words, 2000, SubtitleMode::HighlightSpoken));
	check(lastColors.size() == 4 && lastColors[3] == QColor(Qt::yellow),
	      "spoken-word highlights remain after the last word");
	const QString nested = QStringLiteral("<color=blue>First <color=red>second</color> third</color>");
	const auto nestedColors = wordColors(highlighted(nested, {words[0], words[1], words[2]}, 600));
	check(nestedColors.size() == 3 && nestedColors[0] == QColor(Qt::blue) &&
	      nestedColors[1] == QColor(Qt::yellow) && nestedColors[2] == QColor(Qt::blue),
	      "nested keyword colours are restored on both sides of a highlighted word");
	const QString literal = QStringLiteral("<noparse><b>literal</b></noparse>  <notatag>strange</notatag>");
	const auto literalWords = timedCaptionWords(literal, {}, 1000);
	const QString literalHighlight = highlighted(literal, literalWords, 100);
	check(rich_text::plainText(literalHighlight) == rich_text::plainText(literal),
	      "noparse content and unknown tags remain literal when highlighted");
	check(wordColors(literalHighlight).first() == QColor(Qt::yellow),
	      "literal noparse words can be highlighted without showing generated tags");
	const QString invalid = QStringLiteral("x <color=banana>word</color>");
	check(rich_text::plainText(highlighted(invalid, {}, 700, SubtitleMode::HighlightCurrent, Qt::yellow, 1000)) ==
	      rich_text::plainText(invalid), "an invalid colour tag cannot become valid because highlighting was inserted");
	const QColor translucent(20, 160, 220, 128);
	check(wordColors(highlighted(QStringLiteral("one"), {}, 100, SubtitleMode::HighlightCurrent, translucent)).first() == translucent,
	      "the chosen highlight colour retains its alpha channel");
	const QString unicode = QString::fromUtf8("Olá\u00a0mundo\n👋");
	check(rich_text::plainText(highlighted(unicode, {}, 700, SubtitleMode::HighlightCurrent, Qt::yellow, 900)) == unicode &&
	      timedCaptionWords(unicode, {}, 900).size() == 3,
	      "Unicode spaces, accented text and emoji retain their visible timing and content");
	const auto fallback = timedCaptionWords(QStringLiteral("a  b\nc"), {}, 900);
	check(fallback.size() == 3 && fallback[1].startMs == 300 && fallback[2].endMs == 900,
	      "hand-typed captions pace their visible words evenly across the clip");
	const auto rewrite = timedCaptionWords(QStringLiteral("now a longer new phrase"), words, 2000);
	check(rewrite.size() == 5 && rewrite.first().startMs == 100 && rewrite.last().endMs == 1800,
	      "a rewrite with a different word count uses the original spoken span");
	check(subtitleTextAt(QStringLiteral("one two"), {{QStringLiteral("one"), 0, 100}, {QStringLiteral("two"), 500, 800}},
	      200, SubtitleMode::OneWord) == QStringLiteral("one"), "legacy one-word mode still holds through speech gaps");
	check(subtitleTextAt(QStringLiteral("one two"), {}, 600, SubtitleMode::BuildUp, 1000) == QStringLiteral("one two"),
	      "legacy build-up mode keeps its existing pacing");

	check(captionPopScaleAt(caption, words, 0, 2000) == 1.0, "pop leaves the pose unchanged before the first word");
	check(std::abs(captionPopScaleAt(caption, words, 160, 2000) - 1.1) < 1e-9,
	      "a word reaches the configured ten-percent pop halfway through its pulse");
	check(captionPopScaleAt(caption, words, 220, 2000) == 1.0 && captionPopScaleAt(caption, words, 400, 2000) == 1.0,
	      "a finished pulse returns to the authored zoom and stays still during pauses");
	check(std::abs(captionPopScaleAt(caption, words, 560, 2000) - 1.1) < 1e-9,
	      "each native word timestamp starts its own pulse");
	check(std::abs(captionPopScaleAt(QStringLiteral("one two"), {}, 560, 1000) - 1.1) < 1e-9,
	      "hand-typed words receive the same evenly paced pop");
	const QVector<ClipWordTime> quick = {{QStringLiteral("a"), 0, 40}, {QStringLiteral("b"), 40, 80}};
	check(std::abs(captionPopScaleAt(QStringLiteral("a b"), quick, 20, 80) - 1.1) < 1e-9 &&
	      captionPopScaleAt(QStringLiteral("a b"), quick, 40, 80) == 1.0,
	      "short word gaps finish the first pulse before the next starts");

	ComponentRegistry registry;
	registerBuiltinComponents(registry);
	const auto *subtitle = registry.find(QStringLiteral("harpia.subtitle"));
	const auto *pop = registry.find(QStringLiteral("harpia.captionPop"));
	check(subtitle && subtitle->props.size() == 2 && subtitle->props[0].choices.size() == 5 &&
	      subtitle->props[1].type == PropType::Color, "the Subtitle component exposes all five modes and a colour picker");
	check(pop && pop->stage == Stage::Transform && pop->clipKinds == unsigned(ClipKindText),
	      "Caption Pop is a caption-only transform that composes with existing keyframes");
	ComponentInstance highlight;
	highlight.typeId = QStringLiteral("harpia.subtitle");
	highlight.instanceId = QStringLiteral("highlight");
	highlight.props.insert(QStringLiteral("mode"), 3);
	highlight.props.insert(QStringLiteral("activeColor"), QColor(Qt::cyan));
	ComponentInstance punch;
	punch.typeId = QStringLiteral("harpia.captionPop");
	punch.instanceId = QStringLiteral("pop");
	punch.props.insert(QStringLiteral("durationMs"), 120);
	punch.props.insert(QStringLiteral("strength"), 0.1);
	ComponentStack stack({highlight, punch}, registry);
	EvalContext context;
	context.durMs = 2000;
	context.tMs = 560;
	TlTransform pose;
	pose.scale = 2;
	const auto state = stack.evaluatePose(context, pose, &caption, &words);
	check(std::abs(state.xf.scale - 2.2) < 1e-9 && wordColors(state.text)[1] == QColor(Qt::cyan),
	      "highlight and pop run together through the production component stack");
	context.tMs = 1100;
	stack.evaluatePose(context, pose, &caption, &words);
	context.tMs = 560;
	const auto sought = stack.evaluatePose(context, pose, &caption, &words);
	check(sought.text == state.text && samePose(sought.xf, state.xf), "seeking backward produces exactly the same caption and pose");
	check(samePose(stack.evaluatePose(context, pose).xf, pose), "caption animation does not alter a non-text clip");
	highlight.props[QStringLiteral("mode")] = 1;
	ComponentStack oneWord({highlight, punch}, registry);
	context.tMs = 560;
	check(std::abs(oneWord.evaluatePose(context, pose, &caption, &words).xf.scale - 2.2) < 1e-9,
	      "one-word reveals keep the complete phrase's clock for later pop pulses");
	const QString typed = QStringLiteral("one two three");
	context.durMs = 1500;
	context.tMs = 560;
	check(std::abs(oneWord.evaluatePose(context, pose, &typed).xf.scale - 2.2) < 1e-9,
	      "a hand-typed one-word reveal still pops on its second word");
	check(componentToJson(componentFromJson(componentToJson(highlight, registry), registry), registry) ==
	      componentToJson(highlight, registry) &&
	      componentToJson(componentFromJson(componentToJson(punch, registry), registry), registry) ==
	      componentToJson(punch, registry), "animation mode, colour and pop settings survive the existing project JSON format");

	registerBuiltinComponents(ComponentRegistry::instance());
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.srcEndMs = 2000;
	clip.text.text = QStringLiteral("one two");
	clip.text.fontPx = 150;
	clip.text.outlineWidth = 0;
	clip.words = {{QStringLiteral("one"), 100, 400}, {QStringLiteral("two"), 600, 1000}};
	highlight.props[QStringLiteral("mode")] = 3;
	clip.components = {highlight, punch};
	TlTrack track;
	track.clips = {clip};
	TimelineModel model;
	model.tracks = {track};
	const auto original = model;
	NoFrames frames;
	const QImage first = TimelineCompositor::compose(model, 160, QSize(640, 360), frames);
	const QImage next = TimelineCompositor::compose(model, 660, QSize(640, 360), frames);
	check(!first.isNull() && !next.isNull() && first != next, "actual composited frames show the word highlight moving");
	check(TimelineCompositor::compose(model, 160, QSize(640, 360), frames) == first,
	      "preview/export composition is deterministic after rendering another word");
	check(model == original && caption.contains(QStringLiteral("<color=#ff0000>")) && words[1].startMs == 500,
	      "rendering animations never rewrites saved text, keywords, word timing or component settings");
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
