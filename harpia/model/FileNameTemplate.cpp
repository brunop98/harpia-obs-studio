#include "FileNameTemplate.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace harpia {

namespace {

// Zero-padded integer to a fixed width (used for date/time components).
std::string pad(int value, int width)
{
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%0*d", width, value);
	return buf;
}

// Strip characters that are invalid in filenames on Windows (the strictest of
// our targets), plus path separators.
std::string sanitize(const std::string &in)
{
	static const std::string illegal = "\\/:*?\"<>|";
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		if (illegal.find(c) != std::string::npos || (unsigned char)c < 0x20)
			out.push_back('_');
		else
			out.push_back(c);
	}
	return out;
}

} // namespace

FileNameTemplate::FileNameTemplate()
{
	registerToken("Year", [](const std::tm &t) { return pad(t.tm_year + 1900, 4); });
	registerToken("Month", [](const std::tm &t) { return pad(t.tm_mon + 1, 2); });
	registerToken("Day", [](const std::tm &t) { return pad(t.tm_mday, 2); });
	registerToken("Hour", [](const std::tm &t) { return pad(t.tm_hour, 2); });
	registerToken("Minute", [](const std::tm &t) { return pad(t.tm_min, 2); });
	registerToken("Second", [](const std::tm &t) { return pad(t.tm_sec, 2); });
	registerToken("MonthShort", [](const std::tm &t) {
		char b[16];
		std::strftime(b, sizeof(b), "%b", &t); // "Jul"
		return std::string(b);
	});
	registerToken("MonthLong", [](const std::tm &t) {
		char b[32];
		std::strftime(b, sizeof(b), "%B", &t); // "July"
		return std::string(b);
	});
}

void FileNameTemplate::registerToken(const std::string &name, Resolver resolver)
{
	tokens_[name] = std::move(resolver);
}

std::string FileNameTemplate::expand(const std::string &tmpl, std::time_t when) const
{
	return expand(tmpl, {}, when);
}

std::string FileNameTemplate::expand(const std::string &tmpl,
				     const std::map<std::string, std::string> &vars, std::time_t when) const
{
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &when);
#else
	localtime_r(&when, &local);
#endif

	std::string out;
	out.reserve(tmpl.size());

	for (size_t i = 0; i < tmpl.size();) {
		if (tmpl[i] == '{') {
			size_t end = tmpl.find('}', i);
			if (end != std::string::npos) {
				std::string token = tmpl.substr(i + 1, end - i - 1);

				// Context vars (Preset/Resolution/FPS/Codec/Counter/…) win.
				auto vit = vars.find(token);
				if (vit != vars.end()) {
					out += vit->second;
					i = end + 1;
					continue;
				}
				// Time-based tokens from the registry.
				auto it = tokens_.find(token);
				if (it != tokens_.end()) {
					out += it->second(local);
					i = end + 1;
					continue;
				}
				// {Random}: a 4-digit random group when not supplied.
				if (token == "Random") {
					out += pad(std::rand() % 10000, 4);
					i = end + 1;
					continue;
				}
			}
		}
		out.push_back(tmpl[i]);
		++i;
	}

	return sanitize(out);
}

} // namespace harpia
