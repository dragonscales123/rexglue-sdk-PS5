# cmake -P rex_ps5_isolate_vma.cmake -DARCHIVE=<librexruntime.a> -DNM=<llvm-nm>
#        -DAR=<llvm-ar> -DOBJCOPY=<llvm-objcopy> -DWORK=<dir>
#
# PS5 only. On the desktop the runtime is a shared library, so its own copy of
# the Vulkan Memory Allocator (src/ui/vulkan/vulkan_mem_alloc.cpp, VMA 3.3) stays
# inside it. A PS5 title links everything statically, and a title's renderer
# may bring another VMA (re:Blue's plume has 3.2): the two would collide, and
# picking one would hand code built against one version the other's
# structures. So every symbol that VMA's implementation unit defines is
# renamed rexsdk_<name> throughout the runtime archive, definitions and uses
# alike, and the SDK keeps its VMA to itself.

foreach(var ARCHIVE NM AR OBJCOPY WORK)
    if(NOT ${var})
        message(FATAL_ERROR "rex_ps5_isolate_vma: ${var} is not set")
    endif()
endforeach()

set(member vulkan_mem_alloc.cpp.o)
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${AR}" t "${ARCHIVE}" OUTPUT_VARIABLE members RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "rex_ps5_isolate_vma: cannot list ${ARCHIVE}")
endif()
string(FIND "${members}" "${member}" found)
if(found EQUAL -1)
    return() # no Vulkan in this build
endif()
execute_process(COMMAND "${AR}" x "${ARCHIVE}" "${member}" WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "rex_ps5_isolate_vma: cannot extract ${member}")
endif()
execute_process(COMMAND "${NM}" --defined-only -g "${WORK}/${member}"
    OUTPUT_VARIABLE symbols RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "rex_ps5_isolate_vma: cannot read the symbols of ${member}")
endif()

string(REPLACE "\n" ";" lines "${symbols}")
set(map "")
set(count 0)
foreach(line IN LISTS lines)
    # "<address> <type> <name>"
    if(line MATCHES "^[0-9a-fA-F]+ [A-Za-z] (.+)$")
        set(name "${CMAKE_MATCH_1}")
        string(TOLOWER "${name}" lower)
        if(lower MATCHES "vma" AND NOT name MATCHES "^rexsdk_")
            string(APPEND map "${name} rexsdk_${name}\n")
            math(EXPR count "${count} + 1")
        endif()
    endif()
endforeach()
if(count EQUAL 0)
    return() # already renamed
endif()
file(WRITE "${WORK}/redefine.txt" "${map}")
execute_process(COMMAND "${OBJCOPY}" "--redefine-syms=${WORK}/redefine.txt" "${ARCHIVE}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "rex_ps5_isolate_vma: renaming in ${ARCHIVE} failed")
endif()
message(STATUS "rex_ps5_isolate_vma: ${count} VMA symbols kept private to ${ARCHIVE}")
