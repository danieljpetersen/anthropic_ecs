cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME "anthropic_ecs")
project(${PROJECT_NAME} CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/bin/")

FILE(GLOB_RECURSE APP_SRC_FILES src/*h src/*.cpp src/*.c src/*.cc src/*.hh src/*.hpp src/*.hp)
add_executable(${PROJECT_NAME} ${APP_SRC_FILES})

find_package(Threads REQUIRED)

if(CMAKE_COMPILER_IS_GNUCXX)
    target_link_libraries(${PROJECT_NAME} PRIVATE -lX11 -no-pie)

    target_compile_options(${PROJECT_NAME} PRIVATE
        -fuse-ld=lld
        -std=c++23
        -Wno-sign-compare
        -Waddress
        -Wreturn-type
        -Wall
        -Wextra
        -Wno-unused
        -Wno-exceptions
        -Wpessimizing-move
        -fconcepts
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS)
        target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)
        message("Debug build: Enabled sanitizers and assertions.")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
        message("Release build.")
    endif()
else()
    set(BUILD_ARCH "-m64")
    if(MSVC)
        target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
        target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /std:c++latest)
    endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")


if(UNIX AND NOT APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".out")
endif()
