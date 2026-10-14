#include <windows.h>

#include "common/json.h"
#include "common/settings.h"
#include "common/types.h"
#include "testing.h"

using namespace bta;

TEST(Json, RoundTrip)
{
    auto j = Json::Parse(R"({"a":1,"b":[true,false,null],"c":"x\"yé","d":-2.5,"e":{}})");
    REQUIRE(j.has_value());
    CHECK_EQ(j->Get("a").AsInt(), 1);
    CHECK_EQ(j->Get("b").AsArray().size(), size_t(3));
    CHECK(j->Get("c").AsString() == "x\"y\xC3\xA9");
    CHECK_NEAR(j->Get("d").AsNumber(), -2.5, 1e-12);
    CHECK(j->Get("missing").IsNull());
    auto again = Json::Parse(j->Dump());
    REQUIRE(again.has_value());
    CHECK(again->Dump() == j->Dump());
}

TEST(Json, GetReturnsReferenceUsableInRangeFor)
{
    auto j = Json::Parse(R"({"list":[1,2,3]})");
    REQUIRE(j.has_value());
    int sum = 0;
    for (const auto& v : j->Get("list").AsArray()) sum += static_cast<int>(v.AsInt());
    CHECK_EQ(sum, 6);
    for (const auto& v : j->Get("nope").AsArray()) sum += static_cast<int>(v.AsInt());
    CHECK_EQ(sum, 6);
}

TEST(Json, RejectsMalformed)
{
    std::string err;
    CHECK(!Json::Parse("{\"a\":}", &err));
    CHECK(!err.empty());
    CHECK(!Json::Parse("[1,2"));
    CHECK(!Json::Parse("{} trailing"));
    std::string deep(200, '[');
    CHECK(!Json::Parse(deep));
}

TEST(Types, BdAddr)
{
    auto a = BdAddr::Parse("01:02:03:04:41:87");
    REQUIRE(a.has_value());
    CHECK_EQ(a->value, 0x010203044187ULL);
    CHECK(a->ToString() == "01:02:03:04:41:87");
    CHECK(BdAddr::Parse("010203044187").has_value());
    CHECK(!BdAddr::Parse("01:02:03").has_value());
    CHECK(!BdAddr::Parse("zz:02:03:04:41:87").has_value());
}

TEST(Settings, ValidateAndMerge)
{
    Settings s;
    std::vector<std::string> warn;
    auto j = Json::Parse(R"({"codec":"ldac","ldacQuality":"sq","sampleRate":96000,"bitsPerSample":24,
        "sbc":{"maxBitpool":40,"blocks":8},"sendAheadMs":60,"deviceAddress":"aa-bb-cc-dd-ee-ff","logLevel":"loud"})");
    REQUIRE(j.has_value());
    REQUIRE_OK(s.MergeFrom(*j, &warn));
    CHECK(s.codec == CodecPreference::Ldac);
    CHECK(s.ldacQuality == LdacQuality::Standard);
    CHECK_EQ(s.sampleRate, 96000u);
    CHECK_EQ(int(s.sbc.maxBitpool), 40);
    CHECK_EQ(int(s.sbc.blocks), 8);
    CHECK(s.deviceAddress == "AA:BB:CC:DD:EE:FF");
    CHECK_EQ(warn.size(), size_t(1));

    Settings before = s;
    CHECK(!s.MergeFrom(*Json::Parse(R"({"sampleRate":12345})")).ok());
    CHECK(!s.MergeFrom(*Json::Parse(R"({"codec":"aac"})")).ok());
    CHECK(!s.MergeFrom(*Json::Parse(R"({"sbc":{"maxBitpool":300}})")).ok());
    CHECK(s.ToJson().Dump() == before.ToJson().Dump());  // failed merges leave settings untouched
}

TEST(Settings, AtomicSaveLoad)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"bta_test_settings\\settings.json";
    Settings s;
    s.codec = CodecPreference::Sbc;
    s.sendAheadMs = 120;
    REQUIRE_OK(SaveSettings(path, s));
    Settings l;
    REQUIRE_OK(LoadSettings(path, l));
    CHECK(l.codec == CodecPreference::Sbc);
    CHECK_EQ(l.sendAheadMs, 120u);
    DeleteFileW(path.c_str());
}
