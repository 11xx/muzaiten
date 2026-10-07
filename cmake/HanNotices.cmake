# Source and binary distributions carry the same scoped attributions and copies.
function(muzaiten_install_han_notices source_root python)
    file(GLOB notice_tables CONFIGURE_DEPENDS
        "${source_root}/src/search/fold/tables/han/*.tsv"
        "${source_root}/src/search/fold/unihan/*.tsv")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${source_root}/tools/fold-han-source-terms.json"
        "${source_root}/tools/fold-han-notice.py"
        ${notice_tables})
    execute_process(
        COMMAND "${python}" "${source_root}/tools/fold-han-notice.py" assets
        RESULT_VARIABLE notice_result
        OUTPUT_VARIABLE notice_assets
        ERROR_VARIABLE notice_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT notice_result EQUAL 0)
        message(FATAL_ERROR "Han source notices: ${notice_error}")
    endif()
    string(REPLACE "\n" ";" notice_assets "${notice_assets}")
    set(notice_destination "${CMAKE_INSTALL_DATAROOTDIR}/licenses/muzaiten")
    install(FILES "${source_root}/UNLICENSE"
        DESTINATION "${notice_destination}" COMPONENT HanNotices)
    foreach(asset IN LISTS notice_assets)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${source_root}/src/search/fold/tables/han/${asset}")
        get_filename_component(asset_directory "${asset}" DIRECTORY)
        install(FILES "${source_root}/src/search/fold/tables/han/${asset}"
            DESTINATION "${notice_destination}/${asset_directory}" COMPONENT HanNotices)
    endforeach()
endfunction()
