#pragma once

#include <QSurfaceFormat>

class QOffscreenSurface;
class QString;

namespace harpia {

// Apple exposes 3.2/4.1 core profiles; GLSL 330 effects run in the 4.1 profile.
QSurfaceFormat editorGlFormat();

// macOS offscreen surfaces may own an AppKit window. Prepare their storage on
// the GUI thread before starting exports, then lease them without asking the
// GUI thread to run code (it may be joining an export worker).
// Other platforms retain the existing direct surface creation path.
bool prepareEditorGlSurfaces(int minimumCount = 24, QString *error = nullptr);
// Call on the GUI thread after workers have joined and GUI renderers released
// their resources. A still-leased surface is retained rather than invalidated.
bool releaseEditorGlSurfaces();

QOffscreenSurface *createEditorGlSurface();
void destroyEditorGlSurface(QOffscreenSurface *surface);

} // namespace harpia
