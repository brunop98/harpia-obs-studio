#pragma once

#include "ClipExporter.hpp"
#include "NumberedCover.hpp"

#include <QDialog>
#include <functional>
#include <QImage>
#include <QSize>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QGroupBox;
class QListWidget;
class QTabWidget;
class QTableWidget;
class QVBoxLayout;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
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
		double fps = 30.0;     // frame rate used by Original (GIF chooses its own)
		double seconds = 0.0;  // output duration
		// Whether audio CAN be carried at all -- not whether the source has
		// any. Probing for an audio stream would mean opening the file again
		// here, and "keep the audio if there is any" is what the checkbox has
		// always meant.
		bool canKeepAudio = true;
		bool allowGif = true;  // multi-cut exports are video-only
		QImage previewFrame;   // first frame, for the summary panel

		// An optional sub-range the caller can write INSTEAD of the whole
		// thing -- the selected clip, or the span the selection covers. The
		// scope row only appears when there is one, so a Trim or Multi-Cut
		// export looks exactly as it did.
		//
		// The caller stays in charge of what the range means; the dialog only
		// reports which of the two was chosen, and sizes/summarises whichever
		// it is.
		double rangeSeconds = 0.0; // 0 = no range on offer
		QString rangeLabel;        // e.g. "Selected clip  (0:04 – 0:11)"
		// Whether to START on the range. Right-clicking a clip and choosing
		// Export means that clip; pressing the Export button means the project,
		// even when something happens to be selected.
		bool rangeDefault = false;
		// What to call the file when the range is chosen, so an excerpt does
		// not silently overwrite the full export sitting next to it.
		QString rangeName;

		// Multi-Cut: the cuts, for the Batch tab, which writes each one to
		// its own file. Empty = no Batch tab. Only what the list shows; the
		// caller builds the real cuts (with their sources) itself.
		struct BatchCut {
			qint64 startMs = 0;
			qint64 endMs = 0;
			double speed = 1.0;
		};
		QVector<BatchCut> batchCuts;

		// Full editing with text variations: one label per version ("I like
		// cats · Blue sky"), for the Variations tab. Every version is the
		// whole project, so each lasts `seconds`. Never set together with
		// batchCuts -- the two tabs belong to different modes.
		QStringList variantLabels;
		// Prefer these for combinatorial projects: sizes + a label callback.
		// Only displayed/exported choices are materialized; variantLabels may
		// remain empty. Prelisted labels above still work for simple callers.
		QVector<int> variantOptionCounts;
		std::function<QString(const QVector<int> &)> variantLabel;
		bool allowSpark = false;
		int sparkBlocks = 0;
		int sparkFavorites = 0;
		qint64 sparkUniqueOrders = 0;  // different videos, up to the end marker
		bool sparkUniqueOrdersAtLeast = false; // too many to count: a lower bound
		double sparkSeconds = 0.0; // one sequenced full-timeline export
		QString sparkError;
		int sparkMusts = 0;   // favorites every video must include (in sparkFavorites)
		int sparkOpeners = 0; // opening clips: every video starts with one of them
		TlSparkSettings sparkSettings; // the project's; the Spark tab edits a copy
		// Counts the different videos again for edited settings. Unset: the
		// count shown stays as it was.
		struct SparkCount {
			qint64 uniqueOrders = 0;
			bool atLeast = false;
		};
		std::function<SparkCount(const TlSparkSettings &)> sparkRecount;
		struct SeparateClip {
			QString label;
			qint64 startMs = 0;
			qint64 durationMs = 0;
		};
		// Dedicated export window: shared quality controls and editable filenames.
		QVector<SeparateClip> separateClips;
	};

	explicit ExportOptionsDialog(const Context &ctx, QWidget *parent = nullptr);

	void accept() override;
	QStringList separateNames() const;
	bool separateOpenFolder() const;
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
	ClipExporter::Options::Effort effort() const;
	bool chroma444() const;
	// 0x0 = keep the source/canvas size. Always even; see encodeSize().
	QSize outputSize() const;
	// Constant video output frame rate; 0 = the original (Context::fps). Higher
	// rates can retain more source frames in sped-up clips. Existing frames are
	// held when needed; no interpolation is generated. GIF uses its own gifFps.
	double frameRate() const;
	// True when the user chose the sub-range rather than the whole thing.
	// Always false when the Context offered no range.
	bool exportRange() const;
	// The duration the summary and the estimate are actually describing --
	// ctx_.seconds or ctx_.rangeSeconds, depending on the scope.
	double exportSeconds() const;
	// The estimate currently on screen, so a test can check the label rather
	// than re-deriving it.
	qint64 estimatedBytes() const;

	// The Batch tab (Multi-Cut only): true when it was the one showing on
	// Export. The selection is indices into Context::batchCuts, in order.
	bool batchMode() const;
	bool sparkMode() const;
	int sparkCount() const;
	QString sparkPrefix() const;
	bool sparkOpenFolder() const;
	cover::Numbering numberedCover() const;
	void setSparkTabForTest(bool on);
	// The settings on the Spark tab (the Context's when there is no tab).
	TlSparkSettings sparkSettings() const;
	// After an edit the count waits a moment for the next one; this counts now.
	void recountSparkNow();
	QVector<int> batchSelection() const;
	struct Variation {
		QVector<int> choices;
		qint64 number = 1; // original version number, or sample-local when the product overflows
	};
	QVector<Variation> variationSelection() const;
	QString batchPrefix() const;
	bool batchOpenFolder() const;
	// Output seconds of the selected cuts (each at its own speed).
	double batchSeconds() const;
	// The Batch tab's checklist, for tests.
	QListWidget *batchListForTest() const { return batchList_; }
	void setBatchTabForTest(bool on);

private slots:
	void onFormatChanged();
	void onScopeChanged();
	void refresh(); // summary + estimate, after any change

private:
	struct CoverControls {
		QGroupBox *box = nullptr;
		QWidget *details = nullptr;
		QCheckBox *number = nullptr, *firstFrame = nullptr;
		QLineEdit *prefix = nullptr, *suffix = nullptr;
		QSpinBox *start = nullptr, *digits = nullptr;
		QLabel *preview = nullptr, *hint = nullptr;
		QString key;
	};
	CoverControls variationCover_, sparkCover_;
	void buildCoverOptions(QVBoxLayout *layout, CoverControls &controls, const QString &key);
	cover::Numbering readCoverOptions(const CoverControls &controls) const;
	void refreshCoverOptions(CoverControls &controls);
	QString coverError() const;
	void browseFolder();
	void loadRemembered();
	void saveRemembered() const;
	double effectiveFps() const;

	Context ctx_;
	void buildSeparateNames(QVBoxLayout *layout);
	void updateSeparateNames(bool replaceAll = false);
	QString separateNameError() const;
	QTableWidget *separateTable_ = nullptr;
	QLabel *separateError_ = nullptr;
	QCheckBox *separateOpen_ = nullptr;

	QComboBox *scopeCombo_ = nullptr; // null when no range was offered
	QLineEdit *nameEdit_ = nullptr;
	QLineEdit *folderEdit_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *resCombo_ = nullptr;
	QSpinBox *customW_ = nullptr;
	QSpinBox *customH_ = nullptr;
	QWidget *customRow_ = nullptr;

	QComboBox *fpsCombo_ = nullptr;

	QSlider *qualitySlider_ = nullptr;
	QLabel *qualityLabel_ = nullptr;
	QComboBox *effortCombo_ = nullptr;
	QCheckBox *chroma444Check_ = nullptr;
	QCheckBox *audioCheck_ = nullptr;
	QWidget *videoRow_ = nullptr;

	QSpinBox *gifFpsSpin_ = nullptr;
	QComboBox *gifColorsCombo_ = nullptr;
	QCheckBox *gifDitherCheck_ = nullptr;
	QCheckBox *gifLoopCheck_ = nullptr;
	QWidget *gifRow_ = nullptr;

	QTabWidget *tabs_ = nullptr;      // null without a Batch tab
	QListWidget *batchList_ = nullptr;
	QLineEdit *batchPrefix_ = nullptr;
	QCheckBox *batchOpen_ = nullptr;
	QLabel *batchCount_ = nullptr;
	bool hasVariations() const;
	QVector<int> variationSizes() const;
	QComboBox *variationMode_ = nullptr;
	QSpinBox *variationCount_ = nullptr;
	QPushButton *variationAgain_ = nullptr;
	QVector<Variation> displayedVariations_;
	void rebuildVariationList();
	void buildBatchTab();
	void buildSparkTab();
	int batchTabIndex_ = -1;
	int sparkTabIndex_ = -1;
	QSpinBox *sparkRequested_ = nullptr;
	QLineEdit *sparkPrefix_ = nullptr;
	QCheckBox *sparkOpen_ = nullptr;
	QLabel *sparkCount_ = nullptr;
	QLabel *sparkStats_ = nullptr;
	QCheckBox *sparkFavAll_ = nullptr;
	QSpinBox *sparkFavMin_ = nullptr;
	QSpinBox *sparkFavMax_ = nullptr;
	QComboBox *sparkPlacement_ = nullptr;
	QTimer *sparkRecountTimer_ = nullptr;
	void sparkSettingsEdited();
	void updateSparkStats();

	QLabel *summary_ = nullptr;
	QLabel *sizeValue_ = nullptr;
	QPushButton *exportBtn_ = nullptr;
};

} // namespace harpia
