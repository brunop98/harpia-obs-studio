// Reading a still, by whichever decoder can, and saying why when neither could.
//
// This lives on its own rather than inside the editor window because there are
// two callers on two threads: the window, adding an image as a source, and the
// export worker, re-reading that source's path while it encodes. The exporter
// used to do a bare QImage(path), so a WebP that the fallback below had decoded
// perfectly in the preview came out of the encoder as an empty frame -- a bug
// with no symptom until after the export finished.
//
// Both functions are pure and touch no window state.
#pragma once

#include <QImage>
#include <QString>

namespace harpia {

// Null on failure, with *why (when given) naming which stage failed and what
// this build can actually read.
//
// Two decoders, because neither alone covers what the file dialog offers. Qt
// handles PNG, JPEG, BMP and GIF natively and quickly. WebP needs Qt's
// qtimageformats plugin, which is NOT in every build -- it is absent from the
// trimmed obs-deps Qt used for releases -- so libav, already linked for video,
// is the fallback for anything Qt declines.
QImage readStillImage(const QString &path, QString *why = nullptr);

// The file-dialog filter, built from what can ACTUALLY be opened rather than
// written out by hand. The hand-written one listed *.webp on every build, so on
// a Qt without the plugin the dialog invited a file the editor then refused --
// and offering something you cannot accept is worse than not offering it.
QString imageOpenFilter();

} // namespace harpia
