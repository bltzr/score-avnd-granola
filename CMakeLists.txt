cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
endif()

project(score_addon_granola LANGUAGES CXX)

add_library(
  score_addon_granola
  Granola/GranolaModel.hpp
  Granola/GranolaModel.cpp
  Granola/GranolaUi.hpp
  Granola/Granola.hpp

  score_addon_granola.hpp
  score_addon_granola.cpp
)

setup_score_plugin(score_addon_granola)

target_link_libraries(score_addon_granola PUBLIC score_plugin_engine score_plugin_avnd)