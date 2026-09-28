// The recorder's logger installs a libobs log handler; the editor only reads
// its file list. A window-level test needs neither, so these stand in.
#include "core/Logger.hpp"
namespace harpia {
Logger &Logger::instance() { static Logger l; return l; }
std::string Logger::logDir() const { return std::string(); }
std::string Logger::sessionFilePath() const { return std::string(); }
std::vector<std::string> Logger::sessionFiles() const { return {}; }
void Logger::clearAll() {}
} // namespace harpia
