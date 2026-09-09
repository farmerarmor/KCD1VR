#pragma once

#include <cstddef>
#include <cstdint>

namespace kcdvr {

bool UpdateDxbcChecksum(std::uint8_t* bytecode, std::size_t bytecodeLength);

}  // namespace kcdvr
