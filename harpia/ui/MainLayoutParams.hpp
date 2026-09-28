#pragma once

// The recorder window's layout sizes, and the one table of them that the
// Developer Panel's load, save, Copy JSON and Paste JSON all read -- so a new
// size is added in exactly two places (the struct and the table) and every one
// of those four follows. Kept apart from MainWindow.hpp, which pulls in the
// whole recorder, so the JSON can be checked on its own.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace harpia {

// Runtime-tweakable layout metrics for the main recorder window, edited live
// from the Developer Panel (auto-saved to QSettings) to find the best sizing.
// Every default here MUST match the literal used when the window is first
// built, so a fresh install re-applies identical values.
struct MainLayoutParams {
	int rootMarginH = 18;       // outer left/right padding
	int rootMarginV = 14;       // outer top/bottom padding
	int rootSpacing = 12;       // gap between the stacked sections
	int row1Spacing = 8;        // toolbar row base item spacing
	int presetComboW = 160;     // preset combo min width
	int monitorComboMinW = 140; // display combo min width
	int monitorComboMaxW = 200; // display combo max width
	int behaviorComboW = 200;   // Focus/Webcam/Idle combo fixed width
	int behaviorLabelW = 110;   // right-aligned behavior label width
	int behaviorColSpacing = 10; // gap between behavior rows
	int behaviorRowSpacing = 8;  // gap between a behavior label and its combo
	int middleSpacing = 18;      // gap between the behavior column and controls
	int controlsSpacing = 14;    // gap between record controls
	int recordBtnW = 130;
	int recordBtnH = 46;
	int pauseBtnW = 104;
	int pauseBtnH = 46;
	int separatorH = 28;         // controls-row vertical separator height
	int webcamPreviewW = 100;
	int webcamPreviewH = 56;
	int stripThumbW = 160;       // recent-recording card thumbnail size
	int stripThumbH = 90;
	int stripCardExtraW = 24;    // card width padding beyond the thumbnail
	int stripCardExtraH = 12;    // card height padding beyond thumb + captions
	int stripSpacing = 6;        // gap between recent cards
};

struct MainLayoutField {
	const char *key;                 // the QSettings / JSON name
	int MainLayoutParams::*member;
};

inline const QVector<MainLayoutField> &mainLayoutFields()
{
	static const QVector<MainLayoutField> f = {
		{"rootMarginH", &MainLayoutParams::rootMarginH},
		{"rootMarginV", &MainLayoutParams::rootMarginV},
		{"rootSpacing", &MainLayoutParams::rootSpacing},
		{"row1Spacing", &MainLayoutParams::row1Spacing},
		{"presetComboW", &MainLayoutParams::presetComboW},
		{"monitorComboMinW", &MainLayoutParams::monitorComboMinW},
		{"monitorComboMaxW", &MainLayoutParams::monitorComboMaxW},
		{"behaviorComboW", &MainLayoutParams::behaviorComboW},
		{"behaviorLabelW", &MainLayoutParams::behaviorLabelW},
		{"behaviorColSpacing", &MainLayoutParams::behaviorColSpacing},
		{"behaviorRowSpacing", &MainLayoutParams::behaviorRowSpacing},
		{"middleSpacing", &MainLayoutParams::middleSpacing},
		{"controlsSpacing", &MainLayoutParams::controlsSpacing},
		{"recordBtnW", &MainLayoutParams::recordBtnW},
		{"recordBtnH", &MainLayoutParams::recordBtnH},
		{"pauseBtnW", &MainLayoutParams::pauseBtnW},
		{"pauseBtnH", &MainLayoutParams::pauseBtnH},
		{"separatorH", &MainLayoutParams::separatorH},
		{"webcamPreviewW", &MainLayoutParams::webcamPreviewW},
		{"webcamPreviewH", &MainLayoutParams::webcamPreviewH},
		{"stripThumbW", &MainLayoutParams::stripThumbW},
		{"stripThumbH", &MainLayoutParams::stripThumbH},
		{"stripCardExtraW", &MainLayoutParams::stripCardExtraW},
		{"stripCardExtraH", &MainLayoutParams::stripCardExtraH},
		{"stripSpacing", &MainLayoutParams::stripSpacing},
	};
	return f;
}

// Every size as JSON, in the editor Developer Panel's shape:
//   {"harpiaDevLayout": 1, "panel": "recorder-window", "values": {...}}
inline QByteArray mainLayoutToJson(const MainLayoutParams &p)
{
	QJsonObject values;
	for (const MainLayoutField &f : mainLayoutFields())
		values[QString::fromLatin1(f.key)] = p.*(f.member);
	QJsonObject root;
	root[QStringLiteral("harpiaDevLayout")] = 1;
	root[QStringLiteral("panel")] = QStringLiteral("recorder-window");
	root[QStringLiteral("values")] = values;
	return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// The values in `json` (that shape, or a bare {"key": n} object) applied onto
// `p`. Unknown names are ignored. Returns how many were applied; 0 with *err
// set when it is not JSON at all.
inline int mainLayoutFromJson(const QByteArray &json, MainLayoutParams &p, QString *err = nullptr)
{
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
	if (!doc.isObject()) {
		if (err)
			*err = pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("not a JSON object");
		return 0;
	}
	QJsonObject values = doc.object();
	if (values.value(QStringLiteral("values")).isObject())
		values = values.value(QStringLiteral("values")).toObject();
	int n = 0;
	for (const MainLayoutField &f : mainLayoutFields()) {
		const QJsonValue v = values.value(QString::fromLatin1(f.key));
		if (v.isDouble()) {
			p.*(f.member) = v.toInt();
			++n;
		}
	}
	if (n == 0 && err)
		*err = QStringLiteral("no recorder-window sizes in it");
	return n;
}

} // namespace harpia
