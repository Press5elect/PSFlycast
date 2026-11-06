#include "cfg.h"
#include "ini.h"
#include "stdclass.h"
#include <cerrno>

namespace config
{

static std::string cfgPath;
static bool autoSaving = true;
static IniFile cfgdb;

// What was last written, so that a save of the same text is not written again (a drive's writes
// are counted: every toggle that changes nothing, and every option saved with its own value, was one).
static std::string lastWritten;
static bool haveWritten = false;

static void saveFile()
{
	std::string data;
	cfgdb.save(data);
	if (haveWritten && data == lastWritten)
		return;
	hostfs::File* cfgfile = hostfs::storage().openFile(cfgPath.c_str(), "wt");
	if (!cfgfile) {
		WARN_LOG(COMMON, "Error: Unable to open file '%s' for saving", cfgPath.c_str());
	}
	else {
		const bool written = cfgfile->write(data.c_str(), 1, data.length()) == data.length();
		delete cfgfile;
		if (written)
		{
			lastWritten = std::move(data);
			haveWritten = true;
		}
	}
}

static void autoSave() {
	if (autoSaving)
		saveFile();
}

bool open()
{
	if (get_writable_config_path("").empty())
		// Config dir not set (android onboarding)
		return false;

	const char* filename = "emu.cfg";
	std::string config_path_read = get_readonly_config_path(filename);
	cfgPath = get_writable_config_path(filename);

	hostfs::File* cfgfile = hostfs::storage().openFile(config_path_read.c_str(), "rt");
	if (cfgfile != nullptr) {
		cfgdb.load(cfgfile);
		delete cfgfile;
	}
	else
	{
		// Config file can't be opened
		int error_code = errno;
		WARN_LOG(COMMON, "Warning: Unable to open the config file '%s' for reading (%s)", config_path_read.c_str(), strerror(error_code));
		if (error_code == ENOENT || cfgPath != config_path_read)
		{
			// Config file didn't exist
			INFO_LOG(COMMON, "Creating new empty config file at '%s'", cfgPath.c_str());
			saveFile();
		}
	}

	return true;
}

void saveStr(const std::string& section, const std::string& key, const std::string& value) {
	cfgdb.set(section, key, value);
	autoSave();
}

std::string loadStr(const std::string& section, const std::string& key, const std::string& def) {
	return cfgdb.getString(section, key, def);
}

void saveInt(const std::string& section, const std::string& key, int value) {
	cfgdb.set(section, key, value);
	autoSave();
}

int loadInt(const std::string& section, const std::string& key, int def) {
	return cfgdb.getInt(section, key, def);
}

int64_t loadInt64(const std::string& section, const std::string& key, int64_t def) {
	return cfgdb.getInt64(section, key, def);
}
void saveInt64(const std::string& section, const std::string& key, int64_t value) {
	cfgdb.set(section, key, value);
	autoSave();
}

void saveBool(const std::string& section, const std::string& key, bool value) {
	cfgdb.set(section, key, value);
	autoSave();
}

bool loadBool(const std::string& section, const std::string& key, bool def) {
	return cfgdb.getBool(section, key, def);
}

void setTransient(const std::string& section, const std::string& key, const std::string& value) {
	cfgdb.set(section, key, value, true);
}

bool isTransient(const std::string& section, const std::string& key) {
	return cfgdb.isTransient(section, key);
}

bool hasSection(const std::string& section) {
	return cfgdb.hasSection(section);
}

void deleteSection(const std::string& section) {
	cfgdb.deleteSection(section);
}

void deleteEntry(const std::string& section, const std::string& key) {
	cfgdb.deleteEntry(section, key);
}

void setAutoSave(bool autoSaving)
{
	config::autoSaving = autoSaving;
	if (autoSaving)
		saveFile();
}

void saveFloat(const std::string& section, const std::string& key, float value) {
	cfgdb.set(section, key, value);
	autoSave();
}
float loadFloat(const std::string& section, const std::string& key, float def) {
	return cfgdb.getFloat(section, key, def);
}

std::vector<std::string> getEntries(const std::string& section) {
	return cfgdb.getEntryNames(section);
}

}	// namespace config
