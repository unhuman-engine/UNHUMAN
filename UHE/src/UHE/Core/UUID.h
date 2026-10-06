#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include "UHE/Core/Core.h"
namespace UHE {
class UHE_API UUID {
public:
  UUID();
  UUID(u64 uuid);
  UUID(const UUID &other) = default;

  operator u64() const { return m_UUID; }

private:
  u64 m_UUID;
};
} // namespace UHE

namespace std {
template <> struct hash<UHE::UUID> {
  size_t operator()(const UHE::UUID &uuid) const {
    return hash<u64>()((u64)uuid);
  }
};
} // namespace std
