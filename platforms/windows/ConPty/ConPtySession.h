#pragma once
#include "ConPtyProcess.h"
namespace rad::poc
{
// 技术验证入口保留严格的 EXE 同工作目录限制，底层资源实现与产品共用。
class ConPtySession final : public windows::ConPtyProcess
{
private:
    using windows::ConPtyProcess::enqueueInput;
    using windows::ConPtyProcess::startValidated;
};
}
