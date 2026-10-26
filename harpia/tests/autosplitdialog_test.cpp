#include "ui/AutoSplitDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void check(bool value, const char *message)
{
	std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
	if (!value)
		++failures;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QTemporaryDir temporary;
	check(temporary.isValid(), "settings fixture has a private temporary directory");
	QSettings saved(temporary.filePath(QStringLiteral("autosplit.ini")), QSettings::IniFormat);
	{
		const AutoSplitSettings defaults = auto_split_settings::load(saved);
		check(!defaults.enabled && defaults.mode == AutoSplitSettings::Mode::Time && defaults.minutes == 30 &&
			      defaults.sizeMB == 500,
		      "existing installs default to splitting off with 30 minute/500 MB choices");
		AutoSplitSettings value;
		value.enabled = true;
		value.mode = AutoSplitSettings::Mode::Size;
		value.minutes = 5;
		value.sizeMB = 49;
		auto_split_settings::save(saved, value);
		saved.sync();
		QSettings reopened(saved.fileName(), QSettings::IniFormat);
		const AutoSplitSettings roundTrip = auto_split_settings::load(reopened);
		check(roundTrip.enabled && roundTrip.mode == AutoSplitSettings::Mode::Size && roundTrip.minutes == 5 &&
			      roundTrip.sizeMB == 49,
		      "saved Always settings survive reopening the settings file");
		reopened.setValue(QStringLiteral("recording/autoSplit/mode"), QStringLiteral("unknown"));
		reopened.setValue(QStringLiteral("recording/autoSplit/minutes"), -100);
		reopened.setValue(QStringLiteral("recording/autoSplit/sizeMB"), 2147483647);
		const AutoSplitSettings bounded = auto_split_settings::load(reopened);
		check(bounded.mode == AutoSplitSettings::Mode::Time && bounded.minutes == 1 &&
			      bounded.sizeMB == auto_split_settings::maxSizeMB,
		      "invalid stored modes and out-of-range limits cannot reach the recorder");
		reopened.setValue(QStringLiteral("recording/autoSplit/minutes"), QStringLiteral("broken"));
		reopened.setValue(QStringLiteral("recording/autoSplit/sizeMB"), QStringLiteral("999999999999999999"));
		const AutoSplitSettings malformed = auto_split_settings::load(reopened);
		check(malformed.minutes == 30 && malformed.sizeMB == 500,
		      "malformed and overflowing stored integers fall back to useful defaults");
	}
	saved.clear();
	{
		AutoSplitSettings defaults;
		defaults.enabled = true;
		AutoSplitState state(defaults);
		check(!state.hasOverride() && state.next().enabled && state.next().minutes == 30,
		      "the next recording starts with the persistent default");
		AutoSplitSettings once;
		once.enabled = true;
		once.mode = AutoSplitSettings::Mode::Size;
		once.sizeMB = 49;
		state.apply(once, false);
		check(state.hasOverride() && state.next().mode == AutoSplitSettings::Mode::Size &&
			      state.next().sizeMB == 49 && state.defaults().mode == AutoSplitSettings::Mode::Time,
		      "Only this time overrides the next recording without changing the saved default");
		// Failed starts and canceled countdowns do not call recordingStarted.
		check(state.hasOverride() && state.next().sizeMB == 49 && state.next().sizeMB == 49,
		      "reading settings for unsuccessful attempts does not consume the one-time choice");
		state.recordingStarted();
		check(!state.hasOverride() && state.next().mode == AutoSplitSettings::Mode::Time &&
			      state.next().minutes == 30,
		      "a successful recording consumes the override and restores the saved default");
		once.enabled = false;
		state.apply(once, false);
		check(state.hasOverride() && !state.next().enabled && state.defaults().enabled,
		      "a disabled one-time choice temporarily bypasses enabled Always splitting");
		state.recordingStarted();
		check(state.next().enabled && !state.hasOverride(), "the following recording uses enabled Always again");
		state.apply(once, false);
		AutoSplitSettings replacement;
		replacement.enabled = true;
		replacement.minutes = 5;
		state.apply(replacement, true);
		check(!state.hasOverride() && state.next().enabled && state.next().minutes == 5 &&
			      state.defaults().minutes == 5,
		      "a new Always choice replaces the saved default and clears an older one-time choice");
		state.recordingStarted();
		check(state.next().minutes == 5, "successful recordings never consume an Always preference");
	}
	AutoSplitSettings start;
	start.enabled = true;
	start.minutes = 30;
	AutoSplitDialog dialog(start, true);
	auto *enabled = dialog.findChild<QCheckBox *>(QStringLiteral("autoSplitEnabled"));
	auto *time = dialog.findChild<QRadioButton *>(QStringLiteral("autoSplitTime"));
	auto *size = dialog.findChild<QRadioButton *>(QStringLiteral("autoSplitSize"));
	auto *minutes = dialog.findChild<QSpinBox *>(QStringLiteral("autoSplitMinutes"));
	auto *sizeMB = dialog.findChild<QSpinBox *>(QStringLiteral("autoSplitSizeMB"));
	auto *once = dialog.findChild<QRadioButton *>(QStringLiteral("autoSplitOnce"));
	auto *always = dialog.findChild<QRadioButton *>(QStringLiteral("autoSplitAlways"));
	auto *buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("autoSplitButtons"));
	check(enabled && time && size && minutes && sizeMB && once && always && buttons,
	      "all Auto Split controls have discoverable identities");
	if (!enabled || !time || !size || !minutes || !sizeMB || !once || !always || !buttons)
		return 1;
	check(time->isChecked() && always->isChecked() && !size->isChecked() && !once->isChecked(),
	      "limit and persistence choices form independent radio groups");
	check(minutes->isEnabled() && !sizeMB->isEnabled(), "only the selected limit is editable");
	size->click();
	check(size->isChecked() && !time->isChecked() && always->isChecked(),
	      "selecting size keeps the saved scope and deselects time");
	check(!minutes->isEnabled() && sizeMB->isEnabled(), "size mode enables only the size input");
	sizeMB->setValue(49);
	once->click();
	check(!dialog.always() && size->isChecked() && !always->isChecked(),
	      "Only this time does not change the limit mode");
	check(dialog.settings().enabled && dialog.settings().mode == AutoSplitSettings::Mode::Size &&
		      dialog.settings().sizeMB == 49 && dialog.settings().minutes == 30,
	      "dialog returns the chosen limit and preserves the inactive limit");
	enabled->click();
	check(!time->isEnabled() && !size->isEnabled() && !minutes->isEnabled() && !sizeMB->isEnabled(),
	      "turning splitting off disables both limit inputs");
	check(once->isEnabled() && always->isEnabled(), "turning splitting off still permits saving Always");
	always->click();
	check(dialog.always() && !dialog.settings().enabled,
	      "Always can persist a disabled setting, rather than trapping splitting on");
	enabled->click();
	check(sizeMB->isEnabled() && dialog.settings().sizeMB == 49,
	      "turning splitting back on restores the previous mode and limit");
	time->click();
	minutes->setValue(0);
	check(dialog.settings().minutes == 1, "a zero-minute limit is impossible");
	minutes->setValue(2147483647);
	check(dialog.settings().minutes == auto_split_settings::maxMinutes, "excessive minute limits are bounded");
	size->click();
	sizeMB->setValue(-1);
	check(dialog.settings().sizeMB == 1, "a negative file-size limit is impossible");
	sizeMB->setValue(2147483647);
	check(dialog.settings().sizeMB == auto_split_settings::maxSizeMB, "excessive file-size limits are bounded");
	check(!minutes->accessibleName().isEmpty() && !sizeMB->accessibleName().isEmpty(),
	      "screen readers can identify both numeric inputs");
	const auto *help = dialog.findChild<QLabel *>(QStringLiteral("autoSplitHelp"));
	check(help && help->text().contains(QStringLiteral("exceed")) &&
		      help->text().contains(QStringLiteral("Paused time")) &&
		      help->text().contains(QStringLiteral("webcam")),
	      "dialog explains keyframe limits, excluded pauses and separate webcam behavior");
	buttons->button(QDialogButtonBox::Cancel)->click();
	check(dialog.result() == QDialog::Rejected && saved.allKeys().isEmpty(),
	      "Cancel dismisses without writing persistent or one-time settings");
	AutoSplitDialog accepted(start, false);
	accepted.findChild<QDialogButtonBox *>(QStringLiteral("autoSplitButtons"))
		->button(QDialogButtonBox::Ok)->click();
	check(accepted.result() == QDialog::Accepted && !accepted.always(),
	      "OK returns accepted and preserves an incoming Only this time choice");
	check(saved.allKeys().isEmpty(), "accepting the dialog leaves persistence to the main window");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
