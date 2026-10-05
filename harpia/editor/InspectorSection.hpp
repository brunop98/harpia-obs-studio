#pragma once

// One collapsible section of the Inspector: a "▾ Title" header you click to
// fold the body away, and the body underneath.
//
// It remembers. Folding "Tags" because you never use it should not have to be
// done again on every clip and every launch, so each section's open/closed
// state is kept under its key (QSettings, editor/sections/<key>). The state is
// the section's own flag rather than the body's visibility -- a body inside a
// hidden panel is "not visible" without being folded, and reading it back from
// there would flip a section the user never touched.

#include <QWidget>

class QPushButton;
class QVBoxLayout;

namespace harpia {

class InspectorSection : public QWidget {
public:
	// `key` names the remembered state; empty = not remembered.
	InspectorSection(const QString &title, const QString &key, bool expandedByDefault, QWidget *parent);

	QVBoxLayout *bodyLayout() const { return bodyLayout_; }
	QWidget *body() const { return body_; }
	QPushButton *header() const { return head_; }

	void setTitle(const QString &title);
	QString title() const { return title_; }

	bool expanded() const { return expanded_; }
	void setExpanded(bool on);

private:
	void applyState();

	QString title_;
	QString key_;
	bool expanded_ = true;
	QPushButton *head_ = nullptr;
	QWidget *body_ = nullptr;
	QVBoxLayout *bodyLayout_ = nullptr;
};

} // namespace harpia
