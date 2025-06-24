# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "AuthoLASTVLADIO_autogen"
  "CMakeFiles\\AuthoLASTVLADIO_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\AuthoLASTVLADIO_autogen.dir\\ParseCache.txt"
  )
endif()
