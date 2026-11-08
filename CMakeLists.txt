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
    Granola/Window.hpp
  TARGET granola
  MAIN_CLASS Granola
  NAMESPACE Granola
)

# Granola as released first, for the documents made with it
avnd_score_plugin_add(
  BASE_TARGET score_addon_granola
  SOURCES
    Granola/v1/Granola.hpp
    Granola/v1/GranolaModel.hpp
    Granola/v1/GranolaModel.cpp
    Granola/v1/GranolaUi.hpp
    Granola/v1/grain.hpp
    Granola/v1/grain.cpp
    Granola/v1/utils.hpp
    Granola/v1/utils.cpp
  TARGET granola_v1
  MAIN_CLASS Granola
  NAMESPACE GranolaV1
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
)

if(SCORE_TESTING AND TARGET Catch2::Catch2WithMain AND NOT SCORE_FAST_DEV_BUILD)
  add_executable(score_addon_granola_dsp_test tests/GranolaDspTest.cpp ${GRANOLA_DSP_SOURCES})
  target_include_directories(score_addon_granola_dsp_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_granola_dsp_test PRIVATE Catch2::Catch2WithMain score_plugin_avnd)
  add_test(NAME score_addon_granola_dsp_test COMMAND score_addon_granola_dsp_test)

  add_executable(score_addon_granola_v1_test tests/GranolaV1Test.cpp ${GRANOLA_DSP_SOURCES}
    Granola/v1/GranolaModel.cpp Granola/v1/grain.cpp Granola/v1/utils.cpp)
  target_include_directories(score_addon_granola_v1_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_granola_v1_test PRIVATE Catch2::Catch2WithMain score_plugin_avnd)
  add_test(NAME score_addon_granola_v1_test COMMAND score_addon_granola_v1_test)

  add_executable(score_addon_granola_fuzz_test tests/GranolaFuzzTest.cpp ${GRANOLA_DSP_SOURCES})
  target_include_directories(score_addon_granola_fuzz_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_granola_fuzz_test PRIVATE Catch2::Catch2WithMain score_plugin_avnd)
  add_test(NAME score_addon_granola_fuzz_test COMMAND score_addon_granola_fuzz_test)

  # Granola in score: its panel used while it plays (score's test helpers)
  if(COMMAND score_add_test AND TARGET score_plugin_engine AND TARGET score_plugin_scenario)
    score_add_test(score_addon_granola_app_test
      SOURCES tests/GranolaAppTest.cpp ${GRANOLA_DSP_SOURCES}
      APP
      PLUGINS score_plugin_avnd score_plugin_engine score_plugin_scenario score_lib_process
              score_plugin_media score_plugin_deviceexplorer)
    target_include_directories(score_addon_granola_app_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    # Defined after the addons: its name is resolved when generating
    target_link_libraries(score_addon_granola_app_test PRIVATE score_test_fixtures)
    set_tests_properties(score_addon_granola_app_test PROPERTIES
      TIMEOUT 600 LABELS "integration;granola")
  endif()
endif()

# CPU time of some patches: build it in a release configuration,
#   ninja granola_bench && ./granola_bench [filter]
add_executable(granola_bench EXCLUDE_FROM_ALL tests/GranolaBench.cpp ${GRANOLA_DSP_SOURCES})
target_include_directories(granola_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(granola_bench PRIVATE score_plugin_avnd)
