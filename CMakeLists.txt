cmake_minimum_required(VERSION 3.20.0)

if(CONFIG_ZMK_KEY_LAYER_DEFER)
    target_sources(app PRIVATE src/key_layer_defer.c)
endif()
