Corresponding build sources for the Windows x64 executable in this release.
Local full Canary base: 393187e7dbefe3b380f572a9d1f37decfac1c716.
Includes local tracked source/build changes and the sector protocol header.
game.cpp uses the base version, before the later online-player logging fix.
Live configuration, keys, data and databases are deliberately excluded.

Build with CMake, MSVC x64 and vcpkg dependencies from vcpkg.json.
Example after installing the Visual Studio C++ build tools and setting VCPKG_ROOT:
cmake -S . -B build -A x64 -DCANARY_BUILD_TESTS=OFF -DTOGGLE_BIN_FOLDER=ON
cmake --build build --config RelWithDebInfo
The original build used MSVC 19.51 / Visual Studio 18 Insiders, Ninja,
RelWithDebInfo, x64-windows dynamic dependencies, unity/PCH and IPO disabled.
Dependency package versions are recorded in VCPKG-PACKAGE-VERSIONS.txt.
Compiler/dependency differences can change the resulting binary hash.
