namespace lcs::core {
    struct Config {
        int width = 1920;
        int height = 1080;
        bool fullscreen = false;
        bool vsync = true;
        int max_fps = 60;
    };
    Config g_config;
}
