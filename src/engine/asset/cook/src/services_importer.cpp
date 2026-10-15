// .mcservices -> CHNK_GSVC: a project's game services catalog (M-LIVEOPS step 2;
// mce/liveops/game_services.h has the format). The chunk is the one liveops::decode_services_catalog
// reads, decoded again before it is stored, so a catalog the runtime would reject never cooks.
//
// {
//   "format": "mce-services", "version": 1, "guid": "<uuid>",
//   "achievements": [ { "id": "first_win", "ios": "...", "android": "..." },
//                     { "id": "win_10", "steps": 10 } ],
//   "leaderboards": [ { "id": "best_time", "order": "lower_is_better", "ios": "...", "android": "..." } ],
//   "cloud_saves": [ "progress" ]
// }
//
// "steps" is a whole number from 1 (the default) to 1000000; "order" is higher_is_better (the
// default) or lower_is_better; "ios", "android" and "steam" (M-STEAM step 2) are optional. A cloud save is a save::write name.

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/liveops/game_services.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.services";
constexpr u32 kServicesFormatVersion = 1;
constexpr f64 kMaxSteps = 1000000.0;

void error(DiagList& diags, StringView file, JsonValue at, StringView msg) {
    detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
}

bool valid_id(StringView s) {
    if (s.empty()) return false;
    for (const char c : s) {
        const bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                         c == '-' || c == '.';
        if (!okc) return false;
    }
    return true;
}

// An optional string member into `out`; false (with an error) when it is not a string or too long.
template <usize N>
bool read_text(JsonValue obj, StringView key, FixedString<N>& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    if (!v.is_string()) {
        String msg;
        appendf(msg, "'%.*s' must be a string", static_cast<int>(key.size()), key.data());
        error(diags, file, v, msg);
        return false;
    }
    out = v.as_string();
    if (out.truncated()) {
        String msg;
        appendf(msg, "'%.*s' is longer than %u characters", static_cast<int>(key.size()), key.data(),
                static_cast<unsigned>(out.capacity()));
        error(diags, file, v, msg);
        return false;
    }
    return true;
}

bool read_id(JsonValue obj, FixedString<64>& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get("id");
    if (!v.is_string() || !valid_id(v.as_string())) {
        error(diags, file, v.valid() ? v : obj, "every entry needs an \"id\" of letters, digits, '_', '-' and '.'");
        return false;
    }
    return read_text(obj, "id", out, file, diags);
}

class ServicesImporter final : public IImporter {
public:
    StringView id() const override { return "services"; }
    u32 version() const override { return 2; }
    ChunkType type() const override { return ChunkType::GameServicesCatalog; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcservices"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "achievements", "leaderboards", "cloud_saves"}, file,
                                kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-services") {
            error(diags, file, fmt.valid() ? fmt : doc, "\"format\" must be \"mce-services\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kServicesFormatVersion) {
            String msg;
            appendf(msg, "unsupported game services version (this cook reads version %u)", kServicesFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        liveops::ServicesCatalog catalog;
        const JsonValue achievements = doc.get("achievements");
        if (achievements.valid() && !achievements.is_array())
            error(diags, file, achievements, "'achievements' must be an array");
        else if (achievements.valid())
            for (u32 i = 0; i < achievements.size(); ++i) {
                const JsonValue a = achievements.at(i);
                if (!detail::check_keys(a, {"id", "steps", "ios", "android", "steam"}, file, kStage, diags)) continue;
                liveops::AchievementInfo info;
                if (!read_id(a, info.id, file, diags)) continue;
                if (!read_text(a, "ios", info.ios_id, file, diags) ||
                    !read_text(a, "android", info.android_id, file, diags) || !read_text(a, "steam", info.steam_id, file, diags))
                    continue;
                const JsonValue steps = a.get("steps");
                if (steps.valid()) {
                    if (!steps.is_integer() || steps.as_number() < 1 || steps.as_number() > kMaxSteps) {
                        error(diags, file, steps, "\"steps\" must be a whole number from 1 to 1000000");
                        continue;
                    }
                    info.steps = static_cast<u32>(steps.as_number());
                }
                if (!catalog.add_achievement(info)) error(diags, file, a, "two achievements share this \"id\"");
            }

        const JsonValue leaderboards = doc.get("leaderboards");
        if (leaderboards.valid() && !leaderboards.is_array())
            error(diags, file, leaderboards, "'leaderboards' must be an array");
        else if (leaderboards.valid())
            for (u32 i = 0; i < leaderboards.size(); ++i) {
                const JsonValue l = leaderboards.at(i);
                if (!detail::check_keys(l, {"id", "order", "ios", "android", "steam"}, file, kStage, diags)) continue;
                liveops::LeaderboardInfo info;
                if (!read_id(l, info.id, file, diags)) continue;
                if (!read_text(l, "ios", info.ios_id, file, diags) ||
                    !read_text(l, "android", info.android_id, file, diags) || !read_text(l, "steam", info.steam_id, file, diags))
                    continue;
                const JsonValue order = l.get("order");
                if (order.valid()) {
                    if (!order.is_string() ||
                        (order.as_string() != "higher_is_better" && order.as_string() != "lower_is_better")) {
                        error(diags, file, order, "\"order\" must be higher_is_better or lower_is_better");
                        continue;
                    }
                    info.lower_is_better = order.as_string() == "lower_is_better";
                }
                if (!catalog.add_leaderboard(info)) error(diags, file, l, "two leaderboards share this \"id\"");
            }

        const JsonValue cloud = doc.get("cloud_saves");
        if (cloud.valid() && !cloud.is_array())
            error(diags, file, cloud, "'cloud_saves' must be an array of save names");
        else if (cloud.valid())
            for (u32 i = 0; i < cloud.size(); ++i) {
                const JsonValue c = cloud.at(i);
                if (!c.is_string() || !valid_id(c.as_string()) || c.as_string().size() > 63) {
                    error(diags, file, c, "a cloud save is a save name: 1-63 letters, digits, '_', '-' and '.'");
                    continue;
                }
                if (!catalog.add_cloud_save(c.as_string())) error(diags, file, c, "this cloud save is listed twice");
            }
        if (diags.error_count() != errors_before) return false;

        Array<byte> bytes;
        liveops::encode_services_catalog(catalog, bytes);
        liveops::ServicesCatalog check;
        if (Result<void> r = liveops::decode_services_catalog(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the game services catalog is not loadable: ");
            msg += why.view();
            error(diags, file, doc, msg);
            return false;
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::GameServicesCatalog;
        d.version = asset::chunk_version(asset::GAME_SERVICES_CATALOG_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_services_importer() { return mem::make_unique<ServicesImporter>(mem::heap()); }

}  // namespace mce::cook
