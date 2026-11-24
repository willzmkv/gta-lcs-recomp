# Minimal header-only Boost package for the vendored Boost subset in third_party/boost.
# Only the headers Dynarmic needs (boost/icl, boost/variant and their dependencies) are
# shipped; see third_party/boost/README.md.

get_filename_component(_lcs_boost_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(Boost_FOUND TRUE)
set(Boost_VERSION 1.92.0)
set(Boost_VERSION_STRING 1.92.0)
set(Boost_MAJOR_VERSION 1)
set(Boost_MINOR_VERSION 92)
set(Boost_SUBMINOR_VERSION 0)
set(Boost_INCLUDE_DIR "${_lcs_boost_root}/include")
set(Boost_INCLUDE_DIRS "${_lcs_boost_root}/include")

if(NOT TARGET Boost::headers)
    add_library(Boost::headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(Boost::headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_lcs_boost_root}/include"
        INTERFACE_COMPILE_DEFINITIONS "BOOST_ALL_NO_LIB")
endif()
if(NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED GLOBAL)
    set_target_properties(Boost::boost PROPERTIES INTERFACE_LINK_LIBRARIES Boost::headers)
endif()

unset(_lcs_boost_root)
