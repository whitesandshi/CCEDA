# CCEDA Agent Context

## Product

CCEDA is a solver-neutral geometry and mesh preprocessor for Windows. Read docs/spec/README.md before changing product behavior. The current implementation slice imports one STEP file, shows the XCAF hierarchy, exposes geometry properties, and renders OCCT geometry in a Qt workbench.

## Architecture Boundaries

- src/domain owns plain C++ state and contracts. It must not depend on Qt, OCCT, or Gmsh.
- src/geometry implements geometry contracts with OCCT. Keep OCCT types inside this adapter boundary.
- src/visualization implements the viewport port with OCCT AIS/V3d.
- src/ui owns Qt widgets and application composition. UI behavior should use domain and port types rather than TopoDS_Shape or AIS_InteractiveObject.
- Gmsh must run behind the versioned worker protocol in docs/spec/04-meshing-protocol.md. Do not couple Gmsh objects to UI or project-domain types.
- A successful mesh belongs to one exact geometry revision. Geometry changes must make prior mesh results stale.

## Local Dependencies

- Visual Studio 2022: D:/Visual Studio/2022/Community
- Qt 6.8.3: E:/Qt/6.8.3/msvc2022_64
- OCCT 8.0.0: D:/Projects/OpenCASCADE-8.0.0/install
- Gmsh executable: D:/Projects/gmsh/install/bin/gmsh.exe
- The local Gmsh build currently has ENABLE_BUILD_LIB=OFF; a linkable SDK is not available.

## Build And Verify

    cmake --preset vs2022-x64
    cmake --build --preset release
    ctest --preset release

The generated solution is build-vs2022/CCEDA.sln. Only Release | x64 is supported because the installed OCCT libraries are Release binaries.

