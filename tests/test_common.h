#pragma once
// test_common.h
// 测试辅助宏与主函数封装，统一断言与结果输出格式

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error(std::string("assert failed: ") + #cond + \
                                     " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

inline int TestMain(const std::string& name, const std::function<void()>& fn)
{
    try
    {
        fn();
        std::cout << "[PASS] " << name << std::endl;
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[FAIL] " << name << ": " << ex.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAIL] " << name << ": unknown exception" << std::endl;
        return 1;
    }
}
