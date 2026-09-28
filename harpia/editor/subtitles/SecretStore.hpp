#pragma once

// The secrets the editor keeps: one transcription API key per speech service
// (SpeechProviders.hpp), each stored on its own. OpenAI's stays under the name
// it always had, so a key saved before other services existed still works.
//
// Never in a project file, never in a log, never shown whole. On Windows it
// is sealed with DPAPI (CryptProtectData), so only this user on this machine
// can read it back -- the same protection the browsers use for saved
// passwords. Elsewhere it is kept in the app's settings, lightly scrambled so
// it is not readable at a glance; that is obfuscation, not security, and the
// header says so rather than pretending.

#include "SpeechProviders.hpp"

#include <QByteArray>
#include <QSettings>
#include <QString>

namespace harpia {

class SecretStore {
public:
	// Empty removes it. True when the key can be read back afterwards (or was
	// removed); false when Windows refused to protect it, so nothing was kept.
	// Either way the outcome goes into the error log -- never the key itself.
	static bool saveApiKey(SpeechProvider p, const QString &key);
	static QString loadApiKey(SpeechProvider p);
	static bool hasApiKey(SpeechProvider p) { return !loadApiKey(p).isEmpty(); }
	// Where each key lives in the settings. Exposed for the test.
	static QString settingsName(SpeechProvider p);

	// "sk-…abcd": enough to recognise, not enough to use.
	static QString maskedKey(const QString &key);

private:
	static bool storeSealed(QSettings &s, const QString &kKey, const QString &kKind, const QByteArray &plain);
};

} // namespace harpia
