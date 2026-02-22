function(set_icon TARGET path_to_icon)
    if (NOT WIN32)
        message(WARNING "set_icon currently supports only Windows platform :(")
    else (WIN32)
        # Name of icon
        get_filename_component(icon-name ${path_to_icon} NAME_WE)
        # Extension of icon
        get_filename_component(icon-ext ${path_to_icon} EXT)

        set(current-output-dir ${CMAKE_BINARY_DIR}/res)
        file(MAKE_DIRECTORY ${current-output-dir})
        set(current-output-path ${current-output-dir}/icon.rc)

        # Convert image to icon
        if (NOT icon-ext STREQUAL ".ico")
            find_program(image_magick magick)
            set(icon-output-path ${CMAKE_BINARY_DIR}/res/${icon-name}.ico)
            add_custom_target(icon
                    COMMAND ${image_magick} ${CMAKE_SOURCE_DIR}/${path_to_icon} -resize 48x48 ${icon-output-path}
                    DEPENDS ${path_to_icon}
                    VERBATIM)
            add_dependencies(${TARGET} icon)
            set(path_to_icon ${icon-output-path})
        else ()
            set(path_to_icon ${CMAKE_SOURCE_DIR}/${path_to_icon})
        endif ()

        # Write .res file
        file(WRITE ${current-output-path} "IDR_MAINFRAME ICON \"${path_to_icon}\"\nIDI_ICON1 ICON DISCARDABLE \"${path_to_icon}\"")
        set_source_files_properties(${current-output-path} PROPERTIES GENERATED TRUE)
        # Add .res file to sources of TARGET
        target_sources(${TARGET} PRIVATE ${current-output-path})
    endif ()
endfunction(set_icon)