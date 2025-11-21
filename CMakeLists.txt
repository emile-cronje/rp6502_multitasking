cmake_minimum_required(VERSION 3.13)
project(rp6502_os_polled C ASM)

# ------------------------------------------------------------------
# 1. Configurable settings
# ------------------------------------------------------------------
set(CC65_TARGET "rp6502"   CACHE STRING "cc65 target (e.g. none, rp6502)")
set(OS_SRC_DIR "" CACHE PATH "Dir containing main.s, os.c, main.c, os.h")

set(RP6502_PY "${CMAKE_SOURCE_DIR}/tools/rp6502.py" CACHE FILEPATH "Path to rp6502.py")
set(RP6502_CFG "${CMAKE_SOURCE_DIR}/.rp6502" CACHE FILEPATH "RP6502 config file")

# ------------------------------------------------------------------
# 2. Locate sources
# ------------------------------------------------------------------
if(NOT OS_SRC_DIR OR NOT EXISTS "${OS_SRC_DIR}/main.s")
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/main.s")
    set(OS_SRC_DIR "${CMAKE_SOURCE_DIR}/src")
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/main.s")
    set(OS_SRC_DIR "${CMAKE_SOURCE_DIR}")
  else()
    message(FATAL_ERROR "Could not find main.s. Set -DOS_SRC_DIR=/full/path/to/your/src")
  endif()
endif()
message(STATUS "Using OS_SRC_DIR = ${OS_SRC_DIR}")

# ------------------------------------------------------------------
# 3. Output directories and tools
# ------------------------------------------------------------------
set(BIN_DIR ${CMAKE_BINARY_DIR})
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${BIN_DIR} ${GEN_DIR})

find_program(CC65_COMPILER cc65 HINTS ${CC65_HOME}/bin REQUIRED)
find_program(CA65_ASSEMBLER ca65 HINTS ${CC65_HOME}/bin REQUIRED)
find_program(CL65_DRIVER   cl65 HINTS ${CC65_HOME}/bin REQUIRED)
find_program(LD65_LINKER    ld65 HINTS ${CC65_HOME}/bin REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ------------------------------------------------------------------
# 4. Inputs
# ------------------------------------------------------------------
set(C_SOURCES
  ${OS_SRC_DIR}/main.c
  ${OS_SRC_DIR}/scheduler.c
  ${OS_SRC_DIR}/ringq.c  
)
set(ASM_SOURCES
  ${OS_SRC_DIR}/ctxswitch.s
  ${OS_SRC_DIR}/main.s  
  ${OS_SRC_DIR}/vectors.s
)

# If a C source will generate a .s (and then a .o) with the same basename
# as an existing handwritten assembly file, we must avoid assembling both
# into the same ${GEN_DIR}/${name}.o because CMake will create duplicate
# custom rules for the identical output path.
#
# Build a list of basenames produced by the C sources and filter out any
# ASM_SOURCES whose basename collides with one of those. This prevents the
# "Attempt to add a custom rule to output ... which already has a custom
# rule" CMake error when both main.c -> main.s -> main.o and a handwritten
# main.s would otherwise produce ${GEN_DIR}/main.o twice.
set(_C_BASES "")
foreach(_CSRC IN LISTS C_SOURCES)
  get_filename_component(_CB ${_CSRC} NAME_WE)
  list(APPEND _C_BASES ${_CB})
endforeach()

set(_FILTERED_ASM "")
foreach(_ASRC IN LISTS ASM_SOURCES)
  get_filename_component(_AB ${_ASRC} NAME_WE)
  list(FIND _C_BASES ${_AB} _idx)
  if(_idx EQUAL -1)
    list(APPEND _FILTERED_ASM ${_ASRC})
  else()
    message(STATUS "Skipping assembly source ${_ASRC} because a C source with the same base exists (${_AB})")
  endif()
endforeach()
set(ASM_SOURCES ${_FILTERED_ASM})

# prevent host build
set_source_files_properties(${C_SOURCES} ${ASM_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

# ------------------------------------------------------------------
# 5. Compile chain: C -> .s -> .o + os.asm -> os.o
# ------------------------------------------------------------------
set(GEN_S "")
# set(CC65_INCLUDES
#   -I${OS_SRC_DIR}
#   -I${CMAKE_SOURCE_DIR}/include
# )

# --- CORRECT ---
set(CC65_INCLUDES
  -I${OS_SRC_DIR}
  -I${CMAKE_SOURCE_DIR}/include
  -I${CC65_SHARE}/include          # <-- ADD THIS LINE
)

foreach(CSRC IN LISTS C_SOURCES)
  get_filename_component(BASENAME ${CSRC} NAME_WE)
  if(BASENAME STREQUAL "os")
    set(OUTBASE "os_c")
  else()
    set(OUTBASE ${BASENAME})
  endif()
  add_custom_command(
    OUTPUT ${GEN_DIR}/${OUTBASE}.s
    COMMAND ${CMAKE_COMMAND} -E env CC65_HOME=${CC65_HOME}   # <-- SET ENV VAR
            ${CC65_COMPILER} -O -t ${CC65_TARGET} ${CC65_INCLUDES} ${CSRC} -o ${GEN_DIR}/${OUTBASE}.s
    DEPENDS ${CSRC}
    COMMENT "cc65 ${CSRC} -> ${OUTBASE}.s"
  )  
  list(APPEND GEN_S ${GEN_DIR}/${OUTBASE}.s)
endforeach()

set(OBJS "")
foreach(SSRC IN LISTS GEN_S)
  get_filename_component(SBASE ${SSRC} NAME_WE)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${SBASE}.o
    COMMAND ${CA65_ASSEMBLER} ${SSRC} -o ${GEN_DIR}/${SBASE}.o
    DEPENDS ${SSRC}
    COMMENT "ca65 ${SBASE}.s -> ${SBASE}.o"
  )
  list(APPEND OBJS ${GEN_DIR}/${SBASE}.o)
endforeach()

# Assemble all handwritten assembly sources (.s or .asm)
foreach(ASRC IN LISTS ASM_SOURCES)
  get_filename_component(ABASE ${ASRC} NAME_WE)
  add_custom_command(
    OUTPUT ${GEN_DIR}/${ABASE}.o
    COMMAND ${CA65_ASSEMBLER} ${ASRC} -o ${GEN_DIR}/${ABASE}.o
    DEPENDS ${ASRC}
    COMMENT "ca65 ${ASRC} -> ${ABASE}.o"
  )
  list(APPEND OBJS ${GEN_DIR}/${ABASE}.o)
endforeach()

# ------------------------------------------------------------------
# 6. Link to raw binary (NO extension)
# ------------------------------------------------------------------
set(OUT_RAW ${BIN_DIR}/rp6502_os)
set(OUT_LBL ${BIN_DIR}/rp6502_os.lbl)

add_custom_command(
  OUTPUT ${OUT_RAW}
  COMMAND ${CMAKE_COMMAND} -E env CC65_HOME=${CC65_HOME}   # <-- SET ENV VAR
          ${CL65_DRIVER}                      # Run cl65
          -t ${CC65_TARGET}                   # Tell cl65 the target
          -C ${CMAKE_SOURCE_DIR}/rp6502.cfg   # Pass linker config
          -o ${OUT_RAW}                       # Output file
          -Wl,-Ln,${OUT_LBL}                  # Pass -Ln to linker
          ${OBJS}                           # Your project's object files
  DEPENDS ${OBJS}
  COMMENT "cl65 link -> ${OUT_RAW}"
)
add_custom_target(rom_raw ALL DEPENDS ${OUT_RAW})

# ------------------------------------------------------------------
# 7. Pack raw -> .rp6502 using rp6502.py create
# ------------------------------------------------------------------
set(OUT_RP ${BIN_DIR}/rp6502_os.rp6502)

add_custom_command(
  OUTPUT ${OUT_RP}
  COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1
          ${Python3_EXECUTABLE} ${RP6502_PY}
          -a 0x0200           # DATA / load address
          -n 0x0000          # NMI vector (set to 0 if unused)
          -r 0x0200           # RESET / entry address
          -o rp6502_os.rp6502
          -c ${RP6502_CFG}
          create rp6502_os   # <-- the raw file (no extension)
  WORKING_DIRECTORY ${BIN_DIR}
  DEPENDS ${OUT_RAW} ${RP6502_PY} ${RP6502_CFG}
  BYPRODUCTS ${OUT_RP}
  COMMENT "rp6502.py create -> ${OUT_RP}"
  VERBATIM
)
add_custom_target(rom ALL DEPENDS ${OUT_RP})

# ------------------------------------------------------------------
# 8. VS Code CMake launch integration
# ------------------------------------------------------------------
add_executable(rp6502_launch IMPORTED GLOBAL)
set_target_properties(rp6502_launch PROPERTIES
  IMPORTED_LOCATION "${OUT_RP}"
)

get_target_property(LINKER_LANGUAGE rp6502_launch LINKER_LANGUAGE)

message(STATUS "Raw binary : ${OUT_RAW}")
message(STATUS "ROM image  : ${OUT_RP}")
message(STATUS "Select 'rp6502_launch' as CMake launch target in VS Code.")