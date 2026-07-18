#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <string>

namespace harpia {

// Expands filename templates containing {Token} placeholders into concrete
// filenames. Tokens are resolved from a registry, so adding a new placeholder
// is a one-line registration and never touches the expansion logic.
//
// Built-in tokens: {Year} {Month} {Day} {Hour} {Minute} {Second}
//
// Example:
//   FileNameTemplate t;
//   t.expand("Tutorial_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}");
//   => "Tutorial_2026-07-18_14-05-09"
class FileNameTemplate {
public:
	// A resolver receives the capture-time broken-down clock and returns the
	// replacement text for its token.
	using Resolver = std::function<std::string(const std::tm &)>;

	FileNameTemplate();

	// Register or override a token. Name is given without braces (e.g. "Year").
	void registerToken(const std::string &name, Resolver resolver);

	// Expand all {Token} occurrences using `when` (defaults to the current
	// local time). Unknown tokens are left untouched so mistakes are visible
	// rather than silently dropped. The result is sanitized of characters that
	// are illegal in filenames on the target platforms.
	std::string expand(const std::string &tmpl, std::time_t when = std::time(nullptr)) const;

private:
	std::map<std::string, Resolver> tokens_;
};

} // namespace harpia
