#
# Project configuration
#

# Added by GREG
#add_compile_definitions(GREG) #Used for debugging build
#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_BUILD_TYPE RelWithDebInfo)
#add_compile_definitions(ITERSTUDY) #Used for checking iterations
add_compile_definitions(ITERSFIX) #Used as a fix for the iterations 
add_compile_definitions(PERF_CNTR_MODE) # Enables the PAPI counter profiling

# Tell CMake where to find the PAPI library files
include_directories(/usr/local/include)
link_directories(/usr/local/lib)

add_compile_definitions(NUMA)
add_compile_definitions(NUMA_VERSION1_COMPATIBILITY)
# End of Added by GREG

cmake_minimum_required(VERSION 2.6)
project(pChase)

set (pChase_VERSION_MAJOR 0)
set (pChase_VERSION_MINOR 4)

find_package(Threads)

find_library(LIBNUMA numa)
option(USE_LIBNUMA "Build against NUMA libraries" ON) 

include_directories(lib)
add_subdirectory(lib/AsmJit)


#
# Code compilation
#

add_library(experiment src/experiment.h src/experiment.cpp)

add_library(thread src/thread.h src/thread.cpp)

add_library(lock src/lock.h src/lock.cpp)

add_library(output src/output.h src/output.cpp)

add_library(run src/run.h src/run.cpp)
target_link_libraries(run lock thread)

#Added by GREG
target_link_libraries(run papi)
#End of added by GREG

add_library(spinbarrier src/spinbarrier.h src/spinbarrier.cpp)

add_library(timer src/timer.h src/timer.cpp)

add_executable (chase src/main.cpp)
target_link_libraries(chase run timer output experiment spinbarrier)
target_link_libraries(chase ${CMAKE_THREAD_LIBS_INIT})
if (USE_LIBNUMA)
	if(LIBNUMA)
		target_link_libraries(chase ${LIBNUMA})
	else ()
		message(STATUS "WARNING: libnuma not found, not compiling against it")
	endif ()
endif ()
target_link_libraries(chase AsmJit)
