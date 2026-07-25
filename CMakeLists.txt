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
    src/domain/MeshModel.cpp)
target_include_directories(cceda_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(cceda_mesh_gmsh STATIC
    src/meshing/GmshMsh41Reader.cpp)
target_include_directories(cceda_mesh_gmsh PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_mesh_gmsh PUBLIC cceda_domain)

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
    src/application/MeshingJobController.cpp
    src/application/MeshingJobController.h)
target_include_directories(cceda_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_application
    PUBLIC Qt6::Core cceda_domain
    PRIVATE Qt6::Concurrent cceda_meshing_worker_client)

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
    src/visualization/OccViewportController.cpp)
target_include_directories(cceda_visualization_occ PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cceda_visualization_occ
    PUBLIC cceda_geometry_occ TKOpenGl TKV3d TKService TKBRep)
set_property(TARGET cceda_visualization_occ PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

qt_add_executable(cceda_gmsh_worker
    src/meshing/GmshExecutableWorker.cpp)
target_link_libraries(cceda_gmsh_worker PRIVATE Qt6::Core cceda_meshing_protocol)
set_target_properties(cceda_gmsh_worker PROPERTIES
    WIN32_EXECUTABLE FALSE
    DISABLE_PRECOMPILE_HEADERS ON)

qt_add_executable(CCEDA
    src/ui/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/OccViewportWidget.cpp
    src/ui/OccViewportWidget.h)
target_link_libraries(CCEDA PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    cceda_domain
    cceda_geometry_occ
    cceda_visualization_occ)
target_compile_definitions(CCEDA PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    CCEDA_OCC_RESOURCE_DIR=L\"${OpenCASCADE_RESOURCE_DIR}\")
set_target_properties(CCEDA PROPERTIES
    WIN32_EXECUTABLE TRUE
    DISABLE_PRECOMPILE_HEADERS ON
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:CCEDA>"
    VS_DEBUGGER_ENVIRONMENT "PATH=${OpenCASCADE_BINARY_DIR};${CCEDA_QT_ROOT}/bin;%PATH%")

if(MSVC)
    foreach(target cceda_domain cceda_mesh_gmsh cceda_meshing_protocol cceda_meshing_worker_client cceda_application cceda_geometry_occ cceda_visualization_occ cceda_gmsh_worker CCEDA)
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
    cceda_mesh_gmsh
    cceda_meshing_protocol
    cceda_meshing_worker_client
    cceda_application
    cceda_geometry_occ
    cceda_visualization_occ
    PROPERTIES
    FOLDER "Libraries")

if(BUILD_TESTING)
    add_executable(cceda_meshing_worker_tests
        tests/MeshingWorkerTests.cpp)
    target_link_libraries(cceda_meshing_worker_tests PRIVATE
        Qt6::Core
        cceda_mesh_gmsh
        cceda_meshing_worker_client
        cceda_geometry_occ
        TKTopAlgo
        TKBRep)
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

    add_executable(cceda_meshing_job_controller_tests
        tests/MeshingJobControllerTests.cpp)
    target_link_libraries(cceda_meshing_job_controller_tests PRIVATE
        Qt6::Core
        cceda_application)
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
        TKBRep)
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
        cceda_gmsh_msh41_reader_tests
        cceda_geometry_map_tests
        cceda_geometry_import_tests
        cceda_geometry_snapshot_tests
        PROPERTIES
        FOLDER "Tests")
endif()
