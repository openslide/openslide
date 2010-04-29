###################################################################
#
#  Copyright (c) 2006 Frederic Heem, <frederic.heem@telsey.it>
#  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# * Redistributions of source code must retain the above copyright
#   notice, this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in
#   the documentation and/or other materials provided with the
#   distribution.
#
# * Neither the name of the Telsey nor the names of its
#   contributors may be used to endorse or promote products derived
#   from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
# FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
# COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
# ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
###################################################################
# - Check the bit formats.
# CHECK_BIT_FORMAT(FORMAT VARIABLE)
# - macro which checks if the specified bit format is accepted
#  FORMAT - the format, e.g ll, L, q or I64 (for 64 bit)
#  VARIABLE - variable to store the format if it is a valdid format
#
# Example of use in a CMakeLists.txt for 64 bit tests
#
# include(Check64BitFormat)
#
# check_bit_format(64 ll FORMAT_64BIT)
# check_bit_format(64 L FORMAT_64BIT)
# check_bit_format(64 q FORMAT_64BIT)
# check_bit_format(64 I64 FORMAT_64BIT)
#
# if(NOT FORMAT_64BIT)
#   message(FATAL " 64 bit format missing")
# endif(NOT FORMAT_64BIT)
#
# set(PRIX64 "${FORMAT_64BIT}X")
# set(PRIx64 "${FORMAT_64BIT}x")
# set(PRId64 "${FORMAT_64BIT}d")
# set(PRIo64 "${FORMAT_64BIT}o")
# set(PRIu64 "${FORMAT_64BIT}u")

MACRO(CHECK_BIT_FORMAT CHECK_BITS FORMAT VARIABLE)
  IF(NOT ${VARIABLE})
    SET(FORMAT \"${FORMAT}\")
    CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/cmake/CheckBitFormat.c.in"
      "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/CheckBitFormat.c" IMMEDIATE @ONLY)

    TRY_RUN(RUN_RESULT_VAR COMPILE_RESULT_VAR
      ${CMAKE_BINARY_DIR}
      ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/CheckBitFormat.c
	  COMPILE_DEFINITIONS 
	  -DCHECK_BITS=${CHECK_BITS}
	  -DBIT_FORMAT="${FORMAT}"
      OUTPUT_VARIABLE OUTPUT)
   
    IF(${RUN_RESULT_VAR} STREQUAL "0")
      SET(${VARIABLE} \"${FORMAT}\" CACHE INTERNAL "Have format ${FORMAT}")
      MESSAGE(STATUS "Looking for ${CHECK_BITS} bit format ${FORMAT} - found")
      FILE(APPEND ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeOutput.log
        "Determining if the format ${FORMAT} runs passed with the following output:\n"
        "${OUTPUT}\n\n")
      SET(${VARIABLE} \"${FORMAT}\")
    ELSE(${RUN_RESULT_VAR} STREQUAL "0")
      MESSAGE(STATUS "Looking for ${CHECK_BITS} bit format ${FORMAT} - not found")
      SET(${VARIABLE} "" CACHE INTERNAL "Have format ${FORMAT}")
      FILE(APPEND ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeError.log
        "Determining if the format ${FORMAT} runs with the following output:\n"
        "${OUTPUT}\n\n")
    ENDIF(${RUN_RESULT_VAR} STREQUAL "0")
  ENDIF(NOT ${VARIABLE})
ENDMACRO(CHECK_BIT_FORMAT)
