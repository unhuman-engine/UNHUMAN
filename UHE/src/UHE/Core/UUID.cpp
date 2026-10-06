#include "uhepch.h"
#include "UUID.h"

#include <random>
#include <unordered_set>

namespace UHE {
static std::random_device rd;
static std::mt19937_64 s_Engine(rd());
static std::uniform_int_distribution<u64> s_Distribution;

// Issue #38: this set used to be dead weight - drawn IDs were never checked
// against it, so two random 64-bit IDs could (rarely) collide silently. New
// IDs now re-roll until they are unique among the ones minted this run.
// Deserialized IDs take the explicit-value constructor and are not inserted;
// they already exist in persisted scene data.
static std::unordered_set<u64> s_UUIDMap;

UUID::UUID()
{
    do
    {
        m_UUID = s_Distribution(s_Engine);
    } while (!s_UUIDMap.insert(m_UUID).second);
}

UUID::UUID(u64 uuid)
    : m_UUID(uuid)
{
}
} // namespace UHE