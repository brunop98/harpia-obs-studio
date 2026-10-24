// The silence review dialog uses real audio, isolates the selected speech from
// music, leaves the model untouched until acceptance, and never dismisses a
// running worker before cancellation has completed.
#include "editor/SilenceTrimDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDataStream>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static bool writeAudio(const QString &path, bool music)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	constexpr int rate = 48000;
	constexpr int samples = rate * 6;
	QDataStream stream(&file);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4);
	stream << quint32(36 + samples * 2);
	stream.writeRawData("WAVEfmt ", 8);
	stream << quint32(16) << quint16(1) << quint16(1) << quint32(rate)
	       << quint32(rate * 2) << quint16(2) << quint16(16);
	stream.writeRawData("data", 4);
	stream << quint32(samples * 2);
	for (int index = 0; index < samples; ++index) {
		const int ms = index * 1000 / rate;
		const bool speaking = music || ms < 500 || (ms >= 1500 && ms < 2000);
		const qint16 sample = speaking ? qint16(9000 * std::sin(index * 2.0 * 3.141592653589793 * (music ? 300 : 700) / rate)) : 0;
		stream << sample;
	}
	return stream.status() == QDataStream::Ok;
}

static TimelineModel fixture()
{
	TlClip speech;
	speech.sourceId = 1;
	speech.srcEndMs = 3000;
	speech.outStartMs = 1000;
	speech.fadeInMs = speech.fadeOutMs = 0;
	TlTrack narration;
	narration.kind = TlTrack::Kind::Audio;
	narration.clips = {speech};
	TlClip music = speech;
	music.sourceId = 2;
	music.outStartMs = 0;
	music.srcEndMs = 6000;
	TlTrack bed = narration;
	bed.solo = true; // selecting speech must not be silenced by this unrelated solo
	bed.clips = {music};
	TimelineModel model;
	model.tracks = {narration, bed};
	return model;
}

static bool waitForAnalysis(SilenceTrimDialog &dialog)
{
	QElapsedTimer timer;
	timer.start();
	auto *progress = dialog.findChild<QProgressBar *>(QStringLiteral("silenceProgress"));
	// The initial analysis is scheduled for the next event pass.
	QApplication::processEvents();
	while (!progress->isHidden() && timer.elapsed() < 15000) {
		QApplication::processEvents();
		QThread::msleep(2);
	}
	return progress->isHidden();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir directory;
	const QString speech = directory.filePath(QStringLiteral("speech.wav"));
	const QString music = directory.filePath(QStringLiteral("music.wav"));
	check(writeAudio(speech, false) && writeAudio(music, true), "real speech-like and music fixtures are created");
	const auto lookup = [speech, music](int id) { return id == 1 ? speech : id == 2 ? music : QString(); };
	const TimelineModel before = fixture();
	{
		SilenceTrimDialog dialog(before, lookup, {{0, 0}});
		dialog.show();
		check(waitForAnalysis(dialog), "selected speech analysis completes asynchronously");
		auto *selected = dialog.findChild<QCheckBox *>(QStringLiteral("silenceSelectedOnly"));
		auto *list = dialog.findChild<QListWidget *>(QStringLiteral("silenceCandidates"));
		auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("silenceApply"));
		auto *preview = dialog.findChild<QPushButton *>(QStringLiteral("silencePreview"));
		auto *reanalyze = dialog.findChild<QPushButton *>(QStringLiteral("silenceReanalyze"));
		check(selected->isChecked() && list->count() == 1,
		      "selected speech finds its internal pause despite an unrelated solo music track");
		const auto cuts = dialog.selectedCuts();
		check(cuts.size() == 1 && cuts[0].fromMs >= 1500 && cuts[0].toMs <= 2500 && cuts[0].durationMs() >= 800,
		      "proposed pauses retain original positions and exclude selected-clip edges and outside gaps");
		check(apply->isEnabled() && preview->isEnabled(), "reviewed, valid cuts can be previewed and applied");
		int previews = 0, stopped = 0;
		QVector<TlSpan> requested;
		QObject::connect(&dialog, &SilenceTrimDialog::previewRequested, [&](const QVector<TlSpan> &ranges) {
			++previews;
			requested = ranges;
		});
		QObject::connect(&dialog, &SilenceTrimDialog::previewStopped, [&]() { ++stopped; });
		preview->click();
		check(previews == 1 && requested == cuts, "Preview hands the editor exactly the checked cut ranges");
		list->item(0)->setCheckState(Qt::Unchecked);
		check(stopped > 0 && dialog.selectedCuts().isEmpty() && !apply->isEnabled() && !preview->isEnabled(),
		      "unchecking every pause stops preview and prevents an empty apply");
		list->item(0)->setCheckState(Qt::Checked);
		check(apply->isEnabled() && dialog.selectedCuts() == cuts, "a pause can be restored without re-decoding the source");
		check(before == fixture(), "analysis and preview do not mutate the supplied timeline");
		selected->setChecked(false);
		reanalyze->click();
		check(waitForAnalysis(dialog) && list->count() == 0 && !apply->isEnabled(),
		      "whole-timeline analysis honors its audible music instead of detecting the muted-out speech");
		selected->setChecked(true);
		reanalyze->click();
		check(waitForAnalysis(dialog) && list->count() == 1, "switching back to selected speech restores its pause candidates");
		dialog.findChild<QDoubleSpinBox *>(QStringLiteral("silenceMinPause"))->setValue(2.0);
		reanalyze->click();
		check(waitForAnalysis(dialog) && list->count() == 0, "minimum pause changes update the candidate list");
		dialog.findChild<QDoubleSpinBox *>(QStringLiteral("silenceMinPause"))->setValue(0.5);
		reanalyze->click();
		check(waitForAnalysis(dialog) && list->count() == 1, "the cached audio can be re-analyzed with the original settings");
		apply->click();
		check(dialog.result() == QDialog::Accepted && !dialog.isVisible() && dialog.selectedCuts() == cuts,
		      "Apply accepts only the chosen ranges and leaves the actual edit to the editor");
	}
	{
		TimelineModel locked = before;
		locked.tracks[1].locked = true;
		SilenceTrimDialog dialog(locked, lookup, {{0, 0}});
		dialog.show();
		check(waitForAnalysis(dialog), "analysis still permits reviewing candidates around a locked track");
		check(dialog.findChild<QListWidget *>(QStringLiteral("silenceCandidates"))->count() == 1 &&
		      !dialog.findChild<QPushButton *>(QStringLiteral("silenceApply"))->isEnabled(),
		      "cuts that would move a locked track are not applied");
		check(dialog.findChild<QLabel *>(QStringLiteral("silenceStatus"))->text().contains(QStringLiteral("lock"), Qt::CaseInsensitive),
		      "the reason a candidate cannot be applied is visible");
		dialog.reject();
	}
	{
		TimelineModel noAudio;
		TlTrack track;
		TlClip caption;
		caption.type = TlClip::Type::Text;
		caption.srcEndMs = 3000;
		track.clips = {caption};
		noAudio.tracks = {track};
		SilenceTrimDialog dialog(noAudio, lookup, {});
		dialog.show();
		check(waitForAnalysis(dialog) && dialog.selectedCuts().isEmpty(), "a source-free caption timeline produces no cuts");
		check(!dialog.findChild<QPushButton *>(QStringLiteral("silenceApply"))->isEnabled() &&
		      dialog.findChild<QLabel *>(QStringLiteral("silenceStatus"))->text().contains(QStringLiteral("No usable audio")),
		      "missing audio is explained without offering to erase the project");
		dialog.reject();
	}
	{
		SilenceTrimDialog dialog(before, lookup, {{0, 0}});
		dialog.show();
		check(waitForAnalysis(dialog), "cancellation fixture is ready");
		dialog.findChild<QPushButton *>(QStringLiteral("silenceReanalyze"))->click();
		dialog.reject();
		check(dialog.isVisible() && !dialog.findChild<QPushButton *>(QStringLiteral("silenceCancel"))->isEnabled() &&
		      dialog.findChild<QLabel *>(QStringLiteral("silenceStatus"))->text().startsWith(QStringLiteral("Canceling")),
		      "Cancel remains visible while the analysis worker is stopping");
		check(waitForAnalysis(dialog) && !dialog.isVisible() && dialog.result() == QDialog::Rejected,
		      "cancellation dismisses only after worker completion");
	}
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
