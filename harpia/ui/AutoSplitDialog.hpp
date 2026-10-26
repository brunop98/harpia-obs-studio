#pragma once

#include "core/AutoSplitSettings.hpp"

#include <QDialog>

class QCheckBox;
class QRadioButton;
class QSpinBox;

namespace harpia {

class AutoSplitDialog : public QDialog {
public:
	AutoSplitDialog(const AutoSplitSettings &settings, bool always, QWidget *parent = nullptr);

	AutoSplitSettings settings() const;
	bool always() const;

private:
	void syncEnabled();

	QCheckBox *enabled_ = nullptr;
	QRadioButton *time_ = nullptr;
	QRadioButton *size_ = nullptr;
	QSpinBox *minutes_ = nullptr;
	QSpinBox *sizeMB_ = nullptr;
	QRadioButton *once_ = nullptr;
	QRadioButton *always_ = nullptr;
};

} // namespace harpia
