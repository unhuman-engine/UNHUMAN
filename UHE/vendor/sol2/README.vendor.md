# sol2 (vendored, single header)

Source: https://github.com/ThePhD/sol2 — the generated single-header set from the
`develop` branch at commit `c1f95a773c6f8f4fde8ca3efe872e7286afe4444`
(2025-03-07), produced with the repository's own `single/single.py`.

Why `develop` and not the `v3.3.0` release: 3.3.0 does not compile under the
C++23 the engine builds with (`sol::optional<T&>::emplace` calls a base member
that is no longer there). The branch is pinned by commit rather than tracked.

Files: `include/sol/{sol.hpp,forward.hpp,config.hpp}`. `sol.hpp` includes the
other two.

License: MIT (`LICENSE.txt`).

Only `UHE/src/UHE/Scripting/ScriptEngine.cpp` includes it. It is deliberately not
part of any public header: sol2 is a large template library, and exposing it
through the engine's API would put it in every translation unit that mentions
scripting.
