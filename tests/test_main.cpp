#include <gtest/gtest.h>
#include "imagine/common/logger.hpp"

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    imagine::Logger::instance().setLevel(imagine::LogLevel::Warn);
    return RUN_ALL_TESTS();
}
