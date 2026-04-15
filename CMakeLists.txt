cmake_minimum_required(VERSION 3.10)
project(sv_generator C)
set(CMAKE_C_STANDARD 11)

# 1. Поиск заголовков (учитываем прямое расположение и подпапку iec61850)
find_path(LIBIEC61850_INC_DIR NAMES sv_publisher.h
    PATHS /usr/include /usr/local/include /usr/include/libiec61850 /usr/local/include/iec61850)

# 2. Поиск библиотеки (явно указываем /usr/lib64 для RHEL-подобных дистрибутивов)
find_library(LIBIEC61850_LIB NAMES libiec61850.so
    PATHS /usr/lib64 /usr/lib /usr/local/lib64 /usr/local/lib)

if(NOT LIBIEC61850_INC_DIR OR NOT LIBIEC61850_LIB)
    message(FATAL_ERROR "libiec61850 не найдена.\n"
            "  Include dir: ${LIBIEC61850_INC_DIR}\n"
            "  Library: ${LIBIEC61850_LIB}\n"
            "  Попробуйте: cmake .. -DCMAKE_PREFIX_PATH=/usr")
endif()

message(STATUS "✅ Found libiec61850:")
message(STATUS "   Headers: ${LIBIEC61850_INC_DIR}")
message(STATUS "   Library: ${LIBIEC61850_LIB}")

add_executable(sv_generator main.c)
target_include_directories(sv_generator PRIVATE ${LIBIEC61850_INC_DIR})
target_link_libraries(sv_generator PRIVATE ${LIBIEC61850_LIB} m pthread rt)

set(CMAKE_C_FLAGS_RELEASE "-O3 -Wall -Wextra -fno-strict-aliasing -march=native")
