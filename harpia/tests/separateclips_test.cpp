#include "editor/timeline/SeparateClips.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "editor/timeline/TimelineView.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why)
{
	std::printf("%s %s\n", pass ? "PASS" : "FAIL", why);
	failures += !pass;
}
static TlClip clip(int source, qint64 at, qint64 start, qint64 end)
{
	TlClip c; c.sourceId = source; c.outStartMs = at; c.srcStartMs = start; c.srcEndMs = end;
	return c;
}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaTests"));
	QCoreApplication::setApplicationName(QStringLiteral("separateclips"));
	QSettings().clear();
	TimelineModel original;
	TlTrack pictures;
	pictures.locked = true; pictures.hidden = true;
	TlClip primary = clip(1, 2000, 1000, 5000);
	primary.speed = 2; primary.group = 9; primary.link = 7; primary.opacity = 0.6;
	primary.crop = QRect(5, 6, 100, 80);
	primary.transition.enabled = true;
	TlKeyframe key; key.tMs = 500;
	primary.keys = {key};
	primary.scripts = {{QStringLiteral("pulse"), {}}};
	TlClip grouped = clip(2, 2500, 0, 2000); grouped.group = 9;
	TlClip caption = clip(0, 50, 0, 1000); caption.type = TlClip::Type::Text;
	pictures.clips = {primary, grouped, caption};
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio; audio.gain = 0.8;
	TlClip partner = clip(1, 1000, 500, 6500); partner.link = 7; partner.volume = 0.75;
	key.tMs = 2000; partner.keys = {key};
	audio.clips = {partner, clip(3, 0, 0, 10000)};
	TlTrack effect;
	effect.kind = TlTrack::Kind::Effect; effect.solo = true;
	TlClip grade; grade.type = TlClip::Type::Effect; grade.srcEndMs = 10000;
	effect.clips = {grade};
	original.tracks = {pictures, audio, effect};
	original.videoEndMs = 1000;
	TlSoundRule rule; rule.id = 1; rule.sourceId = 3; rule.trigger = TlSoundRule::Trigger::AnyClipStarts;
	original.soundRules = {rule};
	const TimelineModel before = original;
	const auto chosen = separate_clips::targets(original, {{0, 1}, {0, 0}, {1, 0}, {2, 0}, {0, 2}, {-1, 0}, {0, 0}});
	ok(chosen == QVector<QPair<int, int>>{{0, 2}, {0, 0}, {0, 1}},
	   "explicit picture targets are deduplicated and sorted by timeline time");
	const auto isolated = separate_clips::isolate(original, {0, 0});
	ok(isolated.tracks[0].clips.size() == 1 && isolated.tracks[1].clips.size() == 1 && isolated.tracks[2].clips.isEmpty(),
	   "one clip plus linked audio excludes grouped partners, background music, text and effect layers");
	const auto &c = isolated.tracks[0].clips[0];
	ok(c.outStartMs == 0 && c.srcStartMs == 1000 && c.srcEndMs == 5000 && c.speed == 2,
	   "export retains source trims and speed and starts at zero");
	ok(isolated.videoEndMs == 2000 && isolated.durationMs() == 2000,
	   "clip's full duration is exported even beyond the project end marker");
	ok(c.opacity == primary.opacity && c.crop == primary.crop && c.keys == primary.keys && c.scripts == primary.scripts,
	   "per-clip transforms, crop, effects and animation survive isolation");
	ok(!c.transition.enabled && !isolated.tracks[0].hidden && isolated.tracks[0].locked,
	   "locked/hidden selections can export without importing a neighbor's transition");
	const auto &sound = isolated.tracks[1].clips[0];
	ok(sound.srcStartMs == 1500 && sound.srcEndMs == 3500 && sound.outStartMs == 0,
	   "linked audio is sliced and rebased to the selected clip's own span");
	ok(sound.keys[0].tMs == 1000 && sound.volume == 0.75 && isolated.tracks[1].gain == 0.8,
	   "linked audio animation and levels stay aligned");
	ok(!isolated.tracks[2].solo && isolated.soundRules.isEmpty(), "unrelated solo flags and project sound rules stay out");
	ok(original == before, "preparing exports never changes editor data");
	ok(separate_clips::isolate(original, {1, 0}).isEmpty(), "audio-only rows do not create black video files");
	TimelineView view;
	view.setModel(original);
	view.selectClip(0, 0);
	ok(separate_clips::targets(original, view.selectedPairs()) == QVector<QPair<int, int>>{{0, 0}, {0, 1}},
	   "normal group selection exports each selected picture member separately");
	TlSelection individual = view.selectionState();
	individual.individual = true;
	view.setSelectionState(individual);
	ok(separate_clips::targets(original, view.selectedPairs()) == QVector<QPair<int, int>>{{0, 0}},
	   "individual group-member selection exports only that picture clip");
	ok(separate_clips::nameError({"Run_01", "Green hero", "scene.v2"}).isEmpty(), "portable unique names allow spaces and version dots");
	ok(!separate_clips::nameError({"Run", "run"}).isEmpty(), "case-insensitive filename collisions are rejected");
	ok(!separate_clips::nameError({"../escape"}).isEmpty() && !separate_clips::nameError({"folder/name"}).isEmpty(),
	   "file names cannot escape the new batch folder");
	ok(!separate_clips::nameError({""}).isEmpty() && !separate_clips::nameError({"CON"}).isEmpty() &&
	   !separate_clips::nameError({"name."}).isEmpty(), "empty and Windows-invalid names are rejected");

	ExportOptionsDialog::Context ctx;
	ctx.defaultName = QStringLiteral("Library"); ctx.defaultFolder = QDir::tempPath();
	ctx.sourceSize = QSize(160, 90); ctx.fps = 30; ctx.seconds = 2;
	ctx.separateClips = {{QStringLiteral("Red"), 1000, 500}, {QStringLiteral("Green"), 3000, 1500}};
	ExportOptionsDialog dlg(ctx);
	dlg.show(); app.processEvents();
	auto *names = dlg.findChild<QTableWidget *>(QStringLiteral("separateClipNames"));
	auto *prefix = dlg.findChild<QLineEdit *>(QStringLiteral("exportBaseName"));
	ok(names && prefix && dlg.windowTitle() == QStringLiteral("Export clips separately"), "dedicated window exposes shared settings and per-clip names");
	if (!names || !prefix) return 2;
	ok(dlg.format() == ClipExporter::Format::Mp4 && dlg.findChild<QSlider *>(QStringLiteral("exportQuality"))->isVisible(),
	   "new clip batches start in MP4 with quality controls visible");
	ok(dlg.separateNames() == QStringList{"Library_01", "Library_02"}, "shared name creates numbered defaults for every clip");
	prefix->setText(QStringLiteral("Database"));
	names->item(1, 3)->setText(QStringLiteral("Hero green"));
	prefix->setText(QStringLiteral("Scenes"));
	ok(dlg.separateNames() == QStringList{"Scenes_01", "Hero green"}, "changing shared name preserves a manually edited individual name");
	dlg.findChild<QPushButton *>(QStringLiteral("applySeparateNames"))->click();
	ok(dlg.separateNames() == QStringList{"Scenes_01", "Scenes_02"}, "Apply shared name can intentionally reset every row");
	names->item(1, 3)->setText(QStringLiteral("scenes_01"));
	dlg.accept();
	ok(dlg.result() != QDialog::Accepted && !dlg.findChild<QLabel *>(QStringLiteral("separateNamesError"))->text().isEmpty(),
	   "duplicate names cannot bypass validation with Enter or Ctrl+S");
	prefix->setText(QStringLiteral("CON"));
	dlg.accept();
	ok(dlg.result() != QDialog::Accepted && dlg.findChild<QLabel *>(QStringLiteral("separateNamesError"))->text().contains(QStringLiteral("folder")),
	   "shared names reserved for Windows system folders are rejected before rendering");
	prefix->setText(QStringLiteral("Scenes"));
	names->item(1, 3)->setText(QStringLiteral("Unique green"));
	auto *res = dlg.findChild<QComboBox *>(QStringLiteral("exportResolution"));
	res->setCurrentIndex(res->findData(-1));
	dlg.findChild<QSpinBox *>(QStringLiteral("exportWidth"))->setValue(128);
	dlg.findChild<QSpinBox *>(QStringLiteral("exportHeight"))->setValue(72);
	dlg.findChild<QSlider *>(QStringLiteral("exportQuality"))->setValue(90);
	ok(dlg.outputSize() == QSize(128, 72) && dlg.videoCrf() < 20 && dlg.exportSeconds() == 2,
	   "quality and custom resolution settings apply to the batch's total duration");
	if (argc > 1) dlg.grab().save(QString::fromLocal8Bit(argv[1]));
	dlg.accept();
	ok(dlg.result() == QDialog::Accepted && dlg.separateNames()[1] == QStringLiteral("Unique green"), "unique individual names submit unchanged");
	QSettings().clear();
	return failures ? 1 : 0;
}
