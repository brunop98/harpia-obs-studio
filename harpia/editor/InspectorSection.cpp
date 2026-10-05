#include "InspectorSection.hpp"

#include "ui/UiIcons.hpp"

#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace harpia {

namespace {
QString settingsKey(const QString &key)
{
	return QStringLiteral("editor/sections/") + key;
}
} // namespace

InspectorSection::InspectorSection(const QString &title, const QString &key, bool expandedByDefault,
				   QWidget *parent)
	: QWidget(parent), title_(title), key_(key), expanded_(expandedByDefault)
{
	if (!key_.isEmpty()) {
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		expanded_ = st.value(settingsKey(key_), expandedByDefault).toBool();
	}

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	head_ = new QPushButton(this);
	head_->setFlat(true);
	head_->setCursor(Qt::PointingHandCursor);
	head_->setFocusPolicy(Qt::NoFocus);
	head_->setStyleSheet(QStringLiteral(
		"QPushButton{text-align:left;color:#e8eaed;font-weight:bold;border:none;padding:4px 0;}"
		"QPushButton:hover{color:#ffffff;}"));
	outer->addWidget(head_);

	body_ = new QWidget(this);
	bodyLayout_ = new QVBoxLayout(body_);
	bodyLayout_->setContentsMargins(2, 2, 2, 6);
	bodyLayout_->setSpacing(4);
	outer->addWidget(body_);

	connect(head_, &QPushButton::clicked, this, [this]() { setExpanded(!expanded_); });
	applyState();
}

void InspectorSection::setTitle(const QString &title)
{
	title_ = title;
	head_->setText(title_);
}

void InspectorSection::setExpanded(bool on)
{
	if (on == expanded_)
		return;
	expanded_ = on;
	if (!key_.isEmpty()) {
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		st.setValue(settingsKey(key_), on);
	}
	applyState();
}

void InspectorSection::applyState()
{
	head_->setText(title_);
	head_->setIcon(uiIcon(expanded_ ? Glyph::ChevronDown : Glyph::ChevronRight, 12));
	head_->setToolTip(expanded_ ? QStringLiteral("Click to fold this section away")
				    : QStringLiteral("Click to open this section"));
	body_->setVisible(expanded_);
}

} // namespace harpia
