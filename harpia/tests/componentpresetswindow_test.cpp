// Saved component settings survive a window/project change and apply through
// the real Inspector wiring, including batch edits, identities and undo.
#include <QtWidgets>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#define private public
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "editor/component/ComponentJson.hpp"
#include "editor/component/ComponentPresets.hpp"
#include "editor/component/ComponentPresetClip.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "ui/SearchPicker.hpp"

#include <cmath>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
void settle()
{
	QApplication::processEvents();
}
ComponentInstance rotation(const QString &id, double rate)
{
	ComponentInstance instance;
	instance.typeId = QStringLiteral("harpia.alwaysRotate");
	instance.instanceId = id;
	instance.props.insert(QStringLiteral("degreesPerSecond"), rate);
	return instance;
}
TlClip caption(qint64 position = 0)
{
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.srcEndMs = 1000;
	clip.outStartMs = position;
	clip.text.text = QStringLiteral("Caption");
	return clip;
}
void model(VideoEditorWindow &window, const TimelineModel &timeline, const TlSelection &selection)
{
	window.setEditMode(VideoEditorWindow::EditMode::Full);
	window.timelineView_->setModel(timeline);
	window.timelineView_->setSelectionState(selection);
	window.commitSnapshot();
	settle();
}
QMenu *visibleMenu()
{
	if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
		return menu;
	for (QWidget *widget : QApplication::topLevelWidgets())
		if (auto *menu = qobject_cast<QMenu *>(widget); menu && menu->isVisible())
			return menu;
	return nullptr;
}
bool saveThroughPanel(VideoEditorWindow &window, const QString &type, int ordinal, const QString &name,
		      QMessageBox::StandardButton replace = QMessageBox::No)
{
	bool answered = false, menuChosen = false, timedOut = false;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer responder;
	QObject::connect(&responder, &QTimer::timeout, [&]() {
		if (elapsed.elapsed() > 5000) {
			timedOut = true;
			if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
				dialog->reject();
			if (auto *menu = visibleMenu())
				menu->close();
			return;
		}
		if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
			dialog->setTextValue(name);
			answered = true;
			dialog->accept();
			return;
		}
		if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			if (QAbstractButton *button = dialog->button(replace))
				button->click();
			else
				dialog->reject();
			return;
		}
		if (menuChosen)
			return;
		if (auto *menu = visibleMenu()) {
			auto *save = menu->findChild<QAction *>(QStringLiteral("saveComponentPreset"));
			if (!save)
				return;
			menuChosen = true;
			// Queue the trigger so the polling timer can answer the dialog
			// created by that action inside its own nested event loop.
			QTimer::singleShot(0, menu, [menu, save]() { save->trigger(); menu->close(); });
		}
	});
	responder.start(10);
	window.componentPanel_->componentMenu(type, ordinal);
	return answered && menuChosen && !timedOut;
}
QString presetId(const QString &name)
{
	QString error;
	const auto presets = component_presets::Store().load(&error);
	if (!error.isEmpty())
		return {};
	for (const auto &preset : presets)
		if (preset.name == name)
			return preset.id;
	return {};
}
bool choosePresetFromAdd(ComponentPanel *panel, const QString &name)
{
	bool chosen = false;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer responder;
	QObject::connect(&responder, &QTimer::timeout, [&]() {
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			auto *picker = dynamic_cast<SearchPicker *>(widget);
			if (!picker || !picker->isVisible())
				continue;
			if (elapsed.elapsed() > 5000) {
				picker->close();
				return;
			}
			picker->searchField()->setText(name);
			const auto names = picker->visibleNames();
			chosen = !names.isEmpty() && names.front().startsWith(name);
			QKeyEvent key(QEvent::KeyPress, chosen ? Qt::Key_Return : Qt::Key_Escape, Qt::NoModifier);
			QApplication::sendEvent(picker->searchField(), &key);
			return;
		}
	});
	responder.start(10);
	panel->addComponentMenu();
	return chosen;
}
bool choosePresetFromComponentMenu(ComponentPanel *panel, const QString &type, int ordinal,
				   const QString &submenuName, const QString &id,
				   QMessageBox::StandardButton confirmation = QMessageBox::No)
{
	bool chosen = false, timedOut = false;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer responder;
	QObject::connect(&responder, &QTimer::timeout, [&]() {
		if (elapsed.elapsed() > 5000) {
			timedOut = true;
			if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
				dialog->reject();
			if (auto *menu = visibleMenu())
				menu->close();
			return;
		}
		if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			if (QAbstractButton *button = dialog->button(confirmation))
				button->click();
			else
				dialog->reject();
			return;
		}
		if (chosen)
			return;
		if (auto *menu = visibleMenu())
			if (auto *submenu = menu->findChild<QMenu *>(submenuName))
				for (auto *action : submenu->actions())
					if (action->data().toString() == id) {
						chosen = true;
						QTimer::singleShot(0, menu, [menu, action]() { action->trigger(); menu->close(); });
						return;
					}
	});
	responder.start(10);
	panel->componentMenu(type, ordinal);
	return chosen && !timedOut;
}
bool tone(const QString &path, double frequency)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	QDataStream stream(&file);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4);
	stream << quint32(36 + 48000 * 2);
	stream.writeRawData("WAVEfmt ", 8);
	stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
	stream.writeRawData("data", 4);
	stream << quint32(48000 * 2);
	for (int i = 0; i < 48000; ++i)
		stream << qint16(std::sin(i * 2.0 * 3.141592653589793 * frequency / 48000.0) * 10000.0);
	return stream.status() == QDataStream::Ok;
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	Q_UNUSED(argc);
	Q_UNUSED(argv);
	std::printf("SKIP: native application settings cannot be redirected safely on this platform\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	qputenv("XDG_CACHE_HOME", temporary.filePath("cache").toUtf8());
	qputenv("XDG_DATA_HOME", temporary.filePath("data").toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaComponentPresetTests"));
	QCoreApplication::setApplicationName(QStringLiteral("ComponentPresets"));
	// Qt's test-mode AppConfigLocation can ignore XDG_CONFIG_HOME and use a
	// shared ~/.qttest directory. The XDG roots above isolate all fixture data.
	QStandardPaths::setTestModeEnabled(false);
	for (QSettings::Format format : {QSettings::IniFormat, QSettings::NativeFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.filePath("system"));
	}
	const QString type = QStringLiteral("harpia.alwaysRotate");
	ComponentInstance saved = rotation(QStringLiteral("source-second"), 48);
	saved.enabled = false;
	saved.inMs = 120;
	saved.outMs = 240;
	PropKey first, second;
	first.tMs = 0;
	first.v = 12;
	first.ease = TlEase::Bezier;
	first.bez1 = 0.1;
	first.bez2 = 0.7;
	second.tMs = 800;
	second.v = 48;
	saved.keys[QStringLiteral("degreesPerSecond")] = {first, second};
	saved.unknown.insert(QStringLiteral("pluginExtra"), QJsonObject{{"kept", true}});
	QString id;
	{
		VideoEditorWindow source{QString()};
		TlTrack track;
		TlClip primary = caption(), peer = caption(1200);
		primary.components = {rotation(QStringLiteral("source-first"), 5), saved};
		peer.components = {rotation(QStringLiteral("peer-first"), 9), rotation(QStringLiteral("peer-second"), 72)};
		track.clips = {primary, peer};
		TimelineModel timeline;
		timeline.tracks = {track};
		model(source, timeline, TlSelection{0, 0, false, {{0, 1}}});
		check(saveThroughPanel(source, type, 1, QStringLiteral("Slow animated spin")),
		      "Save as preset opens a naming dialog through the component panel");
		id = presetId(QStringLiteral("Slow animated spin"));
		check(!id.isEmpty(), "saving a mixed selection persists the clicked component under its name");
		const auto presets = component_presets::Store().load();
		const auto stored = presets.isEmpty() ? ComponentInstance() : componentFromJson(presets.front().component, ComponentRegistry::instance());
		check(stored.props == saved.props && stored.keys == saved.keys && stored.inMs == 120 && stored.outMs == 240 &&
		      !stored.enabled && stored.unknown.value(QStringLiteral("pluginExtra")) == saved.unknown.value(QStringLiteral("pluginExtra")),
		      "preset captures stored values, animation, fades and state from the primary clip");
		check(primary.components == source.timelineView_->model().tracks[0].clips[0].components,
		      "saving a preset leaves project components untouched");
		TimelineModel changed = source.timelineView_->model();
		changed.tracks[0].clips[0].components[1].props[QStringLiteral("degreesPerSecond")] = 99;
		model(source, changed, TlSelection{0, 0, false, {}});
		check(saveThroughPanel(source, type, 1, QStringLiteral("Slow animated spin"), QMessageBox::No) &&
		      componentFromJson(component_presets::Store().load().front().component, ComponentRegistry::instance()).props == saved.props,
		      "canceling replacement keeps the existing named preset unchanged");
		check(saveThroughPanel(source, type, 1, QStringLiteral("Slow animated spin"), QMessageBox::Yes) &&
		      component_presets::Store().load().size() == 1 && presetId(QStringLiteral("Slow animated spin")) == id &&
		      componentFromJson(component_presets::Store().load().front().component, ComponentRegistry::instance()).props.value(QStringLiteral("degreesPerSecond")).toDouble() == 99,
		      "confirmed replacement updates the existing preset without creating duplicate entries");
		saved.props[QStringLiteral("degreesPerSecond")] = 99;
	}
	check(!id.isEmpty() && presetId(QStringLiteral("Slow animated spin")) == id,
	      "preset remains available after the original project window is destroyed");
	{
		VideoEditorWindow destination{QString()};
		TlTrack track, locked;
		TlClip firstClip = caption(), secondClip = caption(1200), protectedClip = caption();
		firstClip.components = {rotation(QStringLiteral("a-first"), 1), rotation(QStringLiteral("a-second"), 2)};
		secondClip.components = {rotation(QStringLiteral("b-first"), 3), rotation(QStringLiteral("b-second"), 4)};
		protectedClip.components = {rotation(QStringLiteral("locked-first"), 5), rotation(QStringLiteral("locked-second"), 6)};
		track.clips = {firstClip, secondClip};
		locked.clips = {protectedClip};
		locked.locked = true;
		TimelineModel timeline;
		timeline.tracks = {track, locked};
		model(destination, timeline, TlSelection{0, 0, false, {{0, 1}, {1, 0}}});
		bool menuContainsSaved = false;
		QTimer menuRescue;
		menuRescue.setSingleShot(true);
		QObject::connect(&menuRescue, &QTimer::timeout, [&]() { if (auto *menu = visibleMenu()) menu->close(); });
		menuRescue.start(5000);
		QTimer::singleShot(0, [&]() {
			auto *menu = visibleMenu();
			if (!menu)
				return;
			const auto *save = menu->findChild<QAction *>(QStringLiteral("saveComponentPreset"));
			const auto *apply = menu->findChild<QMenu *>(QStringLiteral("applyComponentPreset"));
			const auto *remove = menu->findChild<QMenu *>(QStringLiteral("deleteComponentPreset"));
			menuContainsSaved = save && apply && remove && !apply->actions().isEmpty() &&
				apply->actions().front()->data().toString() == id &&
				remove->actions().front()->data().toString() == id;
			menu->close();
		});
		destination.componentPanel_->componentMenu(type, 1);
		menuRescue.stop();
		check(menuContainsSaved, "component menu offers Save, Apply and Delete for the saved preset in another project");
		const auto before = destination.timelineView_->model();
		const int previousHistory = destination.histIndex_;
		check(choosePresetFromComponentMenu(destination.componentPanel_, type, 1,
			QStringLiteral("applyComponentPreset"), id), "saved preset applies through its real component menu action");
		destination.commitSnapshot();
		settle();
		const auto applied = destination.timelineView_->model();
		check(applied.tracks[0].clips[0].components[0] == firstClip.components[0] &&
		      applied.tracks[0].clips[1].components[0] == secondClip.components[0],
		      "applying to the second component leaves the first occurrence unchanged");
		check(applied.tracks[0].clips[0].components[1].props == saved.props &&
		      applied.tracks[0].clips[1].components[1].keys == saved.keys &&
		      applied.tracks[0].clips[0].components[1].inMs == 120 &&
		      applied.tracks[0].clips[1].components[1].outMs == 240 &&
		      !applied.tracks[0].clips[1].components[1].enabled,
		      "one apply updates values, animation, fades and enabled state on every unlocked selected clip");
		check(applied.tracks[0].clips[0].components[1].instanceId == QStringLiteral("a-second") &&
		      applied.tracks[0].clips[1].components[1].instanceId == QStringLiteral("b-second"),
		      "existing component identities and stack positions survive preset application");
		check(applied.tracks[1].clips == before.tracks[1].clips,
		      "applying a preset respects locked tracks in a mixed selection");
		check(destination.histIndex_ == previousHistory + 1, "batch preset application creates one undo step");
		destination.undo();
		check(destination.timelineView_->model() == before, "one Undo restores every affected component");
		destination.redo();
		check(destination.timelineView_->model() == applied, "one Redo restores the complete preset application");

		TlTrack freshTrack;
		TlClip existing = caption(), emptyA = caption(1200), emptyB = caption(2400);
		existing.components = {rotation(QStringLiteral("existing"), -20)};
		freshTrack.clips = {existing, emptyA, emptyB};
		TimelineModel fresh;
		fresh.tracks = {freshTrack};
		model(destination, fresh, TlSelection{0, 0, false, {{0, 1}, {0, 2}}});
		check(choosePresetFromAdd(destination.componentPanel_, QStringLiteral("Slow animated spin")),
		      "saved presets are searchable and selectable in the real Add Component popup");
		const auto add = destination.timelineView_->model();
		check(add.tracks[0].clips[0].components.size() == 1 &&
		      add.tracks[0].clips[0].components[0].instanceId == QStringLiteral("existing") &&
		      add.tracks[0].clips[0].components[0].props == saved.props,
		      "Add Component preset updates an existing component without doubling the effect");
		check(add.tracks[0].clips[1].components.size() == 1 && add.tracks[0].clips[2].components.size() == 1 &&
		      !add.tracks[0].clips[1].components[0].instanceId.isEmpty() &&
		      add.tracks[0].clips[1].components[0].instanceId != add.tracks[0].clips[2].components[0].instanceId &&
		      add.tracks[0].clips[1].components[0].instanceId != saved.instanceId,
		      "new components receive distinct identities on each selected clip");
	}

	{
		VideoEditorWindow window{QString()};
		TlTrack track;
		TlClip source = caption(), destination = caption(1400);
		source.posX = 0.23;
		source.posY = 0.67;
		source.scale = 1.3;
		source.rotation = 20;
		source.opacity = 0.65;
		TlKeyframe key;
		key.tMs = 250;
		key.tf = source.baseTransform();
		key.tf.posX = 0.7;
		key.pos.ease = TlEase::Bezier;
		key.pos.bez1 = 0.15;
		key.scale.on = false;
		key.curvedPath = key.handlesManual = true;
		key.inX = -0.2;
		key.outY = 0.3;
		source.keys = {key};
		destination.crop = QRect(1, 2, 3, 4);
		destination.text.text = QStringLiteral("Keep my words");
		track.clips = {source, destination};
		TimelineModel timeline;
		timeline.tracks = {track};
		model(window, timeline, TlSelection{0, 0, false, {}});
		check(saveThroughPanel(window, QStringLiteral("harpia.transform"), -1, QStringLiteral("Animated pose")),
		      "pinned Transform can be saved as a preset");
		const QString poseId = presetId(QStringLiteral("Animated pose"));
		window.timelineView_->selectClip(0, 1);
		emit window.componentPanel_->componentPresetApplied(poseId, QStringLiteral("harpia.transform"), -1);
		const auto pose = window.timelineView_->model().tracks[0].clips[1];
		check(pose.posX == source.posX && pose.posY == source.posY && pose.scale == source.scale &&
		      pose.rotation == source.rotation && pose.opacity == source.opacity && pose.keys == source.keys,
		      "Transform preset restores base pose and per-channel path animation");
		check(pose.components.isEmpty() && pose.crop == destination.crop && pose.text.text == destination.text.text &&
		      pose.outStartMs == destination.outStartMs,
		      "Transform preset preserves content, crop and timing without adding a hidden component");

		TlTrack videos;
		videos.rippleSpeed = true;
		TlClip fast;
		fast.srcEndMs = 2000;
		fast.speed = 2;
		TlClip other = fast;
		other.speed = 1;
		other.outStartMs = 1000;
		TlClip tail = other;
		tail.outStartMs = 3000;
		videos.clips = {fast, other, tail};
		TimelineModel speedTimeline;
		speedTimeline.tracks = {videos};
		model(window, speedTimeline, TlSelection{0, 0, false, {}});
		check(saveThroughPanel(window, QStringLiteral("harpia.speed"), -1, QStringLiteral("Double speed")),
		      "pinned Speed can be saved as a preset");
		window.timelineView_->selectClip(0, 1);
		window.commitSnapshot();
		emit window.componentPanel_->componentPresetApplied(presetId(QStringLiteral("Double speed")), QStringLiteral("harpia.speed"), -1);
		const auto speeds = window.timelineView_->model().tracks[0].clips;
		check(speeds[1].speed == 2 && speeds[1].components.isEmpty() && speeds[2].outStartMs == 2000,
		      "Speed preset uses the native speed field and honors automatic speed ripple");
	}

	const QString soundFile = temporary.filePath("original.wav"), unrelatedFile = temporary.filePath("unrelated.wav");
	check(tone(soundFile, 220) && tone(unrelatedFile, 880), "distinct sound files are available for cross-project identity checks");
	QString soundPreset;
	int oldSoundId = -1;
	{
		VideoEditorWindow window{QString()};
		window.nextSourceId_ = 1; // Sound components use zero as their empty choice.
		oldSoundId = window.addAudioSource(soundFile);
		ComponentInstance sound;
		sound.typeId = QStringLiteral("harpia.sound");
		sound.instanceId = QStringLiteral("sound");
		sound.props = {{QStringLiteral("inSound"), oldSoundId}, {QStringLiteral("outSound"), oldSoundId},
			       {QStringLiteral("outOn"), true}};
		TlClip clip = caption();
		clip.components = {sound};
		TlTrack track;
		track.clips = {clip};
		TimelineModel timeline;
		timeline.tracks = {track};
		model(window, timeline, TlSelection{0, 0, false, {}});
		check(oldSoundId >= 0 && saveThroughPanel(window, sound.typeId, 0, QStringLiteral("Reusable sound")),
		      "Sound component saves its selected audio for reuse in another project");
		soundPreset = presetId(QStringLiteral("Reusable sound"));
	}
	QFile::remove(soundFile);
	{
		VideoEditorWindow window{QString()};
		window.nextSourceId_ = 1;
		const int unrelatedId = window.addAudioSource(unrelatedFile);
		TlTrack track;
		track.clips = {caption()};
		TimelineModel timeline;
		timeline.tracks = {track};
		model(window, timeline, TlSelection{0, 0, false, {}});
		emit window.componentPanel_->componentPresetApplied(soundPreset, QString(), -1);
		const auto components = window.timelineView_->model().tracks[0].clips[0].components;
		const int inId = components.isEmpty() ? -1 : components[0].props.value(QStringLiteral("inSound")).toInt();
		const int outId = components.isEmpty() ? -1 : components[0].props.value(QStringLiteral("outSound")).toInt();
		const EditorSource *source = window.sourceById(inId);
		check(unrelatedId == oldSoundId && inId != unrelatedId && source && QFileInfo::exists(source->origPath),
		      "Sound preset remaps source IDs and survives removal of the original sound file");
		check(inId == outId && inId >= 0 && components[0].unknown.value(QStringLiteral("presetSoundAssets")).isUndefined(),
		      "both sound references reuse the correct imported asset and drop preset-only metadata");
		ComponentInstance libraryPreset;
		for (const auto &entry : component_presets::Store().load())
			if (entry.id == soundPreset)
				libraryPreset = componentFromJson(entry.component, ComponentRegistry::instance());
		QJsonObject soundAssets = libraryPreset.unknown.value(QStringLiteral("presetSoundAssets")).toObject();
		for (const char *field : {"inSound", "outSound"}) {
			QJsonObject asset = soundAssets.value(QLatin1String(field)).toObject();
			asset.insert(QStringLiteral("slot"), QStringLiteral("Missing preset slot"));
			soundAssets.insert(QLatin1String(field), asset);
		}
		libraryPreset.unknown.insert(QStringLiteral("presetSoundAssets"), soundAssets);
		const QString libraryPresetId = component_presets::Store().save(QStringLiteral("Library fallback"), libraryPreset, ComponentRegistry::instance());
		if (EditorSource *placeholder = window.sourceById(unrelatedId)) {
			placeholder->missing = true;
			placeholder->librarySlot = QStringLiteral("Missing preset slot");
		}
		emit window.componentPanel_->componentPresetApplied(libraryPresetId, QString(), -1);
		const auto fallback = window.timelineView_->model().tracks[0].clips[0].components[0];
		check(fallback.props.value(QStringLiteral("inSound")).toInt() == inId &&
		      fallback.props.value(QStringLiteral("outSound")).toInt() == inId,
		      "a missing library-slot placeholder falls back to the saved WAV instead of reusing its positive source ID");

		for (const char *field : {"inSound", "outSound"}) {
			QJsonObject asset = soundAssets.value(QLatin1String(field)).toObject();
			asset.insert(QStringLiteral("slot"), QString());
			asset.insert(QStringLiteral("path"), temporary.filePath("missing-preset-audio.wav"));
			soundAssets.insert(QLatin1String(field), asset);
		}
		libraryPreset.unknown.insert(QStringLiteral("presetSoundAssets"), soundAssets);
		const QString missingPresetId = component_presets::Store().save(QStringLiteral("Missing audio"), libraryPreset, ComponentRegistry::instance());
		bool warningShown = false;
		QElapsedTimer elapsed;
		elapsed.start();
		QTimer warningResponder;
		QObject::connect(&warningResponder, &QTimer::timeout, [&]() {
			if (auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
				warningShown = warning->text().contains(QStringLiteral("In sound")) && warning->text().contains(QStringLiteral("Out sound"));
				warning->accept();
			} else if (elapsed.elapsed() > 5000) {
				if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
					dialog->reject();
			}
		});
		warningResponder.start(10);
		emit window.componentPanel_->componentPresetApplied(missingPresetId, QString(), -1);
		warningResponder.stop();
		const auto missing = window.timelineView_->model().tracks[0].clips[0].components[0];
		check(warningShown && missing.props.value(QStringLiteral("inSound")).toInt() == 0 &&
		      missing.props.value(QStringLiteral("outSound")).toInt() == 0,
		      "missing preset assets clear project-local sound IDs and explain which sounds need replacing");
	}
	{
		VideoEditorWindow window{QString()};
		TlClip clip = caption();
		clip.components = {rotation(QStringLiteral("keep-project-instance"), 20)};
		TlTrack track;
		track.clips = {clip};
		TimelineModel timeline;
		timeline.tracks = {track};
		model(window, timeline, TlSelection{0, 0, false, {}});
		const QString removeId = component_presets::Store().save(QStringLiteral("Temporary remove"), saved, ComponentRegistry::instance());
		const auto before = window.timelineView_->model();
		check(choosePresetFromComponentMenu(window.componentPanel_, type, 0,
			QStringLiteral("deleteComponentPreset"), removeId, QMessageBox::No) &&
		      presetId(QStringLiteral("Temporary remove")) == removeId,
		      "canceling preset deletion retains the saved entry");
		check(choosePresetFromComponentMenu(window.componentPanel_, type, 0,
			QStringLiteral("deleteComponentPreset"), removeId, QMessageBox::Yes) &&
		      presetId(QStringLiteral("Temporary remove")).isEmpty() && window.timelineView_->model() == before,
		      "confirmed preset deletion removes only that entry and preserves project components");
	}
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
