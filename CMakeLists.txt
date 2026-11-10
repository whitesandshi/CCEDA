cmake_minimum_required(VERSION 3.24)

project(CCEDA VERSION 0.2.0 LANGUAGES CXX)

if(MSVC AND CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "Supported configurations" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(CTest)

set(CCEDA_QT_ROOT
    "E:/Qt/6.8.3/msvc2022_64"
    CACHE PATH "Qt kit root")
set(OpenCASCADE_DIR
    "D:/Projects/OpenCASCADE-8.0.0/install/cmake"
    CACHE PATH "Directory containing OpenCASCADEConfig.cmake")
set(CCEDA_GMSH_EXECUTABLE
    "D:/Projects/gmsh/install/bin/gmsh.exe"
    CACHE FILEPATH "Gmsh executable used by Worker integration tests")

list(PREPEND CMAKE_PREFIX_PATH "${CCEDA_QT_ROOT}")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Concurrent)
find_package(OpenCASCADE CONFIG REQUIRED COMPONENTS
    FoundationClasses
    ModelingData
    ModelingAlgorithms
    Visualization
    ApplicationFramework
    DataExchange)
qt_standard_project_setup()

add_library(cceda_domain STATIC
    src/domain/ProjectState.cpp
    src/domain/MeshModel.cpp
    src/domain/MeshExportSettings.h
    src/domain/MeshGenerationSettings.h
    src/domain/MeshingResourceLimits.h)
target_include_directories(cceda_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(cceda_mesh_quality STATIC
    src/mesh_quality/MeshQuality.cpp
    src/mesh_quality/MeshQuality.h
    src/mesh_quality/MeshQualityColoring.cpp
    src/mesh_quality/MeshQualityColoring.h)
target_include_directories(cceda_mesh_quality
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_mesh_quality PUBLIC cceda_domain)

add_library(cceda_project_settings STATIC
    src/persistence/MeshSettingsJson.cpp
    src/persistence/MeshSettingsJson.h)
target_link_libraries(cceda_project_settings PUBLIC Qt6::Core cceda_domain)

add_library(cceda_mesh_gmsh STATIC
    src/meshing/GmshMsh41Reader.cpp)
target_include_directories(cceda_mesh_gmsh PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_mesh_gmsh PUBLIC cceda_domain)

add_library(cceda_mesh_export_gmsh STATIC
    src/meshing/GmshMsh41MeshExporter.cpp
    src/meshing/GmshMsh41MeshExporter.h)
target_include_directories(cceda_mesh_export_gmsh
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_mesh_export_gmsh
    PUBLIC cceda_application
    PRIVATE Qt6::Core)

add_library(cceda_mesh_codec STATIC
    src/meshing/MeshBinaryCodec.cpp
    src/meshing/MeshBinaryCodec.h
    src/meshing/MeshBinaryCodecInternal.h
    src/meshing/MeshBinaryReader.cpp)
target_include_directories(cceda_mesh_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_mesh_codec PUBLIC cceda_domain)

add_library(cceda_mesh_quality_json STATIC
    src/meshing/MeshQualityJson.cpp
    src/meshing/MeshQualityJson.h)
target_include_directories(cceda_mesh_quality_json
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_mesh_quality_json
    PUBLIC Qt6::Core cceda_mesh_quality)

add_library(cceda_meshing_protocol STATIC
    src/meshing/GeometryMap.cpp
    src/meshing/GeometryMap.h)
target_include_directories(cceda_meshing_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_meshing_protocol PUBLIC Qt6::Core)

add_library(cceda_meshing_worker_client STATIC
    src/meshing/GmshExecutableJobWorkspace.cpp
    src/meshing/GmshExecutableJobWorkspace.h
    src/meshing/GmshWorkerClient.cpp
    src/meshing/GmshWorkerClient.h)
target_include_directories(cceda_meshing_worker_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_meshing_worker_client
    PUBLIC Qt6::Core cceda_domain
    PRIVATE cceda_meshing_protocol)

add_library(cceda_application STATIC
    src/application/IMeshExporter.h
    src/application/IStagedFilePublisher.h
    src/application/MeshExportJobController.cpp
    src/application/MeshExportJobController.h
    src/application/MeshExportTransaction.cpp
    src/application/MeshExportTransaction.h
    src/application/MeshingJobController.cpp
    src/application/MeshingJobController.h
    src/application/WorkerMeshArtifactValidator.cpp
    src/application/WorkerMeshArtifactValidator.h)
target_include_directories(cceda_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_application
    PUBLIC Qt6::Core cceda_domain cceda_mesh_codec cceda_mesh_quality
    PRIVATE Qt6::Concurrent cceda_meshing_worker_client
        cceda_meshing_protocol cceda_mesh_quality_json)

add_library(cceda_platform_windows STATIC
    src/platform/WindowsStagedFilePublisher.cpp
    src/platform/WindowsStagedFilePublisher.h)
target_include_directories(cceda_platform_windows
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_platform_windows PUBLIC cceda_application)

add_library(cceda_geometry_occ STATIC
    src/geometry/OccGeometryRepository.cpp
    src/geometry/OccGeometryService.cpp
    src/geometry/OccGeometrySnapshotService.cpp)
target_include_directories(cceda_geometry_occ PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_geometry_occ
    PUBLIC cceda_domain
    PRIVATE Qt6::Core TKDESTEP TKXCAF TKVCAF TKCAF TKLCAF TKCDF TKBRep TKTopAlgo TKG3d TKGeomBase TKMath TKernel)
set_property(TARGET cceda_geometry_occ PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

add_library(cceda_visualization_occ STATIC
    src/visualization/OccMeshTriangulation.cpp
    src/visualization/OccMeshTriangulation.h
    src/visualization/OccViewportController.cpp)
target_include_directories(cceda_visualization_occ PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_visualization_occ
    PUBLIC cceda_geometry_occ cceda_mesh_quality TKOpenGl TKV3d TKService TKBRep)
set_property(TARGET cceda_visualization_occ PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

qt_add_executable(cceda_gmsh_worker
    src/meshing/GmshExecutableWorker.cpp)
target_link_libraries(cceda_gmsh_worker PRIVATE
    Qt6::Core
    cceda_mesh_codec
    cceda_mesh_gmsh
    cceda_mesh_quality_json
    cceda_meshing_protocol)
if(WIN32)
    target_link_libraries(cceda_gmsh_worker PRIVATE Psapi)
endif()
set_target_properties(cceda_gmsh_worker PROPERTIES
    WIN32_EXECUTABLE FALSE
    DISABLE_PRECOMPILE_HEADERS ON)

add_library(cceda_ui_widgets STATIC
    src/ui/MeshingTaskPanel.cpp
    src/ui/MeshingTaskPanelPresentation.cpp
    src/ui/MeshingTaskPanel.h
    src/ui/QualityInspectionPanel.cpp
    src/ui/QualityInspectionPanelPresentation.cpp
    src/ui/QualityInspectionPanelColoring.cpp
    src/ui/QualityInspectionPanel.h)
target_include_directories(cceda_ui_widgets
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_ui_widgets
    PUBLIC Qt6::Widgets cceda_application)
set_property(TARGET cceda_ui_widgets
    PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

qt_add_executable(CCEDA
    src/ui/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindowMeshExport.cpp
    src/ui/MainWindowMeshing.cpp
    src/ui/MainWindow.h
    src/ui/OccViewportWidget.cpp
    src/ui/OccViewportWidget.h)
target_link_libraries(CCEDA PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    cceda_ui_widgets
    cceda_domain
    cceda_geometry_occ
    cceda_mesh_export_gmsh
    cceda_platform_windows
    cceda_visualization_occ)
target_compile_definitions(CCEDA PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    CCEDA_OCC_RESOURCE_DIR=L\"${OpenCASCADE_RESOURCE_DIR}\"
    CCEDA_GMSH_EXECUTABLE_PATH=L\"${CCEDA_GMSH_EXECUTABLE}\")
add_dependencies(CCEDA cceda_gmsh_worker)
set_target_properties(CCEDA PROPERTIES
    WIN32_EXECUTABLE TRUE
    DISABLE_PRECOMPILE_HEADERS ON
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:CCEDA>"
    VS_DEBUGGER_ENVIRONMENT "PATH=${OpenCASCADE_BINARY_DIR};${CCEDA_QT_ROOT}/bin;%PATH%")

if(MSVC)
    foreach(target cceda_domain cceda_mesh_quality cceda_mesh_quality_json cceda_mesh_gmsh cceda_mesh_export_gmsh cceda_mesh_codec cceda_meshing_protocol cceda_meshing_worker_client cceda_application cceda_platform_windows cceda_geometry_occ cceda_visualization_occ cceda_ui_widgets cceda_gmsh_worker CCEDA)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    endforeach()
endif()

file(GLOB OCCT_RUNTIME_DLLS CONFIGURE_DEPENDS "${OpenCASCADE_BINARY_DIR}/*.dll")
add_custom_command(TARGET CCEDA POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OCCT_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:CCEDA>"
    COMMAND_EXPAND_LISTS
    COMMENT "Copying Open CASCADE runtime DLLs")

find_program(WINDEPLOYQT_EXECUTABLE
    NAMES windeployqt
    HINTS "${CCEDA_QT_ROOT}/bin"
    NO_DEFAULT_PATH
    REQUIRED)
add_custom_command(TARGET CCEDA POST_BUILD
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --release
            --no-translations
            --no-compiler-runtime
            "$<TARGET_FILE:CCEDA>"
    COMMENT "Deploying Qt runtime")

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT CCEDA)
set_target_properties(
    cceda_domain
    cceda_mesh_quality
    cceda_mesh_gmsh
    cceda_mesh_export_gmsh
    cceda_mesh_quality_json
    cceda_mesh_codec
    cceda_meshing_protocol
    cceda_meshing_worker_client
    cceda_application
    cceda_platform_windows
    cceda_geometry_occ
    cceda_visualization_occ
    cceda_ui_widgets
    PROPERTIES
    FOLDER "Libraries")

if(BUILD_TESTING)
    add_executable(cceda_occ_mesh_triangulation_tests
        tests/OccMeshTriangulationTests.cpp)
    target_link_libraries(cceda_occ_mesh_triangulation_tests PRIVATE
        cceda_visualization_occ)
    set_property(TARGET cceda_occ_mesh_triangulation_tests
        PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
    add_test(
        NAME visualization.occ_mesh_triangulation
        COMMAND cceda_occ_mesh_triangulation_tests)

    add_executable(cceda_viewport_widget_tests
        tests/OccViewportWidgetTests.cpp
        src/ui/OccViewportWidget.cpp
        src/ui/OccViewportWidget.h)
    target_include_directories(cceda_viewport_widget_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cceda_viewport_widget_tests PRIVATE
        Qt6::Widgets)
    add_test(
        NAME ui.viewport_widget
        COMMAND cceda_viewport_widget_tests)
    set_tests_properties(ui.viewport_widget PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CCEDA_QT_ROOT}/plugins"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${CCEDA_QT_ROOT}/bin")

    add_executable(cceda_meshing_task_panel_tests
        tests/MeshingTaskPanelTests.cpp)
    target_include_directories(cceda_meshing_task_panel_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cceda_meshing_task_panel_tests PRIVATE
        Qt6::Widgets
        cceda_ui_widgets)
    add_test(
        NAME ui.meshing_task_panel
        COMMAND cceda_meshing_task_panel_tests)
    set_tests_properties(ui.meshing_task_panel PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CCEDA_QT_ROOT}/plugins"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${CCEDA_QT_ROOT}/bin")

    add_executable(cceda_quality_inspection_panel_tests
        tests/QualityInspectionPanelTests.cpp)
    target_include_directories(cceda_quality_inspection_panel_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cceda_quality_inspection_panel_tests PRIVATE
        Qt6::Widgets
        cceda_ui_widgets)
    add_test(
        NAME ui.quality_inspection_panel
        COMMAND cceda_quality_inspection_panel_tests)
    set_tests_properties(ui.quality_inspection_panel PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CCEDA_QT_ROOT}/plugins"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${CCEDA_QT_ROOT}/bin")

    add_executable(cceda_meshing_worker_tests
        tests/MeshingWorkerTests.cpp)
    target_link_libraries(cceda_meshing_worker_tests PRIVATE
        Qt6::Core
        cceda_mesh_codec
        cceda_meshing_worker_client
        cceda_mesh_quality_json
        cceda_geometry_occ
        TKTopAlgo
        TKBRep
        TKPrim)
    set_property(TARGET cceda_meshing_worker_tests
        PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
    add_dependencies(cceda_meshing_worker_tests cceda_gmsh_worker)
    add_test(
        NAME meshing.worker_process
        COMMAND cceda_meshing_worker_tests
                $<TARGET_FILE:cceda_gmsh_worker>
                ${CCEDA_GMSH_EXECUTABLE}
                D:/Projects/OpenCASCADE-8.0.0/install/data/step/screw.step)
    set_tests_properties(meshing.worker_process PROPERTIES
        TIMEOUT 30
        LABELS "integration;meshing"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CCEDA_QT_ROOT}/bin")

    add_executable(cceda_domain_tests tests/ProjectStateTests.cpp)
    target_link_libraries(cceda_domain_tests PRIVATE cceda_domain)
    add_test(NAME domain.project_state COMMAND cceda_domain_tests)

    add_executable(cceda_mesh_model_tests tests/MeshModelTests.cpp)
    target_link_libraries(cceda_mesh_model_tests PRIVATE cceda_domain)
    add_test(NAME domain.mesh_model COMMAND cceda_mesh_model_tests)

    add_executable(cceda_mesh_export_settings_tests
        tests/MeshExportSettingsTests.cpp)
    target_link_libraries(cceda_mesh_export_settings_tests PRIVATE cceda_domain)
    add_test(NAME domain.mesh_export_settings
        COMMAND cceda_mesh_export_settings_tests)

    add_executable(cceda_mesh_exporter_contract_tests
        tests/MeshExporterContractTests.cpp)
    target_link_libraries(cceda_mesh_exporter_contract_tests PRIVATE
        cceda_application)
    add_test(NAME application.mesh_exporter_contract
        COMMAND cceda_mesh_exporter_contract_tests)

    add_executable(cceda_mesh_export_transaction_tests
        tests/MeshExportTransactionTests.cpp)
    target_link_libraries(cceda_mesh_export_transaction_tests PRIVATE
        cceda_application
        cceda_platform_windows)
    add_test(NAME application.mesh_export_transaction
        COMMAND cceda_mesh_export_transaction_tests)

    add_executable(cceda_mesh_export_job_controller_tests
        tests/MeshExportJobControllerTests.cpp)
    target_link_libraries(cceda_mesh_export_job_controller_tests PRIVATE
        Qt6::Core
        cceda_application
        cceda_platform_windows)
    add_test(NAME application.mesh_export_job_controller
        COMMAND cceda_mesh_export_job_controller_tests)

    add_executable(cceda_gmsh_msh41_mesh_exporter_tests
        tests/GmshMsh41MeshExporterTests.cpp)
    target_link_libraries(cceda_gmsh_msh41_mesh_exporter_tests PRIVATE
        Qt6::Core
        cceda_mesh_export_gmsh
        cceda_mesh_gmsh)
    add_test(NAME meshing.gmsh_msh41_mesh_exporter
        COMMAND cceda_gmsh_msh41_mesh_exporter_tests
                ${CCEDA_GMSH_EXECUTABLE})
    set_tests_properties(meshing.gmsh_msh41_mesh_exporter PROPERTIES
        TIMEOUT 30
        LABELS "integration;meshing")

    add_executable(cceda_mesh_quality_tests
        tests/MeshQualityTests.cpp)
    target_link_libraries(cceda_mesh_quality_tests PRIVATE
        cceda_mesh_quality)
    add_test(NAME mesh.quality COMMAND cceda_mesh_quality_tests)

    add_executable(cceda_mesh_settings_json_tests tests/MeshSettingsJsonTests.cpp)
    target_link_libraries(cceda_mesh_settings_json_tests PRIVATE cceda_project_settings)
    add_test(NAME persistence.mesh_settings_json COMMAND cceda_mesh_settings_json_tests)

    add_executable(cceda_mesh_quality_coloring_tests
        tests/MeshQualityColoringTests.cpp)
    target_link_libraries(cceda_mesh_quality_coloring_tests PRIVATE
        cceda_mesh_quality)
    add_test(NAME mesh.quality_coloring COMMAND cceda_mesh_quality_coloring_tests)

    add_executable(cceda_mesh_quality_json_tests
        tests/MeshQualityJsonTests.cpp)
    target_link_libraries(cceda_mesh_quality_json_tests PRIVATE
        Qt6::Core
        cceda_mesh_quality_json)
    add_test(NAME meshing.mesh_quality_json
        COMMAND cceda_mesh_quality_json_tests)

    add_executable(cceda_mesh_binary_codec_tests
        tests/MeshBinaryCodecTests.cpp)
    target_link_libraries(cceda_mesh_binary_codec_tests PRIVATE cceda_mesh_codec)
    add_test(NAME meshing.mesh_binary_codec COMMAND cceda_mesh_binary_codec_tests)

    add_executable(cceda_gmsh_msh41_reader_tests
        tests/GmshMsh41ReaderTests.cpp)
    target_link_libraries(cceda_gmsh_msh41_reader_tests PRIVATE cceda_mesh_gmsh)
    add_test(NAME meshing.gmsh_msh41_reader COMMAND cceda_gmsh_msh41_reader_tests)

    add_executable(cceda_gmsh_worker_client_tests
        tests/GmshWorkerClientTests.cpp)
    target_link_libraries(cceda_gmsh_worker_client_tests PRIVATE
        Qt6::Core
        cceda_meshing_worker_client
        cceda_geometry_occ
        TKTopAlgo
        TKBRep)
    set_property(TARGET cceda_gmsh_worker_client_tests
        PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
    add_dependencies(cceda_gmsh_worker_client_tests cceda_gmsh_worker)
    add_test(
        NAME meshing.worker_client
        COMMAND cceda_gmsh_worker_client_tests
                $<TARGET_FILE:cceda_gmsh_worker>
                ${CCEDA_GMSH_EXECUTABLE}
                D:/Projects/OpenCASCADE-8.0.0/install/data/step/screw.step)
    set_tests_properties(meshing.worker_client PROPERTIES
        TIMEOUT 70
        LABELS "integration;meshing"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CCEDA_QT_ROOT}/bin")

    add_executable(cceda_gmsh_job_workspace_tests
        tests/GmshExecutableJobWorkspaceTests.cpp)
    target_link_libraries(cceda_gmsh_job_workspace_tests PRIVATE
        Qt6::Core
        cceda_meshing_worker_client)
    add_test(
        NAME meshing.gmsh_job_workspace
        COMMAND cceda_gmsh_job_workspace_tests)

    add_executable(cceda_geometry_map_tests
        tests/GeometryMapTests.cpp)
    target_link_libraries(cceda_geometry_map_tests PRIVATE
        cceda_meshing_protocol)
    add_test(
        NAME meshing.geometry_map
        COMMAND cceda_geometry_map_tests)

    add_executable(cceda_worker_mesh_artifact_validator_tests
        tests/WorkerMeshArtifactValidatorTests.cpp)
    target_link_libraries(cceda_worker_mesh_artifact_validator_tests PRIVATE
        cceda_application
        cceda_mesh_codec)
    add_test(
        NAME application.worker_mesh_artifact_validator
        COMMAND cceda_worker_mesh_artifact_validator_tests)

    add_executable(cceda_meshing_job_controller_tests
        tests/MeshingJobControllerTests.cpp)
    target_link_libraries(cceda_meshing_job_controller_tests PRIVATE
        Qt6::Core
        cceda_application
        cceda_geometry_occ
        TKTopAlgo
        TKBRep
        TKPrim)
    set_property(TARGET cceda_meshing_job_controller_tests
        PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
    add_dependencies(cceda_meshing_job_controller_tests cceda_gmsh_worker)
    add_test(
        NAME application.meshing_job_controller
        COMMAND cceda_meshing_job_controller_tests
                $<TARGET_FILE:cceda_gmsh_worker>
                ${CCEDA_GMSH_EXECUTABLE}
                D:/Projects/OpenCASCADE-8.0.0/install/data/step/screw.step)
    set_tests_properties(application.meshing_job_controller PROPERTIES
        TIMEOUT 90
        LABELS "integration;application;meshing"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CCEDA_QT_ROOT}/bin")

    add_executable(cceda_geometry_import_tests tests/GeometryImportTests.cpp)
    target_link_libraries(cceda_geometry_import_tests PRIVATE
        cceda_geometry_occ
        TKDESTEP
        TKTopAlgo
        TKBRep
        TKG3d)
    set_property(TARGET cceda_geometry_import_tests PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
    add_test(
        NAME geometry.import_step
        COMMAND cceda_geometry_import_tests
                "D:/Projects/OpenCASCADE-8.0.0/install/data/step/screw.step")
    set_tests_properties(geometry.import_step PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OpenCASCADE_BINARY_DIR}")

    add_executable(cceda_geometry_snapshot_tests
        tests/GeometrySnapshotTests.cpp)
    target_link_libraries(cceda_geometry_snapshot_tests PRIVATE
        Qt6::Core
        cceda_geometry_occ
        TKTopAlgo
        TKBRep
        TKPrim)
    set_property(TARGET cceda_geometry_snapshot_tests PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
    add_test(
        NAME geometry.meshing_snapshot
        COMMAND cceda_geometry_snapshot_tests
                "D:/Projects/OpenCASCADE-8.0.0/install/data/step/screw.step")
    set_tests_properties(geometry.meshing_snapshot PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OpenCASCADE_BINARY_DIR}")
    set_target_properties(
        cceda_domain_tests
        cceda_mesh_model_tests
        cceda_mesh_quality_tests
        cceda_mesh_binary_codec_tests
        cceda_mesh_quality_json_tests
        cceda_viewport_widget_tests
        cceda_gmsh_msh41_reader_tests
        cceda_gmsh_msh41_mesh_exporter_tests
        cceda_mesh_export_job_controller_tests
        cceda_mesh_export_transaction_tests
        cceda_geometry_map_tests
        cceda_geometry_import_tests
        cceda_geometry_snapshot_tests
        PROPERTIES
        FOLDER "Tests")
endif()
