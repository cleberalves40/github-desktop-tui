cmake_minimum_required(VERSION 3.16)
project(github_desktop_tui VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ftxui REQUIRED)
find_package(Threads REQUIRED)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(github-desktop-tui
    src/main.cpp
    src/github/command_parser.cpp
    src/github/remote_browser.cpp
    src/github/syntax_highlighter.cpp
    src/ui/theme.cpp
    src/git/repository.cpp
    src/process/process_runner.cpp
)

target_include_directories(github-desktop-tui PRIVATE src)
target_compile_definitions(github-desktop-tui PRIVATE
    GITHUB_DESKTOP_TUI_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(github-desktop-tui PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component
    Threads::Threads
)

install(TARGETS github-desktop-tui RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(repository_tests
        tests/repository_tests.cpp
        src/github/command_parser.cpp
        src/github/remote_browser.cpp
        src/github/syntax_highlighter.cpp
        src/ui/theme.cpp
        src/git/repository.cpp
        src/process/process_runner.cpp
    )
    target_include_directories(repository_tests PRIVATE src)
    target_link_libraries(repository_tests PRIVATE ftxui::screen Threads::Threads)
    add_test(NAME repository_integration COMMAND repository_tests)
    add_test(NAME version_smoke COMMAND github-desktop-tui --version)
endif()