# CMakeLists.txt  –  top of your repo
cmake_minimum_required(VERSION 3.24)    # CMake ≥3.24 recommended on macOS
project(pscal C)                         # pure‑C project

# ---- options you used everywhere ------------------------------------------
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall)

# Put all executables in <repo>/bin  (matches your Makefile)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ---- gather sources -------------------------------------------------------
file(GLOB SRC_FILES CONFIGURE_DEPENDS src/*.c src/*.h)

# ---- Release‑style build : pscal ------------------------------------------
add_executable(pscal  ${SRC_FILES})
target_link_libraries(pscal PRIVATE m curl)   # -lm -lcurl

# ---- Address‑Sanitized debug build : dscal -------------------------------
add_executable(dscal  ${SRC_FILES})

target_compile_definitions(dscal PRIVATE DEBUG)
target_compile_options( dscal PRIVATE -fsanitize=address -g)
target_link_options(    dscal PRIVATE -fsanitize=address)
target_link_libraries(  dscal PRIVATE m curl)

# ---- “no‑exit” build : hscal (formerly nscal) -----------------------------
add_executable(hscal  ${SRC_FILES})

target_compile_definitions(hscal PRIVATE SUPPRESS_EXIT)
target_compile_options( hscal PRIVATE -g)
target_link_libraries(  hscal PRIVATE m curl)

# ---- optional install rule (matches `make install`) -----------------------
include(GNUInstallDirs)
install(TARGETS pscal dscal RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# --------------------------------------------------------------------------

