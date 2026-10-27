#pragma once
#include <array>
#include <cstdint>

namespace extractor::defaults {
inline constexpr float moverHeight=150;
inline constexpr std::array<std::array<float,3>,4> moverRange{{{-80,-80,0},{80,-80,0},{80,80,0},{-80,80,0}}};
inline constexpr bool waterVolume=true;
inline constexpr int32_t physicsVolumePriority=0;
}
