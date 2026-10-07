#include "ConversationSerialization.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace agentic_dexed::agent::context {
namespace {

juce::String text(const std::string& value)
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

const char* roleName(ConversationRole role)
{
    switch (role) {
        case ConversationRole::user: return "user";
        case ConversationRole::assistant: return "assistant";
        case ConversationRole::toolCall: return "tool_call";
        case ConversationRole::toolResult: return "tool_result";
    }
    return "invalid";
}

const char* terminalName(TurnTerminalState state)
{
    switch (state) {
        case TurnTerminalState::completed: return "completed";
        case TurnTerminalState::failed: return "failed";
        case TurnTerminalState::cancelled: return "cancelled";
    }
    return "invalid";
}

bool parseRole(const juce::String& value, ConversationRole& role)
{
    if (value == "user") role = ConversationRole::user;
    else if (value == "assistant") role = ConversationRole::assistant;
    else if (value == "tool_call") role = ConversationRole::toolCall;
    else if (value == "tool_result") role = ConversationRole::toolResult;
    else return false;
    return true;
}

bool parseTerminal(const juce::String& value, TurnTerminalState& state)
{
    if (value == "completed") state = TurnTerminalState::completed;
    else if (value == "failed") state = TurnTerminalState::failed;
    else if (value == "cancelled") state = TurnTerminalState::cancelled;
    else return false;
    return true;
}

bool validText(const juce::String& value, std::size_t maximumBytes, std::size_t& budget)
{
    const auto bytes = static_cast<std::size_t>(value.getNumBytesAsUTF8());
    if (bytes > maximumBytes || bytes > kMaximumContextBytes - std::min(budget, kMaximumContextBytes))
        return false;

    for (auto cursor = value.getCharPointer(); !cursor.isEmpty(); ++cursor) {
        const auto character = static_cast<std::uint32_t>(*cursor);
        if (character == 0xfffd || (character < 0x20 && character != '\n' && character != '\r' && character != '\t'))
            return false;
    }
    budget += bytes;
    return budget <= kMaximumContextBytes;
}

bool allowedProperties(const juce::var& value, std::initializer_list<const char*> allowed)
{
    const auto* dynamic = value.getDynamicObject();
    if (dynamic == nullptr)
        return false;

    std::unordered_set<std::string> names;
    for (const auto* name : allowed)
        names.emplace(name);

    const auto& properties = dynamic->getProperties();
    for (int i = 0; i < properties.size(); ++i) {
        const auto name = properties.getName(i).toString().toStdString();
        if (names.count(name) == 0 && name.rfind("x_", 0) != 0)
            return false;
    }
    return true;
}

bool requiredString(const juce::var& value, const char* name, std::size_t maximum,
    std::size_t& budget, std::string& destination, bool allowEmpty = false)
{
    const auto* dynamic = value.getDynamicObject();
    if (dynamic == nullptr || !dynamic->hasProperty(name))
        return false;
    const auto property = dynamic->getProperty(name);
    if (!property.isString())
        return false;
    const auto stringValue = property.toString();
    if ((!allowEmpty && stringValue.isEmpty()) || !validText(stringValue, maximum, budget))
        return false;
    destination = stringValue.toStdString();
    return true;
}

bool requiredUnsigned(const juce::var& value, const char* name, std::uint64_t& destination)
{
    const auto* dynamic = value.getDynamicObject();
    if (dynamic == nullptr || !dynamic->hasProperty(name))
        return false;
    const auto property = dynamic->getProperty(name);
    if (!property.isInt() && !property.isInt64())
        return false;
    const auto signedValue = static_cast<juce::int64>(property);
    if (signedValue < 0)
        return false;
    destination = static_cast<std::uint64_t>(signedValue);
    return true;
}

ContextParseResult failure(const char* error)
{
    return { std::nullopt, error };
}

} // namespace

juce::var serializeContext(const PresetConversationContext& context)
{
    juce::Array<juce::var> turns;
    for (const auto& turn : context.recentTurns) {
        juce::Array<juce::var> messages;
        for (const auto& message : turn.messages) {
            messages.add(object({
                { "role", roleName(message.role) },
                { "text", text(message.text) },
                { "call_id", text(message.callId) },
                { "tool_name", text(message.toolName) },
                { "success", message.success },
            }));
        }

        juce::Array<juce::var> transactions;
        for (const auto& transaction : turn.transactions) {
            transactions.add(object({
                { "transaction_id", text(transaction.transactionId) },
                { "summary", text(transaction.summary) },
                { "resulting_revision", static_cast<juce::int64>(transaction.resultingRevision) },
            }));
        }

        turns.add(object({
            { "turn_id", text(turn.turnId) },
            { "terminal_state", terminalName(turn.terminalState) },
            { "messages", juce::var(messages) },
            { "transactions", juce::var(transactions) },
        }));
    }

    juce::Array<juce::var> aliases;
    for (const auto& alias : context.fingerprintAliases)
        aliases.add(text(alias));

    return object({
        { "version", context.version },
        { "preset_id", text(context.presetId) },
        { "revision", static_cast<juce::int64>(context.revision) },
        { "summary", text(context.summary) },
        { "recent_turns", juce::var(turns) },
        { "fingerprint_aliases", juce::var(aliases) },
        { "updated_at_unix_ms", static_cast<juce::int64>(context.updatedAtUnixMs) },
    });
}

ContextParseResult parseContext(const juce::var& value)
{
    if (!allowedProperties(value, { "version", "preset_id", "revision", "summary",
            "recent_turns", "fingerprint_aliases", "updated_at_unix_ms" }))
        return failure("invalid root fields");

    const auto* root = value.getDynamicObject();
    const auto versionValue = root->getProperty("version");
    if ((!versionValue.isInt() && !versionValue.isInt64()) || static_cast<int>(versionValue) != kPresetContextVersion)
        return failure("unsupported context version");

    std::size_t budget = 512;
    PresetConversationContext context;
    context.version = kPresetContextVersion;
    if (!requiredString(value, "preset_id", 128, budget, context.presetId))
        return failure("invalid preset id");
    if (!requiredUnsigned(value, "revision", context.revision))
        return failure("invalid revision");
    if (!requiredString(value, "summary", kMaximumSummaryBytes, budget, context.summary, true))
        return failure("invalid summary");

    const auto updated = root->getProperty("updated_at_unix_ms");
    if ((!updated.isInt() && !updated.isInt64()) || static_cast<juce::int64>(updated) < 0)
        return failure("invalid update time");
    context.updatedAtUnixMs = static_cast<juce::int64>(updated);

    const auto* aliases = root->getProperty("fingerprint_aliases").getArray();
    if (aliases == nullptr || aliases->size() > 128)
        return failure("invalid fingerprint aliases");
    for (const auto& alias : *aliases) {
        if (!alias.isString() || !validText(alias.toString(), 256, budget))
            return failure("invalid fingerprint alias");
        context.fingerprintAliases.push_back(alias.toString().toStdString());
    }

    const auto* turns = root->getProperty("recent_turns").getArray();
    if (turns == nullptr || static_cast<std::size_t>(turns->size()) > kMaximumRecentTurns)
        return failure("invalid recent turns");

    for (const auto& turnValue : *turns) {
        if (!allowedProperties(turnValue, { "turn_id", "terminal_state", "messages", "transactions" }))
            return failure("invalid turn fields");

        ConversationTurn turn;
        if (!requiredString(turnValue, "turn_id", 256, budget, turn.turnId))
            return failure("invalid turn id");
        std::string terminal;
        if (!requiredString(turnValue, "terminal_state", 32, budget, terminal)
            || !parseTerminal(text(terminal), turn.terminalState))
            return failure("invalid terminal state");

        const auto* messages = turnValue.getDynamicObject()->getProperty("messages").getArray();
        if (messages == nullptr || messages->isEmpty() || messages->size() > 256)
            return failure("invalid messages");

        std::unordered_map<std::string, std::string> pendingTools;
        for (const auto& messageValue : *messages) {
            if (!allowedProperties(messageValue, { "role", "text", "call_id", "tool_name", "success" }))
                return failure("invalid message fields");

            ConversationMessage message;
            std::string role;
            if (!requiredString(messageValue, "role", 32, budget, role)
                || !parseRole(text(role), message.role)
                || !requiredString(messageValue, "text", kMaximumMessageBytes, budget, message.text, true)
                || !requiredString(messageValue, "call_id", 256, budget, message.callId, true)
                || !requiredString(messageValue, "tool_name", 256, budget, message.toolName, true))
                return failure("invalid message");

            const auto success = messageValue.getDynamicObject()->getProperty("success");
            if (!success.isBool())
                return failure("invalid message success");
            message.success = static_cast<bool>(success);

            if (message.role == ConversationRole::toolCall) {
                if (message.callId.empty() || message.toolName.empty()
                    || !pendingTools.emplace(message.callId, message.toolName).second)
                    return failure("invalid tool call");
            } else if (message.role == ConversationRole::toolResult) {
                const auto found = pendingTools.find(message.callId);
                if (found == pendingTools.end() || found->second != message.toolName)
                    return failure("unpaired tool result");
                pendingTools.erase(found);
            } else if (!message.callId.empty() || !message.toolName.empty()) {
                return failure("non-tool message has tool fields");
            }
            turn.messages.push_back(std::move(message));
        }
        if (!pendingTools.empty())
            return failure("unpaired tool call");

        const auto* transactions = turnValue.getDynamicObject()->getProperty("transactions").getArray();
        if (transactions == nullptr || transactions->size() > 256)
            return failure("invalid transactions");
        for (const auto& transactionValue : *transactions) {
            if (!allowedProperties(transactionValue, { "transaction_id", "summary", "resulting_revision" }))
                return failure("invalid transaction fields");
            TransactionSummary transaction;
            if (!requiredString(transactionValue, "transaction_id", 256, budget, transaction.transactionId)
                || !requiredString(transactionValue, "summary", 4096, budget, transaction.summary, true)
                || !requiredUnsigned(transactionValue, "resulting_revision", transaction.resultingRevision))
                return failure("invalid transaction");
            turn.transactions.push_back(std::move(transaction));
        }
        context.recentTurns.push_back(std::move(turn));
    }

    if (budget > kMaximumContextBytes)
        return failure("context exceeds size limit");
    return { std::move(context), {} };
}

ContextParseResult parseContextJson(std::string_view bytes)
{
    if (bytes.empty() || bytes.size() > kMaximumContextBytes
        || bytes.find('\0') != std::string_view::npos
        || !juce::CharPointer_UTF8::isValidString(bytes.data(), static_cast<int>(bytes.size())))
        return failure("context JSON is not valid UTF-8");

    juce::var value;
    const auto status = juce::JSON::parse(
        juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())), value);
    if (status.failed())
        return failure("context JSON is invalid");
    return parseContext(value);
}

} // namespace agentic_dexed::agent::context
