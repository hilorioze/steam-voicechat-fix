cmake_minimum_required(VERSION 3.13)
project(steam_voicechat_fix C)

find_package(Threads REQUIRED)

add_compile_options(
  -m32
  -Wall
  -Wextra
)

add_link_options(-m32)

add_library(steam_voicechat_fix SHARED
  src/voicechat_fix.c
)

target_link_libraries(steam_voicechat_fix PRIVATE Threads::Threads dl)
