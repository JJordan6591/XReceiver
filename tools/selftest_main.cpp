#include "pch.h"
#include "SelfTest.h"

#include <cstdio>

int main()
{
    winrt::init_apartment();
    rx::SelfTestResult const result = rx::RunSelfTests();
    std::wprintf(L"%s\n", result.summary.c_str());
    return result.failed == 0 ? 0 : 1;
}
