cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()

if(NOT TARGET score_plugin_avnd)
  return()
endif()

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



# The processor alone, without score around it
set(GRANOLA_DSP_SOURCES
  Granola/GranolaModel.cpp
  Granola/grain.cpp
  Granola/utils.cpp
)

if(SCORE_TESTING AND TARGET Catch2::Catch2WithMain AND NOT SCORE_FAST_DEV_BUILD)
  add_executable(score_addon_granola_dsp_test tests/GranolaDspTest.cpp ${GRANOLA_DSP_SOURCES})
  target_include_directories(score_addon_granola_dsp_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_granola_dsp_test PRIVATE Catch2::Catch2WithMain score_plugin_avnd)
  add_test(NAME score_addon_granola_dsp_test COMMAND score_addon_granola_dsp_test)
endif()

# CPU time of some patches: build it in a release configuration,
#   ninja granola_bench && ./granola_bench [filter]
add_executable(granola_bench EXCLUDE_FROM_ALL tests/GranolaBench.cpp ${GRANOLA_DSP_SOURCES})
target_include_directories(granola_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(granola_bench PRIVATE score_plugin_avnd)
