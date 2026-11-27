project(UHE_EDITOR)


file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

foreach(_FILE ${EDITOR_SOURCES})
  
    get_filename_component(_DIR "${_FILE}" DIRECTORY)
    

    file(RELATIVE_PATH _REL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" "${_DIR}")
    
  
    string(REPLACE "/" "\\" _GROUP "${_REL_DIR}")
    
   
    source_group("${_GROUP}" FILES "${_FILE}")
    
  
endforeach()

add_executable(UHE_EDITOR ${EDITOR_SOURCES})

target_include_directories(UHE_EDITOR PRIVATE
    "src"
    "${CMAKE_SOURCE_DIR}/UHE/src"
    "${CMAKE_SOURCE_DIR}/UHE/vendor"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/glm"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/yaml-cpp/include"
)


# uhe_texcook: File > Export Game encodes a cooked model's images to KTX2.
target_link_libraries(UHE_EDITOR PRIVATE UHE uhe_texcook)

if(WIN32)
    target_compile_definitions(UHE_EDITOR PRIVATE UHE_PLATFORM_WINDOWS UHE_DYNAMIC_LINK)
elseif(UNIX)
    target_compile_definitions(UHE_EDITOR PRIVATE UHE_PLATFORM_LINUX)
endif()

target_compile_definitions(UHE_EDITOR PRIVATE
    UHE_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    ENTT_API_IMPORT
)

if(UHE_PROFILE)
    target_compile_definitions(UHE_EDITOR PRIVATE TRACY_ENABLE=1 TRACY_IMPORTS=1)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_custom_command(TARGET UHE_EDITOR POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:UHE_EDITOR>/assets"
        COMMENT "Copying UHE_EDITOR assets to output directory"
    )
else()
    message(WARNING "Assets directory not found at ${CMAKE_CURRENT_SOURCE_DIR}/assets - skipping copy!")
endif()

if(MSVC)
    target_compile_options(UHE_EDITOR PRIVATE /utf-8)
endif()
