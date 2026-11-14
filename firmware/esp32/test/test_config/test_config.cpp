/*
 * Host tests: configuration (JSON, validation, sections) and ConfigStore.
 */
#include <string.h>
#include <unity.h>

#include <map>
#include <string>

#include "core/config.h"
#include "core/config_store.h"

namespace
{

struct FakeFs : vdm::FileSystem {
    std::map<std::string, std::string> files;
    bool fail_write = false;
    bool read(const char *p, std::string &out) override
    {
        auto it = files.find(p);
        if (it == files.end())
            return false;
        out = it->second;
        return true;
    }
    bool write(const char *p, const std::string &d) override
    {
        if (fail_write)
            return false;
        files[p] = d;
        return true;
    }
    bool rename(const char *a, const char *b) override
    {
        auto it = files.find(a);
        if (it == files.end())
            return false;
        files[b] = it->second;
        files.erase(it);
        return true;
    }
    bool remove(const char *p) override
    {
        return files.erase(p) > 0;
    }
    bool exists(const char *p) override
    {
        return files.count(p) > 0;
    }
};

vdm::Config cfg, cfg2;
char err[128];

std::string text(const vdm::Config &c, bool secrets)
{
    std::string t;
    TEST_ASSERT_TRUE(vdm::ConfigStore::toText(c, secrets, false, t));
    return t;
}

bool parse(const char *json, vdm::Config &out)
{
    return vdm::ConfigStore::fromText(json, out, err, sizeof(err));
}

const char *kRoom = R"({
  "device": {"name": "Heating", "web_user": "admin", "web_pwd": "secret"},
  "flow": {"fallback_pct": 60, "min_total_pct": 150},
  "sources": [
    {"id": "s1", "name": "Bathroom", "type": "onewire", "rom": "28-FF-01-02-03-04-05-06"},
    {"id": "s2", "name": "Living", "type": "mqtt", "topic": "ha/living/temp", "field": "value"}
  ],
  "zones": [
    {"id": "z1", "name": "Bathroom", "setpoint": 22.5, "control_sources": ["s1"],
     "info_sources": ["s2"], "profile": "floor"}
  ],
  "valves": [{"name": "Bath 1", "zone": "z1", "min_pct": 10}, {"name": "Bath 2", "zone": "z1"}],
  "next_source_id": 3, "next_zone_id": 2
})";

} // namespace

void setUp() {}
void tearDown() {}

void test_defaults()
{
    vdm::Config c;
    TEST_ASSERT_EQUAL_STRING("VdMot", c.device.name);
    TEST_ASSERT_EQUAL_STRING("Channel 1", c.valves[0].name);
    TEST_ASSERT_EQUAL_STRING("Channel 12", c.valves[11].name);
    TEST_ASSERT_EQUAL(100, c.flow.fallback_pct);
    TEST_ASSERT_EQUAL(0, c.flow.min_total_pct);
    TEST_ASSERT_EQUAL(0, c.valves[3].min_pct);
    TEST_ASSERT_TRUE(vdm::configValidate(c, err, sizeof(err)));
}

void test_empty_document_gives_defaults()
{
    TEST_ASSERT_TRUE_MESSAGE(parse("{}", cfg), err);
    TEST_ASSERT_EQUAL_STRING("VdMot", cfg.device.name);
    TEST_ASSERT_EQUAL(0, cfg.zone_count);
}

void test_round_trip()
{
    TEST_ASSERT_TRUE_MESSAGE(parse(kRoom, cfg), err);
    TEST_ASSERT_EQUAL(2, cfg.source_count);
    TEST_ASSERT_EQUAL(vdm::SourceType::Mqtt, cfg.sources[1].type);
    TEST_ASSERT_EQUAL(3600u, cfg.sources[1].max_age_s); /* MQTT default */
    TEST_ASSERT_EQUAL(300u, cfg.sources[0].max_age_s);
    TEST_ASSERT_EQUAL_FLOAT(22.5f, cfg.zones[0].setpoint);
    TEST_ASSERT_EQUAL_STRING("s2", cfg.zones[0].info_sources[0]);
    TEST_ASSERT_EQUAL(10, cfg.valves[0].min_pct);
    TEST_ASSERT_EQUAL_STRING("Channel 3", cfg.valves[2].name);

    std::string t = text(cfg, true);
    TEST_ASSERT_TRUE_MESSAGE(parse(t.c_str(), cfg2), err);
    TEST_ASSERT_EQUAL_STRING(t.c_str(), text(cfg2, true).c_str());
    TEST_ASSERT_EQUAL_STRING("secret", cfg2.device.web_pwd);
}

void test_secrets_are_hidden()
{
    TEST_ASSERT_TRUE(parse(kRoom, cfg));
    std::string t = text(cfg, false);
    TEST_ASSERT_NULL(strstr(t.c_str(), "secret"));
    TEST_ASSERT_NOT_NULL(strstr(t.c_str(), "\"web_pwd_set\":true"));
    TEST_ASSERT_NOT_NULL(strstr(t.c_str(), "\"wifi_pwd_set\":false"));
}

void test_validation()
{
    struct {
        const char *json;
        const char *expect;
    } cases[] = {
        {R"({"zones":[{"id":"z1","name":"A"}],"next_zone_id":1})", "next_zone_id"},
        {R"({"zones":[{"id":"z1","name":"A"},{"id":"z1","name":"B"}],"next_zone_id":5})",
         "used twice"},
        {R"({"zones":[{"id":"z1","name":"A","control_sources":["s9"]}],"next_zone_id":2})",
         "unknown control source"},
        {R"({"zones":[{"id":"z1","name":"A","control_source":"s9"}],"next_zone_id":2})",
         "unknown control source"},
        {R"({"zones":[{"id":"z1","name":"A","control_sources":"s1"}],"next_zone_id":2})", "list"},
        {R"({"zones":[{"id":"z1","name":"A","setpoint":40}],"next_zone_id":2})", "setpoint"},
        {R"({"zones":[{"id":"x","name":"A"}],"next_zone_id":2})", "invalid"},
        {R"({"sources":[{"id":"s1","name":"A","rom":"28-FF"}],"next_source_id":2})", "ROM"},
        {R"({"sources":[{"id":"s1","name":"A","type":"knx"}],"next_source_id":2})", "type"},
        {R"({"valves":[{"zone":"z4"}]})", "unknown zone"},
        {R"({"valves":[{"min_pct":50,"max_pct":40}]})", "minimum"},
        {R"({"valves":[{"min_pct":"10"}]})", "number"},
        {R"({"flow":{"min_total_pct":5000}})", "min_total_pct"},
        {R"({"device":{"name":""}})", "name"},
        {R"({"network":{"dhcp":false}})", "static"},
        {R"({"network":{"ip":"192.168.1.300"}})", "IPv4"},
        {R"({"mqtt":{"enabled":true}})", "mqtt.host"},
        {R"({"zones":{}})", "list"},
        {R"([1,2])", "object"},
        {R"({"device":)", "JSON"},
    };
    for (auto &c : cases) {
        bool ok = parse(c.json, cfg);
        TEST_ASSERT_FALSE_MESSAGE(ok, c.json);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, c.expect), err);
    }
    /* an MQTT source without a topic uses the default topic */
    TEST_ASSERT_TRUE_MESSAGE(
        parse(R"({"sources":[{"id":"s1","name":"A","type":"mqtt"}],"next_source_id":2})", cfg),
        err);
}

void test_apply_section_merges_objects()
{
    TEST_ASSERT_TRUE(parse(kRoom, cfg));
    StaticJsonDocument<256> in;
    deserializeJson(in, R"({"name":"Upstairs","web_user":"root"})");
    TEST_ASSERT_TRUE_MESSAGE(
        vdm::configApplySection(cfg, "device", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)),
        err);
    TEST_ASSERT_EQUAL_STRING("Upstairs", cfg2.device.name);
    TEST_ASSERT_EQUAL_STRING("root", cfg2.device.web_user);
    TEST_ASSERT_EQUAL_STRING("secret", cfg2.device.web_pwd); /* not sent: kept */
    TEST_ASSERT_EQUAL(1, cfg2.zone_count);                   /* other sections kept */
}

void test_apply_section_replaces_arrays()
{
    TEST_ASSERT_TRUE(parse(kRoom, cfg));
    StaticJsonDocument<512> in;
    /* removing the zone clears the references of the valves */
    deserializeJson(in, "[]");
    TEST_ASSERT_TRUE_MESSAGE(
        vdm::configApplySection(cfg, "zones", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)),
        err);
    TEST_ASSERT_EQUAL(0, cfg2.zone_count);
    TEST_ASSERT_EQUAL_STRING("", cfg2.valves[0].zone);

    deserializeJson(in, R"([{"name":"Bath 1"},{"name":"Bath 2"}])");
    TEST_ASSERT_TRUE_MESSAGE(
        vdm::configApplySection(cfg, "valves", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)),
        err);
    TEST_ASSERT_EQUAL_STRING("", cfg2.valves[0].zone);
    TEST_ASSERT_EQUAL(0, cfg2.valves[0].min_pct);
    TEST_ASSERT_EQUAL_STRING("Channel 5", cfg2.valves[4].name);

    deserializeJson(in, R"({"a":1})");
    TEST_ASSERT_FALSE(
        vdm::configApplySection(cfg, "zones", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)));
    TEST_ASSERT_FALSE(
        vdm::configApplySection(cfg, "bogus", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)));
}

void test_apply_section_assigns_ids_and_drops_references()
{
    TEST_ASSERT_TRUE(parse(kRoom, cfg));
    StaticJsonDocument<512> in;
    /* s1 removed, a new source added */
    deserializeJson(in, R"([{"id":"s2","name":"Living","type":"mqtt","topic":"x"},
                            {"name":"New","type":"mqtt","topic":"y"}])");
    TEST_ASSERT_TRUE_MESSAGE(
        vdm::configApplySection(cfg, "sources", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)),
        err);
    TEST_ASSERT_EQUAL(2, cfg2.source_count);
    TEST_ASSERT_EQUAL_STRING("s3", cfg2.sources[1].id);
    TEST_ASSERT_EQUAL(4, cfg2.next_source_id);
    TEST_ASSERT_EQUAL_STRING("", cfg2.zones[0].control_sources[0]); /* was s1 */
    TEST_ASSERT_EQUAL_STRING("s2", cfg2.zones[0].info_sources[0]);

    deserializeJson(in, R"([{"id":"z1","name":"Bathroom"},{"name":"Hall"},{"name":"Kitchen"}])");
    TEST_ASSERT_TRUE_MESSAGE(
        vdm::configApplySection(cfg2, "zones", in.as<JsonVariantConst>(), cfg, err, sizeof(err)),
        err);
    TEST_ASSERT_EQUAL_STRING("z2", cfg.zones[1].id);
    TEST_ASSERT_EQUAL_STRING("z3", cfg.zones[2].id);
    TEST_ASSERT_EQUAL(4, cfg.next_zone_id);
    TEST_ASSERT_EQUAL_STRING("z1", cfg.valves[0].zone);

    /* removing info sources */
    deserializeJson(in, "[]");
    TEST_ASSERT_TRUE(
        vdm::configApplySection(cfg, "sources", in.as<JsonVariantConst>(), cfg2, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", cfg2.zones[0].info_sources[0]);
}

void test_store_atomic_save_and_load()
{
    FakeFs fs;
    vdm::ConfigStore store(fs);
    TEST_ASSERT_EQUAL(vdm::LoadResult::Defaults, store.load(cfg));

    TEST_ASSERT_TRUE(parse(kRoom, cfg));
    TEST_ASSERT_TRUE(store.save(cfg));
    TEST_ASSERT_TRUE(fs.exists("/config.json"));
    TEST_ASSERT_FALSE(fs.exists("/config.json.tmp"));
    TEST_ASSERT_FALSE(fs.exists("/config.json.bak"));

    strcpy(cfg.device.name, "Second");
    TEST_ASSERT_TRUE(store.save(cfg));
    TEST_ASSERT_TRUE(fs.exists("/config.json.bak"));

    TEST_ASSERT_EQUAL(vdm::LoadResult::Loaded, store.load(cfg2));
    TEST_ASSERT_EQUAL_STRING("Second", cfg2.device.name);

    /* corrupted main file: backup */
    fs.files["/config.json"] = "{\"device\":";
    TEST_ASSERT_EQUAL(vdm::LoadResult::FromBackup, store.load(cfg2));
    TEST_ASSERT_EQUAL_STRING("Heating", cfg2.device.name);
    TEST_ASSERT_NOT_NULL(strstr(store.lastError(), "/config.json"));

    /* power loss between the renames: main file missing, tmp complete */
    fs.files.erase("/config.json");
    fs.files["/config.json.tmp"] = text(cfg, true);
    TEST_ASSERT_EQUAL(vdm::LoadResult::FromTemp, store.load(cfg2));
    TEST_ASSERT_EQUAL_STRING("Second", cfg2.device.name);

    /* nothing valid */
    fs.files.clear();
    fs.files["/config.json"] = "garbage";
    TEST_ASSERT_EQUAL(vdm::LoadResult::Invalid, store.load(cfg2));
    TEST_ASSERT_EQUAL_STRING("VdMot", cfg2.device.name);

    /* failed write keeps the old file */
    fs.files.clear();
    TEST_ASSERT_TRUE(store.save(cfg));
    fs.fail_write = true;
    strcpy(cfg.device.name, "Third");
    TEST_ASSERT_FALSE(store.save(cfg));
    TEST_ASSERT_EQUAL(vdm::LoadResult::Loaded, store.load(cfg2));
    TEST_ASSERT_EQUAL_STRING("Second", cfg2.device.name);
}

void test_helpers()
{
    uint32_t ip;
    TEST_ASSERT_TRUE(vdm::parseIp("192.168.1.20", ip));
    TEST_ASSERT_EQUAL_HEX32(0x1401A8C0u, ip); /* first octet in the lowest byte */
    char b[24];
    vdm::formatIp(ip, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", b);
    TEST_ASSERT_FALSE(vdm::parseIp("192.168.1", ip));
    TEST_ASSERT_FALSE(vdm::parseIp("192.168.1.2.3", ip));
    TEST_ASSERT_FALSE(vdm::parseIp("1.2.3.256", ip));
    TEST_ASSERT_FALSE(vdm::parseIp("a.b.c.d", ip));

    uint8_t rom[8];
    TEST_ASSERT_TRUE(vdm::parseRom("28-ff-01-02-03-04-05-a6", rom));
    TEST_ASSERT_EQUAL_HEX8(0xA6, rom[7]);
    TEST_ASSERT_TRUE(vdm::parseRom("28FF010203040506", rom));
    vdm::formatRom(rom, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("28-FF-01-02-03-04-05-06", b);
    TEST_ASSERT_FALSE(vdm::parseRom("28-FF-01-02-03-04-05", rom));
    TEST_ASSERT_FALSE(vdm::parseRom("28-FF-01-02-03-04-05-06-07", rom));

    char h[33];
    vdm::hostnameFrom("VdMot Keller_1", h, sizeof(h));
    TEST_ASSERT_EQUAL_STRING("vdmot-keller-1", h);
    vdm::hostnameFrom("  !!", h, sizeof(h));
    TEST_ASSERT_EQUAL_STRING("vdmot", h);
}

void test_legacy_control_source_key()
{
    TEST_ASSERT_TRUE_MESSAGE(parse(R"({"sources":[{"id":"s1","name":"A","type":"mqtt"}],
                  "zones":[{"id":"z1","name":"Z","control_source":"s1"}],
                  "next_source_id":2,"next_zone_id":2})",
                                   cfg),
                             err);
    TEST_ASSERT_EQUAL_STRING("s1", cfg.zones[0].control_sources[0]);
    TEST_ASSERT_TRUE(cfg.zones[0].enabled);
    TEST_ASSERT_NOT_NULL(strstr(text(cfg, true).c_str(), "\"control_sources\":[\"s1\"]"));
    TEST_ASSERT_TRUE(
        parse(R"({"zones":[{"id":"z1","name":"Z","heating":false}],"next_zone_id":2})", cfg));
    TEST_ASSERT_FALSE(cfg.zones[0].enabled);
    TEST_ASSERT_TRUE(parse(R"({"hvac":{"mode":"cool","supply_margin_k":3}})", cfg));
    TEST_ASSERT_EQUAL(vdm::HvacMode::Cool, cfg.hvac.mode);
    TEST_ASSERT_FALSE(parse(R"({"hvac":{"supply_source":"s7"}})", cfg));
    TEST_ASSERT_FALSE(
        parse(R"({"zones":[{"id":"z1","name":"Z","supply_source":"s7"}],"next_zone_id":2})", cfg));
}

void test_reset()
{
    TEST_ASSERT_TRUE(parse(kRoom, cfg));
    cfg.reset();
    TEST_ASSERT_EQUAL_STRING(text(vdm::Config(), true).c_str(), text(cfg, true).c_str());
}

void test_profiles()
{
    vdm::ZoneConfig z;
    z.profile = vdm::Profile::Floor;
    TEST_ASSERT_EQUAL(3600u, vdm::piParams(z).ti_s);
    z.profile = vdm::Profile::Custom;
    z.xp = 3.0f;
    z.ti_s = 1200;
    TEST_ASSERT_EQUAL_FLOAT(3.0f, vdm::piParams(z).xp);
    TEST_ASSERT_EQUAL(1200u, vdm::piParams(z).ti_s);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_empty_document_gives_defaults);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_secrets_are_hidden);
    RUN_TEST(test_validation);
    RUN_TEST(test_apply_section_merges_objects);
    RUN_TEST(test_apply_section_replaces_arrays);
    RUN_TEST(test_apply_section_assigns_ids_and_drops_references);
    RUN_TEST(test_store_atomic_save_and_load);
    RUN_TEST(test_helpers);
    RUN_TEST(test_legacy_control_source_key);
    RUN_TEST(test_reset);
    RUN_TEST(test_profiles);
    return UNITY_END();
}
