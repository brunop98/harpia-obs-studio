#pragma once

#include "ClipExporter.hpp"

#include <QDialog>
#include <QImage>
#include <QSize>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QWidget;

namespace harpia {

// Collects the export choices before processing.
//
// It used to be a Save As box that happened to have a format combo: a name, a
// format and two options, with no way to tell how big the result would be or
// even which folder it was going to land in. Everything it now shows is either
// a choice the encoder honours or a fact about what is about to be written --
// there are deliberately no controls here that the exporter would ignore.
class ExportOptionsDialog : public QDialog {
	Q_OBJECT
public:
	// What the dialog needs to know about the thing being exported in order to
	// summarise it and estimate its size. The window has all of this already,
	// and handing it over beats the dialog reaching back into the editor.
	struct Context {
		QString defaultName;
		QString defaultFolder;
		QSize sourceSize;      // the size the export is at "Original"
		double fps = 30.0;     // output frame rate (GIF chooses its own)
		double seconds = 0.0;  // output duration
		// Whether audio CAN be carried at all -- not whether the source has
		// any. Probing for an audio stream would mean opening the file again
		// here, and "keep the audio if there is any" is what the checkbox has
		// always meant.
		bool canKeepAudio = true;
		bool allowGif = true;  // multi-cut exports are video-only
		QImage previewFrame;   // first frame, for the summary panel
	};

	explicit ExportOptionsDialog(const Context &ctx, QWidget *parent = nullptr);

	QString fileName() const; // base name, no extension
	QString folder() const;   // where to write it
	// Folder + name + the format's extension: what the caller should actually
	// write to, with the three-way join in one place.
	QString outputPath() const;
	ClipExporter::Format format() const;
	int gifFps() const;
	int gifColors() const;
	bool gifDither() const;
	bool gifLoop() const;
	int videoCrf() const;
	bool keepAudio() const;
	// 0x0 = keep the source/canvas size. Always even; see encodeSize().
	QSize outputSize() const;
	// The estimate currently on screen, so a test can check the label rather
	// than re-deriving it.
	qint64 estimatedBytes() const;

private slots:
	void onFormatChanged();
	void refresh(); // summary + estimate, after any change

private:
	void browseFolder();
	void loadRemembered();
	void saveRemembered() const;
	double effectiveFps() const;

	Context ctx_;

	QLineEdit *nameEdit_ = nullptr;
	QLineEdit *folderEdit_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *resCombo_ = nullptr;
	QSpinBox *customW_ = nullptr;
	QSpinBox *customH_ = nullptr;
	QWidget *customRow_ = nullptr;

	QSlider *qualitySlider_ = nullptr;
	QLabel *qualityLabel_ = nullptr;
	QCheckBox *audioCheck_ = nullptr;
	QWidget *videoRow_ = nullptr;

	QSpinBox *gifFpsSpin_ = nullptr;
	QComboBox *gifColorsCombo_ = nullptr;
	QCheckBox *gifDitherCheck_ = nullptr;
	QCheckBox *gifLoopCheck_ = nullptr;
	QWidget *gifRow_ = nullptr;

	QLabel *summary_ = nullptr;
	QLabel *sizeValue_ = nullptr;
	QPushButton *exportBtn_ = nullptr;
};

} // namespace harpia
