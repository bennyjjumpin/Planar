cmake_minimum_required(VERSION 3.24)
project(music_player LANGUAGES C CXX)

# Some fetched dependencies declare very old cmake_minimum_required() versions,
# which CMake 4.x rejects outright. This tells CMake to treat them as 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(client)
