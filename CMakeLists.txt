cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()

include(ScoreAvndHelper)

project(score_addon_granola LANGUAGES CXX)

avnd_score_plugin_init(
  BASE_TARGET score_addon_granola
)

avnd_score_plugin_add(
  BASE_TARGET score_addon_granola
  SOURCES
    Granola/Granola.hpp
    Granola/GranolaModel.hpp
    Granola/GranolaModel.cpp
    Granola/GranolaUi.hpp
    Granola/grain.hpp
    Granola/grain.cpp
    Granola/utils.hpp
    Granola/utils.cpp
  TARGET granola
  MAIN_CLASS Granola
  NAMESPACE Granola
)

avnd_score_plugin_finalize(
  BASE_TARGET score_addon_granola
  PLUGIN_VERSION 1
  PLUGIN_UUID "1C857777-FE04-4A7D-9428-137954C83E8A"
)


