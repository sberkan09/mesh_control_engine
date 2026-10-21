# The stock player's UI feeds without a window (M-MISSION 3b): the player's minimap_feed.cpp is compiled into the
# test, the world and the document are built in code, no GPU.
mce_add_test(test_player_minimap UMBRELLA
    SOURCES
        test_minimap_feed.cpp
        ${PROJECT_SOURCE_DIR}/src/player/src/minimap_feed.cpp
    DEPS
        mce::mce_gameplay_kit
        mce::mce_scene
        mce::mce_ui
        mce::mce_render)
target_include_directories(test_player_minimap PRIVATE ${PROJECT_SOURCE_DIR}/src/player/src)

# The inventory widgets (M-INVENTORY 4b): item_grid_feed.cpp compiled into the test, no GPU.
mce_add_test(test_player_item_grid UMBRELLA
    SOURCES
        test_item_grid_feed.cpp
        ${PROJECT_SOURCE_DIR}/src/player/src/item_grid_feed.cpp
    DEPS
        mce::mce_gameplay_kit
        mce::mce_scene
        mce::mce_ui
        mce::mce_render)
target_include_directories(test_player_item_grid PRIVATE ${PROJECT_SOURCE_DIR}/src/player/src)
