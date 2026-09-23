#pragma once

// The one secret the editor keeps: the transcription API key.
//
// Never in a project file, never in a log, never shown whole. On Windows it
// is sealed with DPAPI (CryptProtectData), so only this user on this machine
// can read it back -- the same protection the browsers use for saved
// passwords. Elsewhere it is kept in the app's settings, lightly scrambled so
// it is not readable at a glance; that is obfuscation, not security, and the
// header says so rather than pretending.

#include <QString>

namespace harpia {

class SecretStore {
public:
	static void saveApiKey(const QString &key); // empty removes it
	static QString loadApiKey();
	static bool hasApiKey() { return !loadApiKey().isEmpty(); }
	// "sk-…abcd": enough to recognise, not enough to use.
	static QString maskedKey(const QString &key);
};

} // namespace harpia
