function(add_muzaiten_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ../src
        ${CMAKE_CURRENT_BINARY_DIR}/../generated
    )
    target_link_libraries(${name} PRIVATE
        muzaiten_core
        muzaiten_ui_support
        Qt6::Core
        Qt6::Gui
        Qt6::Test
    )
    target_compile_features(${name} PRIVATE cxx_std_26)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_muzaiten_test(test_rating test_rating.cpp)
set_tests_properties(test_rating PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_track_menu_sections
    test_track_menu_sections.cpp
)
target_include_directories(test_track_menu_sections PRIVATE ../src)
target_link_libraries(test_track_menu_sections PRIVATE muzaiten_ui_support Qt6::Core Qt6::Widgets Qt6::Test)
target_compile_features(test_track_menu_sections PRIVATE cxx_std_26)
add_test(NAME test_track_menu_sections COMMAND test_track_menu_sections)
set_tests_properties(test_track_menu_sections PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_track_info_settings
    test_track_info_settings.cpp
)
target_include_directories(test_track_info_settings PRIVATE ../src)
target_link_libraries(test_track_info_settings PRIVATE muzaiten_core muzaiten_ui_support Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_track_info_settings PRIVATE cxx_std_26)
add_test(NAME test_track_info_settings COMMAND test_track_info_settings)
set_tests_properties(test_track_info_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_metadata_blob
    test_metadata_blob.cpp
)
target_include_directories(test_metadata_blob PRIVATE ../src)
target_link_libraries(test_metadata_blob PRIVATE muzaiten_core Qt6::Core Qt6::Test)
target_compile_features(test_metadata_blob PRIVATE cxx_std_26)
add_test(NAME test_metadata_blob COMMAND test_metadata_blob)

add_executable(test_sql_util
    test_sql_util.cpp
)
target_include_directories(test_sql_util PRIVATE ../src)
target_link_libraries(test_sql_util PRIVATE muzaiten_db Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_sql_util PRIVATE cxx_std_26)
add_test(NAME test_sql_util COMMAND test_sql_util)

add_executable(test_schema
    test_schema.cpp
)
target_include_directories(test_schema PRIVATE ../src)
target_link_libraries(test_schema PRIVATE muzaiten_db Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_schema PRIVATE cxx_std_26)
add_test(NAME test_schema COMMAND test_schema)

# Standalone CLI search path (build/load/refresh the cache, query the engine).
add_executable(test_search_cli
    test_search_cli.cpp
)
target_include_directories(test_search_cli PRIVATE ../src)
target_link_libraries(test_search_cli PRIVATE
    muzaiten_cli
    muzaiten_reco
    muzaiten_scrobble
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)
target_compile_features(test_search_cli PRIVATE cxx_std_26)
add_dependencies(test_search_cli muzaitenctl)
add_test(NAME test_search_cli COMMAND test_search_cli)

add_executable(test_playlist_database
    test_playlist_database.cpp
)
target_include_directories(test_playlist_database PRIVATE ../src)
target_link_libraries(test_playlist_database PRIVATE muzaiten_db Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_playlist_database PRIVATE cxx_std_26)
add_test(NAME test_playlist_database COMMAND test_playlist_database)
set_tests_properties(test_playlist_database PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_listen_history
    test_listen_history.cpp
)
target_include_directories(test_listen_history PRIVATE ../src)
target_link_libraries(test_listen_history PRIVATE muzaiten_scrobble Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_listen_history PRIVATE cxx_std_26)
add_test(NAME test_listen_history COMMAND test_listen_history)
set_tests_properties(test_listen_history PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_semantic_search_data
    test_semantic_search_data.cpp
)
target_include_directories(test_semantic_search_data PRIVATE ../src)
target_link_libraries(test_semantic_search_data PRIVATE muzaiten_core muzaiten_ui_support Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_semantic_search_data PRIVATE cxx_std_26)
add_test(NAME test_semantic_search_data COMMAND test_semantic_search_data)
set_tests_properties(test_semantic_search_data PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_decode_gate
    test_decode_gate.cpp
)
target_include_directories(test_decode_gate PRIVATE ../src)
target_link_libraries(test_decode_gate PRIVATE muzaiten_core Qt6::Core Qt6::Test)
target_compile_features(test_decode_gate PRIVATE cxx_std_26)
add_test(NAME test_decode_gate COMMAND test_decode_gate)
set_tests_properties(test_decode_gate PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_staging_buffer
    test_staging_buffer.cpp
)
target_include_directories(test_staging_buffer PRIVATE ../src)
target_link_libraries(test_staging_buffer PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_staging_buffer PRIVATE cxx_std_26)
add_test(NAME test_staging_buffer COMMAND test_staging_buffer)
set_tests_properties(test_staging_buffer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_query_vector_cache
    test_query_vector_cache.cpp
)
target_include_directories(test_query_vector_cache PRIVATE ../src)
target_link_libraries(test_query_vector_cache PRIVATE muzaiten_features Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_query_vector_cache PRIVATE cxx_std_26)
add_test(NAME test_query_vector_cache COMMAND test_query_vector_cache)
set_tests_properties(test_query_vector_cache PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_feature_store
    test_feature_store.cpp
)
target_include_directories(test_feature_store PRIVATE ../src)
target_link_libraries(test_feature_store PRIVATE muzaiten_features Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_feature_store PRIVATE cxx_std_26)
add_test(NAME test_feature_store COMMAND test_feature_store)
set_tests_properties(test_feature_store PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_curation_dialog_data
    test_curation_dialog_data.cpp
)
target_include_directories(test_curation_dialog_data PRIVATE ../src)
target_link_libraries(test_curation_dialog_data PRIVATE muzaiten_reco muzaiten_ui_data Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_curation_dialog_data PRIVATE cxx_std_26)
add_test(NAME test_curation_dialog_data COMMAND test_curation_dialog_data)
set_tests_properties(test_curation_dialog_data PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_song_identity
    test_song_identity.cpp
)
target_include_directories(test_song_identity PRIVATE ../src)
target_link_libraries(test_song_identity PRIVATE muzaiten_features Qt6::Core Qt6::Test)
target_compile_features(test_song_identity PRIVATE cxx_std_26)
add_test(NAME test_song_identity COMMAND test_song_identity)

# The DSP oracle instruments generate fixture audio in float expressions
# (e.g. makeMix's multiply-add chains), so they need the same FMA-contraction
# pin as muzaiten_dsp itself: without it, an ambient -march=native build
# contracts the GENERATOR and shifts the fixture audio by ULPs, which reads
# as (bounded, gate-passing) golden drift that the analyzer never caused.
set(MUZAITEN_DSP_ORACLE_FLAGS
    $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>)

add_executable(test_dsp
    test_dsp.cpp
)
target_include_directories(test_dsp PRIVATE ../src)
target_link_libraries(test_dsp PRIVATE muzaiten_dsp Qt6::Core Qt6::Test)
target_compile_features(test_dsp PRIVATE cxx_std_26)
target_compile_options(test_dsp PRIVATE ${MUZAITEN_DSP_ORACLE_FLAGS})
add_test(NAME test_dsp COMMAND test_dsp)

# DSP stage benchmark + v1 oracle printer. Built by default so it can never
# silently rot, but NOT a registered test: its timings are only meaningful on
# an otherwise idle machine, and its --goldens output is a diff artifact.
add_executable(bench_dsp
    bench_dsp.cpp
)
target_include_directories(bench_dsp PRIVATE ../src)
target_link_libraries(bench_dsp PRIVATE muzaiten_dsp)
target_compile_features(bench_dsp PRIVATE cxx_std_26)
target_compile_options(bench_dsp PRIVATE ${MUZAITEN_DSP_ORACLE_FLAGS})

# Fixed-size clean-room real-FFT probe. Like bench_dsp, it is built by default
# but not registered with CTest because timing is a product decision, not a
# hermetic pass/fail test.
add_executable(bench_fft
    bench_fft.cpp
)
target_include_directories(bench_fft PRIVATE ../src)
target_link_libraries(bench_fft PRIVATE muzaiten_dsp)
target_compile_features(bench_fft PRIVATE cxx_std_26)
target_compile_options(bench_fft PRIVATE ${MUZAITEN_DSP_ORACLE_FLAGS})

# Radio context/fill benchmark. Built by default but not registered with CTest:
# its timings are a product-decision artifact, not a hermetic pass/fail test.
add_executable(bench_radio_context
    bench_radio_context.cpp
)
target_include_directories(bench_radio_context PRIVATE ../src)
target_link_libraries(bench_radio_context PRIVATE muzaiten_app muzaiten_ui_app)
target_compile_features(bench_radio_context PRIVATE cxx_std_26)

# Queue-state save benchmark. Built by default but not registered with CTest:
# it times persisted-document writes, a product-decision artifact, not a gate.
add_executable(bench_queue_state
    bench_queue_state.cpp
)
target_include_directories(bench_queue_state PRIVATE ../src)
target_link_libraries(bench_queue_state PRIVATE muzaiten_app muzaiten_ui_app)
target_compile_features(bench_queue_state PRIVATE cxx_std_26)

add_executable(test_features_refresh
    test_features_refresh.cpp
)
target_include_directories(test_features_refresh PRIVATE ../src)
target_link_libraries(test_features_refresh PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_features_refresh PRIVATE cxx_std_26)
add_dependencies(test_features_refresh muzaiten-features)
add_test(NAME test_features_refresh COMMAND test_features_refresh)

add_executable(test_quality_rank
    test_quality_rank.cpp
)
target_include_directories(test_quality_rank PRIVATE ../src)
target_link_libraries(test_quality_rank PRIVATE muzaiten_features Qt6::Core Qt6::Test)
target_compile_features(test_quality_rank PRIVATE cxx_std_26)
add_test(NAME test_quality_rank COMMAND test_quality_rank)

add_executable(test_play_events
    test_play_events.cpp
)
target_include_directories(test_play_events PRIVATE ../src)
target_link_libraries(test_play_events PRIVATE muzaiten_scrobble Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_play_events PRIVATE cxx_std_26)
add_test(NAME test_play_events COMMAND test_play_events)
set_tests_properties(test_play_events PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_radio
    test_radio.cpp
)
target_include_directories(test_radio PRIVATE ../src)
target_link_libraries(test_radio PRIVATE
    muzaiten_db
    muzaiten_features
    muzaiten_reco
    muzaiten_scrobble
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)
target_compile_features(test_radio PRIVATE cxx_std_26)
add_test(NAME test_radio COMMAND test_radio)
set_tests_properties(test_radio PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_radio_profile
    test_radio_profile.cpp
)
target_include_directories(test_radio_profile PRIVATE ../src)
target_link_libraries(test_radio_profile PRIVATE muzaiten_reco Qt6::Core Qt6::Test)
target_compile_features(test_radio_profile PRIVATE cxx_std_26)
add_test(NAME test_radio_profile COMMAND test_radio_profile)

add_executable(test_scrobble_backfill
    test_scrobble_backfill.cpp
)
target_include_directories(test_scrobble_backfill PRIVATE ../src)
target_link_libraries(test_scrobble_backfill PRIVATE muzaiten_scrobble Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
target_compile_features(test_scrobble_backfill PRIVATE cxx_std_26)
add_test(NAME test_scrobble_backfill COMMAND test_scrobble_backfill)
set_tests_properties(test_scrobble_backfill PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_path_guesser
    test_path_guesser.cpp
)
target_include_directories(test_path_guesser PRIVATE ../src)
target_link_libraries(test_path_guesser PRIVATE muzaiten_scanner Qt6::Core Qt6::Test)
target_compile_features(test_path_guesser PRIVATE cxx_std_26)
add_test(NAME test_path_guesser COMMAND test_path_guesser)

add_executable(test_scan_pipeline
    test_scan_pipeline.cpp
)
target_include_directories(test_scan_pipeline PRIVATE ../src)
target_link_libraries(test_scan_pipeline PRIVATE muzaiten_scanner Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_scan_pipeline PRIVATE cxx_std_26)
add_test(NAME test_scan_pipeline COMMAND test_scan_pipeline)

add_executable(test_tag_rating
    test_tag_rating.cpp
)
target_include_directories(test_tag_rating PRIVATE ../src)
target_link_libraries(test_tag_rating PRIVATE muzaiten_scanner Qt6::Core Qt6::Test)
target_compile_features(test_tag_rating PRIVATE cxx_std_26)
add_test(NAME test_tag_rating COMMAND test_tag_rating)

add_executable(test_path_resolver
    test_path_resolver.cpp
)
target_include_directories(test_path_resolver PRIVATE ../src)
target_link_libraries(test_path_resolver PRIVATE muzaiten_core Qt6::Core Qt6::Test)
target_compile_features(test_path_resolver PRIVATE cxx_std_26)
add_test(NAME test_path_resolver COMMAND test_path_resolver)

add_executable(test_mpd_config
    test_mpd_config.cpp
)
target_include_directories(test_mpd_config PRIVATE ../src)
target_link_libraries(test_mpd_config PRIVATE muzaiten_mpd Qt6::Core Qt6::Test)
target_compile_features(test_mpd_config PRIVATE cxx_std_26)
add_test(NAME test_mpd_config COMMAND test_mpd_config)

add_executable(test_mpd_client
    test_mpd_client.cpp
)
target_include_directories(test_mpd_client PRIVATE ../src)
target_link_libraries(test_mpd_client PRIVATE muzaiten_mpd Qt6::Core Qt6::Network Qt6::Test)
target_compile_features(test_mpd_client PRIVATE cxx_std_26)
add_test(NAME test_mpd_client COMMAND test_mpd_client)

add_executable(test_listenbrainz_url
    test_listenbrainz_url.cpp
)
target_include_directories(test_listenbrainz_url PRIVATE ../src)
target_link_libraries(test_listenbrainz_url PRIVATE muzaiten_scrobble Qt6::Core Qt6::Test)
target_compile_features(test_listenbrainz_url PRIVATE cxx_std_26)
add_test(NAME test_listenbrainz_url COMMAND test_listenbrainz_url)

add_executable(test_listenbrainz_multi_destination
    test_listenbrainz_multi_destination.cpp
)
target_include_directories(test_listenbrainz_multi_destination PRIVATE
    ../src
    ${CMAKE_CURRENT_BINARY_DIR}/../generated
)
target_link_libraries(test_listenbrainz_multi_destination PRIVATE muzaiten_scrobble Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
target_compile_features(test_listenbrainz_multi_destination PRIVATE cxx_std_26)
add_test(NAME test_listenbrainz_multi_destination COMMAND test_listenbrainz_multi_destination)

add_executable(test_scrobble_destinations
    test_scrobble_destinations.cpp
)
target_include_directories(test_scrobble_destinations PRIVATE ../src)
target_link_libraries(test_scrobble_destinations PRIVATE muzaiten_scrobble muzaiten_ui_support Qt6::Core Qt6::Test)
target_compile_features(test_scrobble_destinations PRIVATE cxx_std_26)
add_test(NAME test_scrobble_destinations COMMAND test_scrobble_destinations)

add_executable(test_scrobblers_panel
    test_scrobblers_panel.cpp
)
target_include_directories(test_scrobblers_panel PRIVATE ../src)
target_link_libraries(test_scrobblers_panel PRIVATE muzaiten_ui_scrobble muzaiten_scrobble Qt6::Core Qt6::Sql Qt6::Widgets Qt6::Test)
target_compile_features(test_scrobblers_panel PRIVATE cxx_std_26)
add_test(NAME test_scrobblers_panel COMMAND test_scrobblers_panel)
set_tests_properties(test_scrobblers_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_lastfm_api
    test_lastfm_api.cpp
)
target_include_directories(test_lastfm_api PRIVATE ../src)
target_link_libraries(test_lastfm_api PRIVATE muzaiten_scrobble Qt6::Core Qt6::Network Qt6::Test)
target_compile_features(test_lastfm_api PRIVATE cxx_std_26)
add_test(NAME test_lastfm_api COMMAND test_lastfm_api)

# Search engine tests (no Qt Widgets dependency)
add_executable(test_search_query
    test_search_query.cpp
)
target_include_directories(test_search_query PRIVATE ../src)
target_link_libraries(test_search_query PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_search_query PRIVATE cxx_std_26)
add_test(NAME test_search_query COMMAND test_search_query)

add_executable(test_fuzzy_match
    test_fuzzy_match.cpp
)
target_include_directories(test_fuzzy_match PRIVATE ../src)
target_link_libraries(test_fuzzy_match PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_fuzzy_match PRIVATE cxx_std_26)
add_test(NAME test_fuzzy_match COMMAND test_fuzzy_match)

add_executable(test_fold
    test_fold.cpp
)
target_include_directories(test_fold PRIVATE ../src)
target_link_libraries(test_fold PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_fold PRIVATE cxx_std_26)
target_compile_definitions(test_fold PRIVATE
    FOLD_CASES_PATH="${CMAKE_SOURCE_DIR}/src/search/fold/fold_cases.tsv"
)
add_test(NAME test_fold COMMAND test_fold)

add_executable(test_search_index
    test_search_index.cpp
)
target_include_directories(test_search_index PRIVATE ../src)
target_link_libraries(test_search_index PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_search_index PRIVATE cxx_std_26)
add_test(NAME test_search_index COMMAND test_search_index)

add_executable(test_search_matcher
    test_search_matcher.cpp
)
target_include_directories(test_search_matcher PRIVATE ../src)
target_link_libraries(test_search_matcher PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_search_matcher PRIVATE cxx_std_26)
add_test(NAME test_search_matcher COMMAND test_search_matcher)

add_executable(test_rank_config
    test_rank_config.cpp
)
target_include_directories(test_rank_config PRIVATE ../src)
target_link_libraries(test_rank_config PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_rank_config PRIVATE cxx_std_26)
add_test(NAME test_rank_config COMMAND test_rank_config)

add_executable(test_exclusion
    test_exclusion.cpp
)
target_include_directories(test_exclusion PRIVATE ../src)
target_link_libraries(test_exclusion PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_exclusion PRIVATE cxx_std_26)
add_test(NAME test_exclusion COMMAND test_exclusion)

add_executable(test_result_ranker
    test_result_ranker.cpp
)
target_include_directories(test_result_ranker PRIVATE ../src)
target_link_libraries(test_result_ranker PRIVATE muzaiten_search Qt6::Core Qt6::Test)
target_compile_features(test_result_ranker PRIVATE cxx_std_26)
add_test(NAME test_result_ranker COMMAND test_result_ranker)

add_executable(test_main_panel_keybindings
    test_main_panel_keybindings.cpp
)
target_include_directories(test_main_panel_keybindings PRIVATE ../src)
target_link_libraries(test_main_panel_keybindings PRIVATE muzaiten_ui_support Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_main_panel_keybindings PRIVATE cxx_std_26)
add_test(NAME test_main_panel_keybindings COMMAND test_main_panel_keybindings)

add_executable(test_navigation_scroll
    test_navigation_scroll.cpp
)
target_include_directories(test_navigation_scroll PRIVATE ../src)
target_link_libraries(test_navigation_scroll PRIVATE muzaiten_ui_support Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_navigation_scroll PRIVATE cxx_std_26)
add_test(NAME test_navigation_scroll COMMAND test_navigation_scroll)
set_tests_properties(test_navigation_scroll PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panel_border_style
    test_panel_border_style.cpp
)
target_include_directories(test_panel_border_style PRIVATE ../src)
target_link_libraries(test_panel_border_style PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_panel_border_style PRIVATE cxx_std_26)
add_test(NAME test_panel_border_style COMMAND test_panel_border_style)

add_executable(test_queue_table
    test_queue_table.cpp
)
target_include_directories(test_queue_table PRIVATE ../src)
target_link_libraries(test_queue_table PRIVATE muzaiten_ui_tables Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_queue_table PRIVATE cxx_std_26)
add_test(NAME test_queue_table COMMAND test_queue_table)
set_tests_properties(test_queue_table PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_playlist_view
    test_playlist_view.cpp
)
target_include_directories(test_playlist_view PRIVATE ../src)
target_link_libraries(test_playlist_view PRIVATE muzaiten_ui_tables Qt6::Core Qt6::Sql Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_playlist_view PRIVATE cxx_std_26)
add_test(NAME test_playlist_view COMMAND test_playlist_view)
set_tests_properties(test_playlist_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_responsive_column_layout
    test_responsive_column_layout.cpp
)
target_include_directories(test_responsive_column_layout PRIVATE ../src)
target_link_libraries(test_responsive_column_layout PRIVATE muzaiten_ui_tables Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_responsive_column_layout PRIVATE cxx_std_26)
add_test(NAME test_responsive_column_layout COMMAND test_responsive_column_layout)
set_tests_properties(test_responsive_column_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_playlist_import
    test_playlist_import.cpp
)
target_include_directories(test_playlist_import PRIVATE ../src)
target_link_libraries(test_playlist_import PRIVATE muzaiten_playlist Qt6::Core Qt6::Test)
target_compile_features(test_playlist_import PRIVATE cxx_std_26)
add_test(NAME test_playlist_import COMMAND test_playlist_import)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_demo_screens
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_demo_screens.py
)
add_test(NAME test_playlist_converter
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_playlist_converter.py
            ${CMAKE_SOURCE_DIR}/tools/playlist-import/muzaiten-import)
add_test(NAME test_arch_provider_gate
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_arch_provider_gate.py
            ${CMAKE_SOURCE_DIR}/packaging/features-clap/check-arch-gate.py)

add_executable(test_keybindings_dialog
    test_keybindings_dialog.cpp
)
target_include_directories(test_keybindings_dialog PRIVATE ../src)
target_link_libraries(test_keybindings_dialog PRIVATE muzaiten_ui_dialogs Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_keybindings_dialog PRIVATE cxx_std_26)
add_test(NAME test_keybindings_dialog COMMAND test_keybindings_dialog)
set_tests_properties(test_keybindings_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_album_grid
    test_album_grid.cpp
    ArtworkCacheStub.cpp
    ../src/scanner/ArtworkCache.h
    ../resources/resources.qrc
)
target_include_directories(test_album_grid PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_album_grid PRIVATE muzaiten_ui_library Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_features(test_album_grid PRIVATE cxx_std_26)
add_test(NAME test_album_grid COMMAND test_album_grid)
set_tests_properties(test_album_grid PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_music_explorer_view
    test_music_explorer_view.cpp
    ArtworkCacheStub.cpp
    ../src/scanner/ArtworkCache.h
    ../resources/resources.qrc
)
target_include_directories(test_music_explorer_view PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_music_explorer_view PRIVATE muzaiten_ui_library Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_features(test_music_explorer_view PRIVATE cxx_std_26)
add_test(NAME test_music_explorer_view COMMAND test_music_explorer_view)
set_tests_properties(test_music_explorer_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panel_search_bar
    test_panel_search_bar.cpp
)
target_include_directories(test_panel_search_bar PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_panel_search_bar PRIVATE muzaiten_ui_search Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_panel_search_bar PRIVATE cxx_std_26)
add_test(NAME test_panel_search_bar COMMAND test_panel_search_bar)
set_tests_properties(test_panel_search_bar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panel_search
    test_panel_search.cpp
)
target_include_directories(test_panel_search PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_panel_search PRIVATE muzaiten_ui_search Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_panel_search PRIVATE cxx_std_26)
add_test(NAME test_panel_search COMMAND test_panel_search)
set_tests_properties(test_panel_search PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_player_core
    test_player_core.cpp
)
target_include_directories(test_player_core PRIVATE ../src)
target_link_libraries(test_player_core PRIVATE muzaiten_player muzaiten_playback Qt6::Core Qt6::Test)
target_compile_features(test_player_core PRIVATE cxx_std_26)
add_test(NAME test_player_core COMMAND test_player_core)
set_tests_properties(test_player_core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_gstreamer_playback
    test_gstreamer_playback.cpp
)
target_include_directories(test_gstreamer_playback PRIVATE ../src)
target_link_libraries(test_gstreamer_playback PRIVATE muzaiten_player muzaiten_playback Qt6::Core Qt6::Test)
target_compile_features(test_gstreamer_playback PRIVATE cxx_std_26)
add_test(NAME test_gstreamer_playback COMMAND test_gstreamer_playback)
set_tests_properties(test_gstreamer_playback PROPERTIES ENVIRONMENT "MUZAITEN_DEMO_SILENT_AUDIO=1")

add_executable(test_ipc_server
    test_ipc_server.cpp
)
target_include_directories(test_ipc_server PRIVATE ../src)
target_link_libraries(test_ipc_server PRIVATE muzaiten_ipc Qt6::Core Qt6::Network Qt6::Test)
target_compile_features(test_ipc_server PRIVATE cxx_std_26)
add_test(NAME test_ipc_server COMMAND test_ipc_server)
set_tests_properties(test_ipc_server PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_idle_release_controller
    test_idle_release_controller.cpp
)
target_include_directories(test_idle_release_controller PRIVATE ../src)
target_link_libraries(test_idle_release_controller PRIVATE muzaiten_ui_support Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_idle_release_controller PRIVATE cxx_std_26)
add_test(NAME test_idle_release_controller COMMAND test_idle_release_controller)
set_tests_properties(test_idle_release_controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_menu_highlight_style test_menu_highlight_style.cpp)
target_include_directories(test_menu_highlight_style PRIVATE ../src)
target_link_libraries(test_menu_highlight_style PRIVATE muzaiten_ui_support Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_menu_highlight_style PRIVATE cxx_std_26)
add_test(NAME test_menu_highlight_style COMMAND test_menu_highlight_style)
set_tests_properties(test_menu_highlight_style PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_scrobbling_dialog test_scrobbling_dialog.cpp)
target_include_directories(test_scrobbling_dialog PRIVATE ../src)
target_link_libraries(test_scrobbling_dialog PRIVATE muzaiten_ui_scrobble muzaiten_scrobble Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_scrobbling_dialog PRIVATE cxx_std_26)
add_test(NAME test_scrobbling_dialog COMMAND test_scrobbling_dialog)
set_tests_properties(test_scrobbling_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_listening_history_panel test_listening_history_panel.cpp)
target_include_directories(test_listening_history_panel PRIVATE ../src)
target_link_libraries(test_listening_history_panel PRIVATE muzaiten_ui_scrobble muzaiten_scrobble Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_listening_history_panel PRIVATE cxx_std_26)
add_test(NAME test_listening_history_panel COMMAND test_listening_history_panel)
set_tests_properties(test_listening_history_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_main_window_navigation
    test_main_window_navigation.cpp
    ../resources/resources.qrc
)
target_include_directories(test_main_window_navigation PRIVATE ../src)
target_link_libraries(test_main_window_navigation PRIVATE muzaiten_app muzaiten_ui_app Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_main_window_navigation PRIVATE cxx_std_26)
add_test(NAME test_main_window_navigation COMMAND test_main_window_navigation)
set_tests_properties(test_main_window_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Offline matcher benchmark tool (dev only; not a registered test). Needs a real
# library.sqlite copy: ./playlist_match_bench <library.sqlite> <playlist.jsonl ...>
# Off by default so a normal build pays nothing; enable with
#   cmake -DMUZAITEN_BUILD_MATCH_BENCH=ON
option(MUZAITEN_BUILD_MATCH_BENCH "Build the offline playlist-matcher benchmark tool" OFF)
if(MUZAITEN_BUILD_MATCH_BENCH)
add_executable(playlist_match_bench
    ../tools/playlist-import/match_bench.cpp
)
target_include_directories(playlist_match_bench PRIVATE ../src)
target_link_libraries(playlist_match_bench PRIVATE muzaiten_playlist Qt6::Core Qt6::Sql)
target_compile_features(playlist_match_bench PRIVATE cxx_std_26)
endif()

get_property(MUZAITEN_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
if(MUZAITEN_REGISTERED_TESTS)
    set_property(TEST ${MUZAITEN_REGISTERED_TESTS} APPEND PROPERTY
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" "QT_QPA_PLATFORMTHEME=")
endif()
