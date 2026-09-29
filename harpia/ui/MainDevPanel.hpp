#pragma once

#include <QDialog>
#include <QPair>
#include <QVector>

#include "AreaStyle.hpp"
#include "MainLayoutParams.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace harpia {

class MainWindow;

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
	// The Multi-Area overlay's look (Areas tab), persisted the same way.
	static void loadAreaStyle(AreaStyle &s);
	static void saveAreaStyle(const AreaStyle &s);

protected:
	void hideEvent(QHideEvent *e) override; // ends the on-screen preview

private:
	void apply();
	void resetDefaults();
	void copyJson();
	void pasteJson();
	MainLayoutParams current() const;        // what the boxes say
	void showValues(const MainLayoutParams &p); // put values in the boxes, then apply

	QWidget *buildAreasTab();
	AreaStyle currentAreaStyle() const;
	void showAreaStyle(const AreaStyle &s); // into the controls, then apply
	void applyAreaStyle();
	static void paintSwatch(QPushButton *b, const QColor &c);

	QTabWidget *tabs_ = nullptr;
	QWidget *areasTab_ = nullptr;
	// Areas tab: one control per AreaStyle field, in areaStyleFields() order.
	QVector<QSpinBox *> areaSpins_;       // null for colour fields
	QVector<QPushButton *> areaSwatches_; // null for integer fields
	QVector<QColor> areaColors_;          // the swatches' colours
	QComboBox *lineStyleCombo_ = nullptr; // stands in for the lineStyle spin
	QComboBox *areaPreview_ = nullptr;    // Off / Idle / Recording

	QVector<QPair<QSpinBox *, int MainLayoutParams::*>> spins_;
	QLabel *jsonStatus_ = nullptr;

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
