/*
 * ConfigStore - loads and saves the configuration as JSON.
 *
 * Saving is atomic: the new document is written to "<path>.tmp" and read
 * back, the current file becomes "<path>.bak", then the temporary file is
 * renamed. Loading tries the file, the temporary file (power loss between
 * the renames) and the backup, in this order; if none is valid the
 * defaults are used.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <string>

#include "core/config.h"

namespace vdm
{

class FileSystem
{
  public:
    virtual ~FileSystem() = default;
    virtual bool read(const char *path, std::string &out) = 0;
    virtual bool write(const char *path, const std::string &data) = 0;
    virtual bool rename(const char *from, const char *to) = 0;
    virtual bool remove(const char *path) = 0;
    virtual bool exists(const char *path) = 0;
};

enum class LoadResult : uint8_t {
    Loaded,
    FromTemp,
    FromBackup,
    Defaults, /* no file */
    Invalid,  /* files present but none valid: defaults are used */
};

const char *loadResultName(LoadResult r);

class ConfigStore
{
  public:
    explicit ConfigStore(FileSystem &fs, const char *path = "/config.json");

    LoadResult load(Config &out);
    bool save(const Config &c);

    /* serializes the configuration (pretty printed for export) */
    static std::string toText(const Config &c, bool secrets, bool pretty);
    /* parses a document, e.g. an import; `out` is only changed on success */
    static bool fromText(const std::string &text, Config &out, char *err, size_t err_len);

    /* reason why the last load() did not use the main file, empty if it did */
    const char *lastError() const
    {
        return err_;
    }

  private:
    bool tryLoad(const char *path, Config &out);

    FileSystem &fs_;
    std::string path_, tmp_, bak_;
    char err_[96] = "";
};

} // namespace vdm
