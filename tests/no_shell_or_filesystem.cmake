# Fails when OpenScope's own sources (SOURCE_DIR) include a header or call a function that runs a
# program or opens files: no SCPI command may execute a shell or touch the filesystem (spec #1,
# story 84). Standard streams (std::cout, std::cerr) are allowed.
#
#   cmake -DSOURCE_DIR=<src> -P no_shell_or_filesystem.cmake

file(GLOB_RECURSE sources "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.hpp")
if(NOT sources)
  message(FATAL_ERROR "no sources under '${SOURCE_DIR}'")
endif()

set(forbidden
  "#include[ \t]*<(fstream|filesystem|cstdio|stdio\\.h|unistd\\.h|spawn\\.h|dlfcn\\.h|fcntl\\.h)>"
  "(^|[^_a-zA-Z0-9.>])(system|popen|fopen|freopen|open|execl|execle|execlp|execv|execve|execvp|fork|vfork|dlopen)[ \t]*\\(")

foreach(source IN LISTS sources)
  file(READ "${source}" text)
  foreach(pattern IN LISTS forbidden)
    string(REGEX MATCH "${pattern}" found "${text}")
    if(found)
      message(SEND_ERROR "${source}: '${found}' may run a program or open a file")
    endif()
  endforeach()
endforeach()
