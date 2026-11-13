/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/config_store.h"

#include <stdio.h>
#include <string.h>

namespace vdm
{

const char *loadResultName(LoadResult r)
{
    switch (r) {
    case LoadResult::Loaded:
        return "loaded";
    case LoadResult::FromTemp:
        return "loaded from the temporary file";
    case LoadResult::FromBackup:
        return "loaded from the backup";
    case LoadResult::Defaults:
        return "defaults (no configuration)";
    case LoadResult::Invalid:
        return "defaults (configuration invalid)";
    }
    return "?";
}

ConfigStore::ConfigStore(FileSystem &fs, const char *path)
    : fs_(fs), path_(path), tmp_(std::string(path) + ".tmp"), bak_(std::string(path) + ".bak")
{
}

std::string ConfigStore::toText(const Config &c, bool secrets, bool pretty)
{
    static DynamicJsonDocument doc(kConfigJsonCapacity);
    doc.clear();
    configToJson(c, doc.to<JsonObject>(), secrets);
    std::string out;
    if (pretty) {
        serializeJsonPretty(doc, out);
    } else {
        serializeJson(doc, out);
    }
    return out;
}

bool ConfigStore::fromText(const std::string &text, Config &out, char *err, size_t err_len)
{
    static DynamicJsonDocument doc(kConfigJsonCapacity);
    DeserializationError e = deserializeJson(doc, text);
    if (e) {
        snprintf(err, err_len, "invalid JSON: %s", e.c_str());
        return false;
    }
    if (!configFromJson(doc.as<JsonObjectConst>(), out, err, err_len)) {
        return false;
    }
    /* Migrations of older versions go here. A document of a newer firmware
     * is accepted as far as it is understood (unknown fields are ignored). */
    out.version = kConfigVersion;
    return true;
}

bool ConfigStore::tryLoad(const char *path, Config &out)
{
    std::string text;
    if (!fs_.exists(path) || !fs_.read(path, text)) {
        return false;
    }
    char e[80];
    if (!fromText(text, out, e, sizeof(e))) {
        if (err_[0] == '\0') {
            snprintf(err_, sizeof(err_), "%s: %s", path, e);
        }
        return false;
    }
    return true;
}

LoadResult ConfigStore::load(Config &out)
{
    err_[0] = '\0';
    bool any = fs_.exists(path_.c_str()) || fs_.exists(tmp_.c_str()) || fs_.exists(bak_.c_str());
    if (tryLoad(path_.c_str(), out)) {
        return LoadResult::Loaded;
    }
    if (tryLoad(tmp_.c_str(), out)) {
        return LoadResult::FromTemp;
    }
    if (tryLoad(bak_.c_str(), out)) {
        return LoadResult::FromBackup;
    }
    out = configDefaults();
    return any ? LoadResult::Invalid : LoadResult::Defaults;
}

bool ConfigStore::save(const Config &c)
{
    std::string text = toText(c, true, false);
    if (!fs_.write(tmp_.c_str(), text)) {
        return false;
    }
    std::string check;
    if (!fs_.read(tmp_.c_str(), check) || check != text) {
        fs_.remove(tmp_.c_str());
        return false;
    }
    if (fs_.exists(path_.c_str())) {
        fs_.remove(bak_.c_str());
        if (!fs_.rename(path_.c_str(), bak_.c_str())) {
            return false;
        }
    }
    return fs_.rename(tmp_.c_str(), path_.c_str());
}

} // namespace vdm
