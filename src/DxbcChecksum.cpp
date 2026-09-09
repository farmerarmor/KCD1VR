#include "DxbcChecksum.h"

#include <cstring>

namespace kcdvr {
namespace {

// DXBC uses a modified MD5 container checksum. This compact implementation is
// derived from AMD's freely usable DXBCChecksum reference implementation:
// https://github.com/GPUOpen-Archive/common-src-ShaderUtils/tree/master/DX10
// The MD5 algorithm is derived from the RSA Data Security, Inc. reference MD5
// implementation and is identified here as such, as required by its notice.

constexpr std::uint32_t kConstants[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};

constexpr unsigned kRotations[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};

std::uint32_t RotateLeft(std::uint32_t value, unsigned amount)
{
    return (value << amount) | (value >> (32U - amount));
}

void Transform(std::uint32_t state[4], const std::uint8_t block[64])
{
    std::uint32_t words[16]{};
    std::memcpy(words, block, sizeof(words));
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    for (unsigned i = 0; i < 64; ++i) {
        std::uint32_t function = 0;
        std::uint32_t word = 0;
        if (i < 16) {
            function = (b & c) | (~b & d);
            word = i;
        } else if (i < 32) {
            function = (d & b) | (~d & c);
            word = (5U * i + 1U) % 16U;
        } else if (i < 48) {
            function = b ^ c ^ d;
            word = (3U * i + 5U) % 16U;
        } else {
            function = c ^ (b | ~d);
            word = (7U * i) % 16U;
        }
        const std::uint32_t previousD = d;
        d = c;
        c = b;
        b += RotateLeft(a + function + kConstants[i] + words[word], kRotations[i]);
        a = previousD;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

}  // namespace

bool UpdateDxbcChecksum(std::uint8_t* bytecode, std::size_t bytecodeLength)
{
    constexpr std::size_t hashEnd = 0x14;
    if (!bytecode || bytecodeLength <= hashEnd ||
        std::memcmp(bytecode, "DXBC", 4) != 0) {
        return false;
    }

    std::uint32_t state[4]{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::uint8_t* payload = bytecode + hashEnd;
    const std::size_t payloadSize = bytecodeLength - hashEnd;
    const std::uint32_t bitCount = static_cast<std::uint32_t>(payloadSize * 8U);
    const std::size_t fullSize = payloadSize & ~std::size_t(63);
    for (std::size_t offset = 0; offset < fullSize; offset += 64) {
        Transform(state, payload + offset);
    }

    const std::size_t remaining = payloadSize - fullSize;
    std::uint8_t block[64]{};
    if (remaining >= 56) {
        std::memcpy(block, payload + fullSize, remaining);
        block[remaining] = 0x80;
        Transform(state, block);
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &bitCount, sizeof(bitCount));
    } else {
        std::memcpy(block, &bitCount, sizeof(bitCount));
        std::memcpy(block + sizeof(bitCount), payload + fullSize, remaining);
        block[sizeof(bitCount) + remaining] = 0x80;
    }
    const std::uint32_t terminator = (bitCount >> 2U) | 1U;
    std::memcpy(block + 60, &terminator, sizeof(terminator));
    Transform(state, block);
    std::memcpy(bytecode + 4, state, sizeof(state));
    return true;
}

}  // namespace kcdvr
