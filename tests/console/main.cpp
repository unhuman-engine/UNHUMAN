// Console harness: variables parse to their registered type, commands get
// their arguments, quoting groups tokens, help/find/completion list what is
// registered, and a hook may call back into the console without deadlocking.

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "UHE/Core/Console.h"

using UHE::Console;

namespace
{
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    std::cout << (condition ? "  ok   - " : "  FAIL - ") << what << '\n';
    g_failures += condition ? 0 : 1;
}

void section(const char* name)
{
    std::cout << "\n[ " << name << " ]\n";
}
} // namespace

int main()
{
    section("tokenize");
    {
        const auto tokens = Console::Tokenize("  set  \"two words\"  x\t\"\" ");
        check(tokens.size() == 4, "whitespace splits, quotes group");
        check(tokens.size() == 4 && tokens[1] == "two words" && tokens[3].empty(), "a quoted token keeps its spaces");
        check(Console::Tokenize("   ").empty(), "a blank line has no tokens");
    }

    section("variables");
    {
        Console console;
        int changes = 0;
        check(console.RegisterVar("sim.paused", false, "pause", [&](const Console::Value&) { ++changes; }),
              "registers a bool");
        check(console.RegisterVar("sim.tickrate", 120, "rate"), "registers an int");
        check(console.RegisterVar("sim.scale", 1.0f, "scale"), "registers a float");
        check(console.RegisterVar("player.name", std::string("ada"), "name"), "registers a string");
        check(!console.RegisterVar("sim.tickrate", 60, "again"), "a taken name is refused");
        check(console.GetInt("sim.tickrate") == 120, "and keeps the first registration");

        check(console.Execute("sim.paused on") == "sim.paused = true", "a bool parses on/off");
        check(console.GetBool("sim.paused") && changes == 1, "and runs the change hook");
        check(console.Execute("sim.tickrate 60") == "sim.tickrate = 60", "an int parses");
        check(console.Execute("sim.tickrate 6x").find("not a int") != std::string::npos, "a bad int is refused");
        check(console.GetInt("sim.tickrate") == 60, "and leaves the value alone");
        check(console.Execute("sim.scale 0.5") == "sim.scale = 0.5", "a float parses");
        check(console.GetFloat("sim.scale") == 0.5f, "and reads back");
        check(console.Execute("player.name grace hopper") == "player.name = \"grace hopper\"",
              "a string takes the rest of the line");
        check(console.Execute("sim.scale") == "sim.scale = 0.5", "a bare name prints the value");
        check(console.GetInt("sim.scale", 7) == 7, "a typed read of the wrong type falls back");
        check(console.Execute("nope 1").find("unknown") != std::string::npos, "an unknown name says so");
    }

    section("commands");
    {
        Console console;
        std::vector<std::string> seen;
        console.RegisterCommand("echo", "echo args",
                                [&](const std::vector<std::string>& args)
                                {
                                    seen = args;
                                    return std::string("done");
                                });
        check(console.Execute("echo a \"b c\"") == "done", "a command returns its text");
        check(seen.size() == 2 && seen[1] == "b c", "and receives its arguments");

        console.RegisterVar("x", 1, "x");
        console.RegisterCommand("bump", "x += 1",
                                [&](const std::vector<std::string>&)
                                {
                                    console.SetVar("x", std::to_string(console.GetInt("x") + 1));
                                    return console.Execute("x");
                                });
        check(console.Execute("bump") == "x = 2", "a command may call back into the console");

        console.Unregister("echo");
        check(!console.Has("echo"), "unregister removes it");
        console.Unregister("help");
        check(console.Has("help"), "built-ins cannot be removed");
    }

    section("help, find and completion");
    {
        Console console;
        console.RegisterVar("render.vsync", true, "wait for vblank");
        console.RegisterVar("render.scale", 1.0f, "resolution scale");
        const std::string help = console.Execute("help");
        check(help.find("render.vsync = true") != std::string::npos, "help lists variables with values");
        check(console.Execute("help render.scale").find("default 1") != std::string::npos, "help names a default");
        check(console.Execute("find vblank") == "render.vsync", "find searches descriptions");
        const auto names = console.Complete("render.");
        check(names.size() == 2 && names[0] == "render.scale", "completion is sorted by name");
    }

    section("concurrent reads");
    {
        Console console;
        console.RegisterVar("n", 0, "n");
        std::atomic<bool> stop{false};
        std::thread reader(
            [&]
            {
                while (!stop.load())
                    (void)console.GetInt("n");
            });
        for (int i = 0; i < 2000; ++i)
            console.SetVar("n", std::to_string(i));
        stop = true;
        reader.join();
        check(console.GetInt("n") == 1999, "reads on another thread see the last set");
    }

    std::cout << (g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return g_failures == 0 ? 0 : 1;
}
