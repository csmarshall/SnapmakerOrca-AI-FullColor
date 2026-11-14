get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_bambu_filament_ids.cpp
    test_bed_exclusion_fields.cpp
    test_cloud_delete_queue.cpp
    test_config_value_formatter.cpp
    test_creality_cfs_match.cpp
    test_creality_print.cpp
    test_dev_mapping.cpp
    test_device_access.cpp
    test_send_dialog_options.cpp
    test_print_preference.cpp
    test_snapmaker_preprint_mapping.cpp
    test_snapmaker_preprint_gate.cpp
    test_moonraker_printer_agent.cpp
    test_spoolman_join.cpp
    test_spoolman_direct.cpp
    test_network_discovery.cpp
    test_filament_bitmap_utils.cpp
    test_colour_picker_field.cpp
    test_filament_color_picker.cpp
    test_ams_materials_setting.cpp
    test_gcode_preview_colors.cpp
    test_device_progress.cpp
    test_device_manager_integration.cpp
    test_web_media_controller.cpp
    test_scene_raycaster.cpp
    test_scene_benchmark.cpp
    test_lazy.cpp
    test_prebuild_queue.cpp
    test_staged_build.cpp
    test_network_versions.cpp
    test_orca_cloud_agent.cpp
    test_action_source.cpp
    test_option_state_registry.cpp
    test_plugin_host_api.cpp
    # Exercise seam enums and predicates through the embedded Python host API.
    test_precise_seam_plugin.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_status.cpp
    test_printer_agent.cpp
    test_qidi_printer_agent.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_plugin_resource_texture.cpp
    test_plugin_printer_agent.cpp
    test_post_processor.cpp
    test_printhost.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_post_process_approval.cpp
    test_plugin_json_depth.cpp
    test_plugin_permission_validation.cpp
    test_shortcuts.cpp
    test_speed_dial_dismiss.cpp
    test_speed_dial_position.cpp
    test_texture_color_palette.cpp
    test_texture_island_view.cpp
    test_timelapse_codecs.cpp
    test_file_url.cpp
    test_web_page_server.cpp
    test_sswcp.cpp
    test_snapmaker_device_ui.cpp
    test_print_host_agents.cpp
    test_imgui_wrapper.cpp
    test_user_manager.cpp
    test_sequential_clearance.cpp
    test_object_list.cpp
    ../fff_print/test_helpers.cpp
    )

if (SLIC3R_CAD)
    target_sources(${_TEST_NAME}_tests PRIVATE test_design_sketch_tool.cpp)
endif ()

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2WithMain)
if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(${_TEST_NAME}_tests ${CMAKE_SOURCE_DIR}/src/slic3r/pchheader.hpp FORCEINCLUDE)
endif ()

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls(${_TEST_NAME}_tests)

if (WIN32)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_PREFIX_PATH}/libpython" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_PREFIX_PATH}/libpython/python${_bundled_python_abi}.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>"
        COMMENT "Copying Python runtime for slic3rutils plugin host API tests"
        VERBATIM
    )
elseif (NOT FLATPAK)
    # The CI unit-test runner only receives the build/tests tree, so both the
    # interpreter and the libpython the test binary links have to travel next to
    # the executable; find_bundled_python_home() picks the copy up from there.
    if (APPLE)
        target_link_options(${_TEST_NAME}_tests PRIVATE
            "LINKER:-rpath,@executable_path/python/lib")
    else ()
        set_property(TARGET ${_TEST_NAME}_tests APPEND PROPERTY BUILD_RPATH "$ORIGIN/python/lib")
    endif ()

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for the plugin host API tests"
        VERBATIM
    )
elseif (FLATPAK)
    # Same <testdir>/python home as WIN32/APPLE; symlink since /app/libpython
    # already ships in the flatpak (the test exe links libpython3.12.so from it).
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Linking Python runtime for flatpak plugin host API tests"
        VERBATIM
    )
endif()

# scripts/run_unit_tests.sh installs the tests' numpy into that runtime with this uv,
# staged where the app build tree keeps it (<exe dir>/tools/uv, see src/CMakeLists.txt).
if (ORCA_BUNDLED_UV_EXECUTABLE)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORCA_BUNDLED_UV_EXECUTABLE}"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv/${ORCA_BUNDLED_UV_FILENAME}"
        COMMENT "Copying uv for the plugin host API tests"
        VERBATIM
    )
endif()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

set(_FILAMENT_SORT_TEST_NAME filament_sort)
# Adaptation for this tree: wxWidgets is found by src/CMakeLists.txt in CONFIG mode (3.3), whose imported
# targets are directory-scoped, so wxWidgets_INCLUDE_DIRS / wxWidgets_LIBRARIES are empty here. Find it
# again (same call shapes as src/CMakeLists.txt, base component only, all FilamentSort needs).
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(wxWidgets 3.3 REQUIRED COMPONENTS base)
    include(${wxWidgets_USE_FILE})
else ()
    find_package(wxWidgets 3.3 CONFIG REQUIRED COMPONENTS base)
endif ()
# The shipped allow-list is a data contract with PresetUpdater (which deploys it) and with the
# loader under test, so the suite reads the real file instead of a copy that could drift from it.
set(_FILAMENT_ALLOW_LIST_FILE ${PROJECT_SOURCE_DIR}/resources/profiles/Snapmaker/filament/filament_allow_list.json)
file(TO_CMAKE_PATH "${_FILAMENT_ALLOW_LIST_FILE}" _FILAMENT_ALLOW_LIST_FILE)
add_executable(${_FILAMENT_SORT_TEST_NAME}_tests
    FilamentSort_test.cpp
    ${PROJECT_SOURCE_DIR}/src/slic3r/GUI/FilamentSort.cpp)
target_compile_definitions(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
    wxDEBUG_LEVEL=0
    "FILAMENT_ALLOW_LIST_FILE=R\"(${_FILAMENT_ALLOW_LIST_FILE})\"")
# Adaptation: deps_src is SYSTEM here because the vendored nlohmann/json.hpp trips -Werror
# (-Wdeprecated-literal-operator) on this tree's compiler; libslic3r's own targets reach it as a system include.
target_include_directories(${_FILAMENT_SORT_TEST_NAME}_tests SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/deps_src)
target_include_directories(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/libslic3r
    ${Boost_INCLUDE_DIR}
    ${wxWidgets_INCLUDE_DIRS})
if (WIN32 AND wxWidgets_LIB_DIR AND wxWidgets_CONFIGURATION)
    target_include_directories(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
        "${wxWidgets_LIB_DIR}/${wxWidgets_CONFIGURATION}")
endif ()
target_link_libraries(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
    Catch2::Catch2WithMain
    ${wxWidgets_LIBRARIES})
set_property(TARGET ${_FILAMENT_SORT_TEST_NAME}_tests PROPERTY FOLDER "tests")
catch_discover_tests(${_FILAMENT_SORT_TEST_NAME}_tests TEST_PREFIX "${_FILAMENT_SORT_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
