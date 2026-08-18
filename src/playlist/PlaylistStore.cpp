#include "playlist/PlaylistStore.h"

#include <fstream>
#include <string>
#include <utility>

#include "logging/Logger.h"
#include "util/json.h"
#include "util/utf8.h"

namespace vw::playlist {

namespace {

constexpr wchar_t kKeyMode[] = L"mode";
constexpr wchar_t kKeyLoop[] = L"loop";
constexpr wchar_t kKeyCurrent[] = L"current";
constexpr wchar_t kKeyShuffleOrder[] = L"shuffleOrder";
constexpr wchar_t kKeyItems[] = L"items";
constexpr wchar_t kKeyPath[] = L"path";
constexpr wchar_t kKeyStart[] = L"start100ns";
constexpr wchar_t kKeyEnd[] = L"end100ns";
constexpr wchar_t kKeyEnabled[] = L"enabled";
constexpr wchar_t kKeyDuration[] = L"duration100ns";
constexpr wchar_t kKeyWidth[] = L"width";
constexpr wchar_t kKeyHeight[] = L"height";
constexpr wchar_t kKeyCodec[] = L"codec";

PlaylistItem itemFromJson(const util::Json& j) {
    PlaylistItem item;
    item.path = j.get(kKeyPath).asString();
    item.start100ns = j.get(kKeyStart).asInt(0);
    item.end100ns = j.get(kKeyEnd).asInt(0);
    item.enabled = j.get(kKeyEnabled).asBool(true);
    item.duration100ns = j.get(kKeyDuration).asInt(0);
    item.width = static_cast<uint32_t>(j.get(kKeyWidth).asInt(0));
    item.height = static_cast<uint32_t>(j.get(kKeyHeight).asInt(0));
    item.codec = j.get(kKeyCodec).asString();
    return item;
}

util::Json itemToJson(const PlaylistItem& item) {
    util::Json::Object o;
    o.emplace(kKeyPath, util::Json::string(item.path));
    o.emplace(kKeyStart, util::Json::number(static_cast<double>(item.start100ns)));
    o.emplace(kKeyEnd, util::Json::number(static_cast<double>(item.end100ns)));
    o.emplace(kKeyEnabled, util::Json::boolean(item.enabled));
    o.emplace(kKeyDuration, util::Json::number(static_cast<double>(item.duration100ns)));
    o.emplace(kKeyWidth, util::Json::number(static_cast<double>(item.width)));
    o.emplace(kKeyHeight, util::Json::number(static_cast<double>(item.height)));
    o.emplace(kKeyCodec, util::Json::string(item.codec));
    return util::Json::object(std::move(o));
}

} // namespace

const wchar_t* PlaylistStore::modeName(Mode mode) {
    switch (mode) {
        case Mode::Single: return L"single";
        case Mode::Sequential: return L"sequential";
        case Mode::Loop: return L"loop";
        case Mode::Shuffle: return L"shuffle";
    }
    return L"loop";
}

std::optional<Mode> PlaylistStore::modeFromName(const std::wstring& name) {
    if (name == L"single") return Mode::Single;
    if (name == L"sequential") return Mode::Sequential;
    if (name == L"loop") return Mode::Loop;
    if (name == L"shuffle") return Mode::Shuffle;
    return std::nullopt;
}

std::optional<PlaylistData> PlaylistStore::load(const std::filesystem::path& path) {
    auto& log = log::Logger::instance();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log.debug(L"playlist store: no file at {} — first run", path.wstring());
        return std::nullopt;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto wide = util::utf8ToWide(bytes);
    if (!wide) {
        log.warn(L"playlist store: invalid UTF-8 at {} — using defaults", path.wstring());
        return std::nullopt;
    }
    auto parsed = util::Json::parse(*wide);
    if (!parsed) {
        log.warn(L"playlist store: corrupt JSON at {} — using defaults", path.wstring());
        return std::nullopt;
    }
    const util::Json& root = *parsed;
    const int64_t version = root.get(L"version").asInt(1);
    if (version > kFormatVersion) {
        log.warn(L"playlist store: unsupported format version {} at {}", version, path.wstring());
        return std::nullopt;
    }

    PlaylistData data;
    if (auto mode = modeFromName(root.get(kKeyMode).asString()); mode.has_value()) {
        data.mode = *mode;
    }
    data.loop = root.get(kKeyLoop).asBool(data.loop);
    // kNoIndex is serialized as -1 (docs in save()); a missing key defaults to
    // -1 too. Any negative read is kNoIndex — never cast a huge double that a
    // 2^64 value would produce (out-of-range int64 cast is UB).
    const int64_t current = root.get(kKeyCurrent).asInt(-1);
    data.current = current < 0 ? PlaylistManager::kNoIndex : static_cast<size_t>(current);
    for (const util::Json& v : root.get(kKeyShuffleOrder).asArray()) {
        const int64_t i = v.asInt(-1);
        if (i >= 0) {
            data.shuffleOrder.push_back(static_cast<size_t>(i));
        }
    }
    for (const util::Json& v : root.get(kKeyItems).asArray()) {
        data.items.push_back(itemFromJson(v));
    }
    log.info(L"playlist store: loaded {} item(s) from {}", data.items.size(), path.wstring());
    return data;
}

Result<void> PlaylistStore::save(const std::filesystem::path& path, const PlaylistData& data) {
    util::Json::Array items;
    for (const PlaylistItem& item : data.items) {
        items.push_back(itemToJson(item));
    }
    util::Json::Array order;
    for (const size_t i : data.shuffleOrder) {
        order.push_back(util::Json::number(static_cast<double>(i)));
    }
    util::Json::Object root;
    root.emplace(L"version", util::Json::number(kFormatVersion));
    root.emplace(kKeyMode, util::Json::string(modeName(data.mode)));
    root.emplace(kKeyLoop, util::Json::boolean(data.loop));
    // Serialize kNoIndex as -1: casting size_t(-1) to double (1.84e19) and
    // back through int64_t on load is an out-of-range cast (UB).
    const int64_t current =
        data.current == PlaylistManager::kNoIndex ? -1 : static_cast<int64_t>(data.current);
    root.emplace(kKeyCurrent, util::Json::number(static_cast<double>(current)));
    root.emplace(kKeyShuffleOrder, util::Json::array(std::move(order)));
    root.emplace(kKeyItems, util::Json::array(std::move(items)));

    // Atomic write (temp + rename), same pattern as the config loader: a
    // crash mid-write never leaves a torn playlist file.
    const std::filesystem::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected(L"playlist save: cannot open " + tmp.wstring());
        }
        const std::string bytes = util::wideToUtf8(util::Json::object(std::move(root)).serialize());
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            return std::unexpected(L"playlist save: write failed for " + tmp.wstring());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        const auto msg = util::utf8ToWide(ec.message());
        return std::unexpected(L"playlist save: rename failed: " +
                               (msg ? *msg : L"(unknown error)"));
    }
    return {};
}

} // namespace vw::playlist
