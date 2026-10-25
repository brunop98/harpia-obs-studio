#pragma once

// The secrets the editor keeps: one transcription API key per speech service
// (SpeechProviders.hpp), each stored on its own. OpenAI's stays under the name
// it always had, so a key saved before other services existed still works.
//
// Never in a project file, never in a log, never shown whole. On Windows it
// is sealed with DPAPI (CryptProtectData), so only this user on this machine
// can read it back -- the same protection the browsers use for saved
// passwords. macOS uses the login Keychain and migrates legacy scrambled
// settings only after verified storage. Other platforms keep a lightly
// scrambled settings value; that is obfuscation, not security.

#include "SpeechProviders.hpp"

#include <QByteArray>
#include <QSettings>
#include <QString>

namespace harpia {

class SecretStore {
public:
	// Empty removes it. True when the key can be read back afterwards (or was
	// removed); false when the platform credential store refused the request.
	// Either way the outcome goes into the error log -- never the key itself.
	static bool saveApiKey(SpeechProvider p, const QString &key);
	static QString loadApiKey(SpeechProvider p);
	static bool hasApiKey(SpeechProvider p) { return !loadApiKey(p).isEmpty(); }
	// A key that belongs to no transcription service (Google's text to
	// speech): sealed the same way, under its own settings name; `display` is
	// what the log calls it.
	static bool saveNamedKey(const QString &settingsName, const QString &display, const QString &key);
	static QString loadNamedKey(const QString &settingsName, const QString &display);
	// Where each key lives in the settings. Exposed for the test.
	static QString settingsName(SpeechProvider p);

	// "sk-…abcd": enough to recognise, not enough to use.
	static QString maskedKey(const QString &key);

private:
	static bool storeSealed(QSettings &s, const QString &kKey, const QString &kKind, const QByteArray &plain);
};

} // namespace harpia
