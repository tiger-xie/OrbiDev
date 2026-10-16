#pragma once

#include <QString>

namespace rad
{
// 返回实际编译使用的 Qt 版本，供两个入口报告构建基线，不包含本机路径。
QString buildBaseline();
}
