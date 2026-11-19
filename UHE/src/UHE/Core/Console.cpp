#include "uhepch.h"
#include "Console.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace UHE
{
namespace
{
std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// @brief Parses \p text into a value of the same type as \p like.
/// @return std::nullopt when the text is not a value of that type.
std::optional<Console::Value> Parse(const Console::Value& like, std::string_view text)
{
    if (std::holds_alternative<std::string>(like))
        return Console::Value(std::string(text));

    if (std::holds_alternative<bool>(like))
    {
        const std::string word = Lower(text);
        if (word == "1" || word == "true" || word == "on" || word == "yes")
            return Console::Value(true);
        if (word == "0" || word == "false" || word == "off" || word == "no")
            return Console::Value(false);
        return std::nullopt;
    }

    const char* first = text.data();
    const char* last = text.data() + text.size();
    if (std::holds_alternative<i32>(like))
    {
        i32 value = 0;
        const auto [end, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || end != last)
            return std::nullopt;
        return Console::Value(value);
    }

    float value = 0.0f;
    const auto [end, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || end != last)
        return std::nullopt;
    return Console::Value(value);
}

const char* TypeName(const Console::Value& value)
{
    switch (value.index())
    {
        case 0:
            return "bool";
        case 1:
            return "int";
        case 2:
            return "float";
        default:
            return "string";
    }
}
} // namespace

Console& Console::Get()
{
    static Console console;
    return console;
}

Console::Console()
{
    // Built-ins are entries like any other, so they show up in help and completion.
    m_Entries["help"] = Entry{.Description = "help [name]: list everything, or describe one entry",
                              .IsCommand = true,
                              .Command = [this](const std::vector<std::string>& args) { return Help(args); }};
    m_Entries["find"] = Entry{.Description = "find <text>: entries whose name or description contains text",
                              .IsCommand = true,
                              .Command = [this](const std::vector<std::string>& args) { return Find(args); }};
}

bool Console::RegisterVar(const std::string& name, Value defaultValue, std::string description, ChangedFn onChanged)
{
    std::scoped_lock lock(m_Mutex);
    if (name.empty() || m_Entries.contains(name))
        return false;
    m_Entries[name] = Entry{.Description = std::move(description),
                            .Current = defaultValue,
                            .Default = defaultValue,
                            .OnChanged = std::move(onChanged)};
    return true;
}

bool Console::RegisterCommand(const std::string& name, std::string description, CommandFn fn)
{
    std::scoped_lock lock(m_Mutex);
    if (name.empty() || !fn || m_Entries.contains(name))
        return false;
    m_Entries[name] = Entry{.Description = std::move(description), .IsCommand = true, .Command = std::move(fn)};
    return true;
}

void Console::Unregister(const std::string& name)
{
    if (name == "help" || name == "find")
        return; // built-ins stay
    std::scoped_lock lock(m_Mutex);
    m_Entries.erase(name);
}

bool Console::Has(const std::string& name) const
{
    std::scoped_lock lock(m_Mutex);
    return m_Entries.contains(name);
}

std::optional<Console::Value> Console::GetVar(const std::string& name) const
{
    std::scoped_lock lock(m_Mutex);
    const auto it = m_Entries.find(name);
    if (it == m_Entries.end() || it->second.IsCommand)
        return std::nullopt;
    return it->second.Current;
}

bool Console::GetBool(const std::string& name, bool fallback) const
{
    const auto value = GetVar(name);
    return value && std::holds_alternative<bool>(*value) ? std::get<bool>(*value) : fallback;
}

i32 Console::GetInt(const std::string& name, i32 fallback) const
{
    const auto value = GetVar(name);
    return value && std::holds_alternative<i32>(*value) ? std::get<i32>(*value) : fallback;
}

float Console::GetFloat(const std::string& name, float fallback) const
{
    const auto value = GetVar(name);
    return value && std::holds_alternative<float>(*value) ? std::get<float>(*value) : fallback;
}

std::string Console::GetString(const std::string& name, const std::string& fallback) const
{
    const auto value = GetVar(name);
    return value && std::holds_alternative<std::string>(*value) ? std::get<std::string>(*value) : fallback;
}

bool Console::SetVar(const std::string& name, std::string_view text, std::string* error)
{
    ChangedFn onChanged;
    Value updated;
    {
        std::scoped_lock lock(m_Mutex);
        const auto it = m_Entries.find(name);
        if (it == m_Entries.end() || it->second.IsCommand)
        {
            if (error)
                *error = "unknown variable '" + name + "'";
            return false;
        }

        const auto parsed = Parse(it->second.Current, text);
        if (!parsed)
        {
            if (error)
                *error = "'" + std::string(text) + "' is not a " + TypeName(it->second.Current) + " (" + name + ")";
            return false;
        }
        it->second.Current = *parsed;
        updated = *parsed;
        onChanged = it->second.OnChanged;
    }

    // Outside the lock: the hook may read other variables.
    if (onChanged)
        onChanged(updated);
    return true;
}

std::string Console::Execute(std::string_view line)
{
    std::vector<std::string> tokens = Tokenize(line);
    if (tokens.empty())
        return {};

    const std::string name = tokens.front();
    tokens.erase(tokens.begin());

    CommandFn command;
    bool isVariable = false;
    {
        std::scoped_lock lock(m_Mutex);
        const auto it = m_Entries.find(name);
        if (it == m_Entries.end())
            return "unknown command or variable '" + name + "' (try 'help')";
        if (it->second.IsCommand)
            command = it->second.Command;
        else
            isVariable = true;
    }

    if (command)
        return command(tokens);

    if (isVariable && tokens.empty())
    {
        const auto value = GetVar(name);
        return value ? name + " = " + Format(*value) : std::string();
    }

    // A string variable takes the rest of the line, so `name a b` sets "a b".
    std::string text = tokens.front();
    for (size_t i = 1; i < tokens.size(); ++i)
        text += " " + tokens[i];

    std::string error;
    if (!SetVar(name, text, &error))
        return error;
    const auto value = GetVar(name);
    return value ? name + " = " + Format(*value) : std::string();
}

std::vector<std::string> Console::Complete(std::string_view prefix) const
{
    std::vector<std::string> names;
    {
        std::scoped_lock lock(m_Mutex);
        for (const auto& [name, entry] : m_Entries)
        {
            (void)entry;
            if (name.starts_with(prefix))
                names.push_back(name);
        }
    }
    std::ranges::sort(names);
    return names;
}

std::vector<std::string> Console::Tokenize(std::string_view line)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    bool hasToken = false;
    for (const char c : line)
    {
        if (c == '"')
        {
            quoted = !quoted;
            hasToken = true; // "" is an empty token
            continue;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(c)))
        {
            if (hasToken)
                tokens.push_back(std::move(current));
            current.clear();
            hasToken = false;
            continue;
        }
        current += c;
        hasToken = true;
    }
    if (hasToken)
        tokens.push_back(std::move(current));
    return tokens;
}

std::string Console::Format(const Value& value)
{
    if (const bool* b = std::get_if<bool>(&value))
        return *b ? "true" : "false";
    if (const i32* i = std::get_if<i32>(&value))
        return std::to_string(*i);
    if (const float* f = std::get_if<float>(&value))
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(*f));
        return buffer;
    }
    return "\"" + std::get<std::string>(value) + "\"";
}

std::string Console::Help(const std::vector<std::string>& args) const
{
    std::scoped_lock lock(m_Mutex);
    if (!args.empty())
    {
        const auto it = m_Entries.find(args.front());
        if (it == m_Entries.end())
            return "unknown '" + args.front() + "'";
        const Entry& entry = it->second;
        if (entry.IsCommand)
            return args.front() + " (command): " + entry.Description;
        return args.front() + " (" + TypeName(entry.Current) + ") = " + Format(entry.Current) + ", default " +
               Format(entry.Default) + ": " + entry.Description;
    }

    std::vector<std::string> lines;
    for (const auto& [name, entry] : m_Entries)
        lines.push_back(name + (entry.IsCommand ? "" : " = " + Format(entry.Current)) + "  " + entry.Description);
    std::ranges::sort(lines);

    std::string out;
    for (const std::string& line : lines)
        out += line + "\n";
    if (!out.empty())
        out.pop_back();
    return out;
}

std::string Console::Find(const std::vector<std::string>& args) const
{
    if (args.empty())
        return "usage: find <text>";
    const std::string needle = Lower(args.front());

    std::vector<std::string> names;
    {
        std::scoped_lock lock(m_Mutex);
        for (const auto& [name, entry] : m_Entries)
        {
            if (Lower(name).find(needle) != std::string::npos ||
                Lower(entry.Description).find(needle) != std::string::npos)
                names.push_back(name);
        }
    }
    if (names.empty())
        return "nothing matches '" + args.front() + "'";
    std::ranges::sort(names);

    std::string out;
    for (const std::string& name : names)
        out += name + "\n";
    out.pop_back();
    return out;
}

} // namespace UHE
