#pragma once

/**
 * @file Console.h
 * @brief In-app console: named variables (cvars) and commands, driven by text.
 *
 * Any system registers what it wants tweakable at runtime ("script.budget",
 * "sim.tickrate") and the editor's Console panel, a headless server's stdin or
 * a test drives it with Execute(). Pure logic with no window or device, so
 * tests/console exercises it directly.
 */

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE
{

/**
 * @brief Registry of console variables and commands.
 *
 * A line is tokenized on whitespace, with double quotes grouping a token:
 *  - `name`          prints a variable, or runs a command with no arguments
 *  - `name value`    sets a variable (parsed to its registered type)
 *  - `name a b ...`  runs a command with its arguments
 *  - `help [name]`, `find <text>` are built in.
 *
 * Thread-safe: values may be read from jobs while the main thread sets them.
 * Callbacks (commands, on-change hooks) run outside the lock, so they may call
 * back into the console.
 */
class UHE_API Console
{
public:
    /// @brief A variable's value; its type is fixed at registration.
    using Value = std::variant<bool, i32, float, std::string>;
    /// @brief A command: gets the arguments after its name, returns what to print.
    using CommandFn = std::function<std::string(const std::vector<std::string>& args)>;
    /// @brief Called after a variable changed, with its new value.
    using ChangedFn = std::function<void(const Value& value)>;

    /// @brief The process-wide console the editor and the engine share.
    static Console& Get();

    Console();

    /**
     * @brief Registers a variable.
     * @param name dotted lowercase name, e.g. "sim.tickrate"
     * @param defaultValue initial value; also fixes the type
     * @param onChanged optional hook run after each successful set
     * @return false when the name is already taken (the existing entry wins)
     */
    bool RegisterVar(const std::string& name, Value defaultValue, std::string description, ChangedFn onChanged = {});

    /// @brief Registers a command. @return false when the name is already taken.
    bool RegisterCommand(const std::string& name, std::string description, CommandFn fn);

    /// @brief Removes a variable or command; owners call it before they die.
    void Unregister(const std::string& name);

    [[nodiscard]] bool Has(const std::string& name) const;
    [[nodiscard]] std::optional<Value> GetVar(const std::string& name) const;

    /// @brief Typed reads: \p fallback when the name is unknown or of another type.
    [[nodiscard]] bool GetBool(const std::string& name, bool fallback = false) const;
    [[nodiscard]] i32 GetInt(const std::string& name, i32 fallback = 0) const;
    [[nodiscard]] float GetFloat(const std::string& name, float fallback = 0.0f) const;
    [[nodiscard]] std::string GetString(const std::string& name, const std::string& fallback = {}) const;

    /**
     * @brief Sets a variable from text, parsed to its registered type.
     * @param error receives why it failed (unknown name, bad value)
     */
    bool SetVar(const std::string& name, std::string_view text, std::string* error = nullptr);

    /// @brief Runs one line and @return the text to show for it.
    std::string Execute(std::string_view line);

    /// @brief Names starting with \p prefix, sorted, for completion.
    [[nodiscard]] std::vector<std::string> Complete(std::string_view prefix) const;

    /// @brief Splits a line on whitespace; "double quotes" group a token.
    [[nodiscard]] static std::vector<std::string> Tokenize(std::string_view line);

    /// @brief A value as Execute() prints it.
    [[nodiscard]] static std::string Format(const Value& value);

private:
    struct Entry
    {
        std::string Description;
        bool IsCommand = false;
        Value Current;
        Value Default;
        ChangedFn OnChanged;
        CommandFn Command;
    };

    std::string Help(const std::vector<std::string>& args) const;
    std::string Find(const std::vector<std::string>& args) const;

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, Entry> m_Entries;
};

} // namespace UHE
