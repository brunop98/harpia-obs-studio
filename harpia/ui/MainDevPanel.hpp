#pragma once

#include <QDialog>

class QSpinBox;

namespace harpia {

class MainWindow;
struct MainLayoutParams;

// Developer Panel for the main recorder window: exposes every layout size
// (margins, spacings, combo/label widths, button and preview sizes, recent-card
// metrics…) as live spin boxes that apply to the running window immediately.
// Every change is auto-saved (QSettings) and restored on the next launch.
class MainDevPanel : public QDialog {
	Q_OBJECT
public:
	MainDevPanel(MainWindow *win, QWidget *parent = nullptr);

	// Persisted tweaks: the window calls loadInto() at startup; the panel calls
	// saveFrom() after every change.
	static void loadInto(MainLayoutParams &p);
	static void saveFrom(const MainLayoutParams &p);

private:
	void apply();
	void resetDefaults();

	MainWindow *win_ = nullptr;
	bool loading_ = false; // guard: programmatic setValue must not re-apply

	// Window
	QSpinBox *rootMarginH_ = nullptr;
	QSpinBox *rootMarginV_ = nullptr;
	QSpinBox *rootSpacing_ = nullptr;
	// Toolbar
	QSpinBox *row1Spacing_ = nullptr;
	QSpinBox *presetComboW_ = nullptr;
	QSpinBox *monitorMinW_ = nullptr;
	QSpinBox *monitorMaxW_ = nullptr;
	// Behavior column
	QSpinBox *behaviorComboW_ = nullptr;
	QSpinBox *behaviorLabelW_ = nullptr;
	QSpinBox *behaviorColSpacing_ = nullptr;
	QSpinBox *behaviorRowSpacing_ = nullptr;
	QSpinBox *middleSpacing_ = nullptr;
	// Record controls
	QSpinBox *controlsSpacing_ = nullptr;
	QSpinBox *recordBtnW_ = nullptr;
	QSpinBox *recordBtnH_ = nullptr;
	QSpinBox *pauseBtnW_ = nullptr;
	QSpinBox *pauseBtnH_ = nullptr;
	QSpinBox *separatorH_ = nullptr;
	QSpinBox *webcamPreviewW_ = nullptr;
	QSpinBox *webcamPreviewH_ = nullptr;
	// Recent strip
	QSpinBox *stripThumbW_ = nullptr;
	QSpinBox *stripThumbH_ = nullptr;
	QSpinBox *stripCardExtraW_ = nullptr;
	QSpinBox *stripCardExtraH_ = nullptr;
	QSpinBox *stripSpacing_ = nullptr;
};

} // namespace harpia
