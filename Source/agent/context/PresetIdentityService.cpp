#include "PresetIdentityService.h"

#include <algorithm>
#include <unordered_set>

namespace agentic_dexed::agent::context {
namespace {

constexpr int indexVersion = 1;
constexpr std::int64_t maximumIndexBytes = 512 * 1024;
std::mutex fileMutex;

juce::String string(const std::string& value)
{
    return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
}

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* result = new juce::DynamicObject();
    for (const auto& [name, value] : fields)
        result->setProperty(name, value);
    return juce::var(result);
}

bool exactFields(const juce::var& value, std::initializer_list<const char*> expected)
{
    const auto* dynamic = value.getDynamicObject();
    if (dynamic == nullptr)
        return false;
    std::unordered_set<std::string> fields;
    for (const auto* name : expected)
        fields.emplace(name);
    const auto& properties = dynamic->getProperties();
    if (properties.size() != static_cast<int>(fields.size()))
        return false;
    for (int index = 0; index < properties.size(); ++index)
        if (fields.count(properties.getName(index).toString().toStdString()) == 0)
            return false;
    return true;
}

bool validId(const std::string& value)
{
    return !value.empty() && value.size() <= 64 && !juce::Uuid(string(value)).isNull();
}

juce::String lockName(const juce::File& file)
{
    auto path = file.getFullPathName();
   #if JUCE_WINDOWS
    path = path.toLowerCase();
   #endif
    return "SBFAD-preset-index-" + juce::String::toHexString(path.hashCode64());
}

} // namespace

PresetIdentityService::PresetIdentityService(juce::File indexFile)
    : indexFile_(std::move(indexFile))
{
    loadFromDisk();
}

std::string PresetIdentityService::fingerprint(const CanonicalVoice& voice)
{
    return "sha256:" + juce::SHA256(voice.data(), voice.size()).toHexString().toStdString();
}

PresetActivation PresetIdentityService::activateVoice(const CanonicalVoice& voice)
{
    const auto voiceFingerprint = fingerprint(voice);
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = state_.fingerprintToPreset.find(voiceFingerprint);
        found != state_.fingerprintToPreset.end())
        return { found->second, voiceFingerprint, false };

    const auto id = juce::Uuid().toString().toStdString();
    state_.records.emplace(id, Record { {}, { voiceFingerprint } });
    state_.fingerprintToPreset.emplace(voiceFingerprint, id);
    saveUnlocked();
    return { id, voiceFingerprint, true };
}

bool PresetIdentityService::updateFingerprint(const PresetId& presetId, const CanonicalVoice& voice)
{
    const auto voiceFingerprint = fingerprint(voice);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto record = state_.records.find(presetId);
    if (record == state_.records.end())
        return false;
    const auto existing = state_.fingerprintToPreset.find(voiceFingerprint);
    if (existing != state_.fingerprintToPreset.end() && existing->second != presetId)
        return false;
    if (std::find(record->second.fingerprints.begin(), record->second.fingerprints.end(), voiceFingerprint)
        == record->second.fingerprints.end())
        record->second.fingerprints.push_back(voiceFingerprint);
    state_.fingerprintToPreset[voiceFingerprint] = presetId;
    return saveUnlocked();
}

std::optional<PresetId> PresetIdentityService::clonePreset(const PresetId& source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.records.count(source) == 0)
        return std::nullopt;
    const auto id = juce::Uuid().toString().toStdString();
    state_.records.emplace(id, Record { source, {} });
    if (!saveUnlocked()) {
        state_.records.erase(id);
        return std::nullopt;
    }
    return id;
}

std::optional<PresetId> PresetIdentityService::clonedFrom(const PresetId& presetId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = state_.records.find(presetId);
    if (found == state_.records.end() || found->second.clonedFrom.empty())
        return std::nullopt;
    return found->second.clonedFrom;
}

bool PresetIdentityService::bindSlot(int slot, const PresetId& presetId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 0 || slot >= 32 || state_.records.count(presetId) == 0)
        return false;
    state_.slots[slot] = presetId;
    return saveUnlocked();
}

bool PresetIdentityService::moveSlot(int source, int destination)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (source < 0 || source >= 32 || destination < 0 || destination >= 32)
        return false;
    const auto found = state_.slots.find(source);
    if (found == state_.slots.end())
        return false;
    if (source == destination)
        return true;

    const auto moving = found->second;
    if (source < destination) {
        for (int slot = source; slot < destination; ++slot) {
            const auto next = state_.slots.find(slot + 1);
            if (next == state_.slots.end()) state_.slots.erase(slot);
            else state_.slots[slot] = next->second;
        }
    } else {
        for (int slot = source; slot > destination; --slot) {
            const auto previous = state_.slots.find(slot - 1);
            if (previous == state_.slots.end()) state_.slots.erase(slot);
            else state_.slots[slot] = previous->second;
        }
    }
    state_.slots[destination] = moving;
    return saveUnlocked();
}

bool PresetIdentityService::renamePreset(const PresetId& presetId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.records.count(presetId) != 0;
}

std::optional<PresetId> PresetIdentityService::presetForSlot(int slot) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = state_.slots.find(slot);
    return found == state_.slots.end() ? std::nullopt : std::optional<PresetId>(found->second);
}

juce::var PresetIdentityService::exportIndexUnlocked() const
{
    juce::Array<juce::var> presets;
    std::vector<PresetId> ids;
    ids.reserve(state_.records.size());
    for (const auto& [id, unused] : state_.records) {
        static_cast<void>(unused);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (const auto& id : ids) {
        const auto& record = state_.records.at(id);
        juce::Array<juce::var> fingerprints;
        for (const auto& item : record.fingerprints)
            fingerprints.add(string(item));
        presets.add(object({
            { "id", string(id) },
            { "cloned_from", string(record.clonedFrom) },
            { "fingerprints", juce::var(fingerprints) },
        }));
    }

    juce::Array<juce::var> slots;
    for (const auto& [slot, id] : state_.slots)
        slots.add(object({ { "slot", slot }, { "id", string(id) } }));
    return object({ { "version", indexVersion }, { "presets", juce::var(presets) }, { "slots", juce::var(slots) } });
}

juce::var PresetIdentityService::exportIndex() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return exportIndexUnlocked();
}

std::optional<PresetIdentityService::State> PresetIdentityService::parseIndex(const juce::var& value)
{
    if (!exactFields(value, { "version", "presets", "slots" }))
        return std::nullopt;
    const auto* root = value.getDynamicObject();
    const auto version = root->getProperty("version");
    if ((!version.isInt() && !version.isInt64()) || static_cast<int>(version) != indexVersion)
        return std::nullopt;
    const auto* presets = root->getProperty("presets").getArray();
    const auto* slots = root->getProperty("slots").getArray();
    if (presets == nullptr || slots == nullptr || presets->size() > 4096 || slots->size() > 32)
        return std::nullopt;

    State candidate;
    for (const auto& item : *presets) {
        if (!exactFields(item, { "id", "cloned_from", "fingerprints" }))
            return std::nullopt;
        const auto* recordObject = item.getDynamicObject();
        if (!recordObject->getProperty("id").isString() || !recordObject->getProperty("cloned_from").isString())
            return std::nullopt;
        const auto id = recordObject->getProperty("id").toString().toStdString();
        const auto parent = recordObject->getProperty("cloned_from").toString().toStdString();
        if (!validId(id) || (!parent.empty() && !validId(parent)) || candidate.records.count(id) != 0)
            return std::nullopt;
        const auto* fingerprints = recordObject->getProperty("fingerprints").getArray();
        if (fingerprints == nullptr || fingerprints->size() > 256)
            return std::nullopt;
        Record record { parent, {} };
        for (const auto& fingerprintValue : *fingerprints) {
            if (!fingerprintValue.isString())
                return std::nullopt;
            const auto itemFingerprint = fingerprintValue.toString().toStdString();
            if (itemFingerprint.size() != 71 || itemFingerprint.rfind("sha256:", 0) != 0
                || candidate.fingerprintToPreset.count(itemFingerprint) != 0)
                return std::nullopt;
            record.fingerprints.push_back(itemFingerprint);
            candidate.fingerprintToPreset.emplace(itemFingerprint, id);
        }
        candidate.records.emplace(id, std::move(record));
    }

    for (const auto& item : *slots) {
        if (!exactFields(item, { "slot", "id" }))
            return std::nullopt;
        const auto* slotObject = item.getDynamicObject();
        const auto slotValue = slotObject->getProperty("slot");
        const auto idValue = slotObject->getProperty("id");
        if ((!slotValue.isInt() && !slotValue.isInt64()) || !idValue.isString())
            return std::nullopt;
        const auto slot = static_cast<int>(slotValue);
        const auto id = idValue.toString().toStdString();
        if (slot < 0 || slot >= 32 || candidate.records.count(id) == 0 || candidate.slots.count(slot) != 0)
            return std::nullopt;
        candidate.slots.emplace(slot, id);
    }
    return candidate;
}

bool PresetIdentityService::importIndex(const juce::var& value)
{
    auto candidate = parseIndex(value);
    if (!candidate.has_value())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto previous = state_;
    state_ = std::move(*candidate);
    if (!saveUnlocked()) {
        state_ = previous;
        return false;
    }
    return true;
}

bool PresetIdentityService::saveUnlocked() const
{
    std::lock_guard<std::mutex> processLock(fileMutex);
    juce::InterProcessLock interprocess(lockName(indexFile_));
    if (!interprocess.enter(1500) || !indexFile_.getParentDirectory().createDirectory())
        return false;
    struct Exit { juce::InterProcessLock& lock; ~Exit() { lock.exit(); } } exit { interprocess };
    const auto json = juce::JSON::toString(exportIndexUnlocked(), true);
    if (json.getNumBytesAsUTF8() > maximumIndexBytes)
        return false;
    juce::TemporaryFile temporary(indexFile_);
    return temporary.getFile().replaceWithText(json, false, false, "\n")
        && temporary.overwriteTargetFileWithTemporary();
}

bool PresetIdentityService::save() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return saveUnlocked();
}

void PresetIdentityService::loadFromDisk()
{
    if (!indexFile_.existsAsFile() || indexFile_.getSize() <= 0 || indexFile_.getSize() > maximumIndexBytes
        || indexFile_.isSymbolicLink())
        return;
    std::lock_guard<std::mutex> processLock(fileMutex);
    juce::InterProcessLock interprocess(lockName(indexFile_));
    if (!interprocess.enter(1500))
        return;
    struct Exit { juce::InterProcessLock& lock; ~Exit() { lock.exit(); } } exit { interprocess };
    juce::MemoryBlock bytes;
    if (!indexFile_.loadFileAsData(bytes) || bytes.getSize() > static_cast<std::size_t>(maximumIndexBytes)
        || !juce::CharPointer_UTF8::isValidString(static_cast<const char*>(bytes.getData()), static_cast<int>(bytes.getSize())))
        return;
    juce::var value;
    if (juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()), static_cast<int>(bytes.getSize())), value).failed())
        return;
    if (auto candidate = parseIndex(value); candidate.has_value())
        state_ = std::move(*candidate);
}

} // namespace agentic_dexed::agent::context
