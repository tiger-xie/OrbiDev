#pragma once

namespace rad
{
// 当前 RADP 主版本，与编解码和版本协商使用同一版本。
inline constexpr int kProtocolVersion = 1;
int protocolVersion();
}
