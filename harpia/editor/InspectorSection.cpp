#include "InspectorSection.hpp"

#include "ui/UiIcons.hpp"

#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace harpia {

namespace {
QString settingsKey(const QString &key)
{
	return QStringLiteral("editor/sections/") + key;
}

// The card: a step up from the panel (#1e1f22), a hairline edge, and a header
// a shade lighter again (lighter still under the pointer).
const QColor kCardFill(0x24, 0x26, 0x2a);
const QColor kCardEdge(0x34, 0x37, 0x3d);
constexpr double kRadius = 6.0;
constexpr int kGap = 6; // below each card, so neighbours read as separate groups

QString headerStyle(bool expanded)
{
	// The header's own corners follow the card's: all four when folded (it is
	// the whole card), the top two when open.
	const QString radius = expanded ? QStringLiteral("border-top-left-radius:%1px;border-top-right-radius:%1px;")
					: QStringLiteral("border-radius:%1px;");
	return QStringLiteral("QPushButton{text-align:left;color:#e8eaed;font-weight:bold;border:none;"
			      "background:#282a2f;padding:7px %2px 7px %2px;%1}"
			      "QPushButton:hover{color:#ffffff;background:#2e3137;}")
		.arg(radius.arg(int(kRadius) - 1))
		.arg(InspectorSection::kHeaderInset);
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

	// 1 px all round for the card's edge (painted in paintEvent), the gap below.
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(1, 1, 1, 1 + kGap);
	outer->setSpacing(0);

	head_ = new QPushButton(this);
	head_->setFlat(true);
	head_->setCursor(Qt::PointingHandCursor);
	head_->setFocusPolicy(Qt::NoFocus);
	outer->addWidget(head_);

	// 1 px for the divider under the header, then the content, inset.
	body_ = new QWidget(this);
	bodyLayout_ = new QVBoxLayout(body_);
	bodyLayout_->setContentsMargins(kBodyInset, 1 + 8, kBodyInset, kBodyInset);
	bodyLayout_->setSpacing(6);
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

void InspectorSection::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5 - kGap);
	p.setPen(QPen(kCardEdge, 1.0));
	p.setBrush(kCardFill);
	p.drawRoundedRect(card, kRadius, kRadius);
	if (expanded_) {
		// The divider between the header and the body.
		const double y = head_->geometry().bottom() + 1.0;
		p.setRenderHint(QPainter::Antialiasing, false);
		p.setPen(kCardEdge);
		p.drawLine(QPointF(card.left() + 1.0, y), QPointF(card.right() - 1.0, y));
	}
}

void InspectorSection::applyState()
{
	head_->setStyleSheet(headerStyle(expanded_));
	head_->setText(title_);
	// Its own colour, a step softer than the title, rather than the palette's
	// button text: the header's look is set here, not by the theme.
	head_->setIcon(uiIcon(expanded_ ? Glyph::ChevronDown : Glyph::ChevronRight, 12, QColor(0xb4, 0xb9, 0xc1)));
	head_->setToolTip(expanded_ ? QStringLiteral("Click to fold this section away")
				    : QStringLiteral("Click to open this section"));
	body_->setVisible(expanded_);
	update();
}

} // namespace harpia
