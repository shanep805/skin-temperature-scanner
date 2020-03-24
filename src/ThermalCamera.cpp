//
// Created by G.F. Duivesteijn on 23.03.20.
//
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>
#include <sstream>
#include <iomanip>
#include <iostream>
#include "ThermalCamera.h"
#include "constants.h"

ThermalCamera::ThermalCamera() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== ThermalCamera, Copyright 2020 Ava-X ===");
    char *base_path = SDL_GetBasePath();
    if (base_path) {
        resource_path = std::string(base_path) + "/resources";
//        resource_path = "../resources";
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Resource path: %s\n", resource_path.c_str());
    }
    init_sdl();
    init_sensor();
    // Load slider background image
    is_running = true;
    is_measuring = false;
    is_measuring_lpf = is_measuring;
    mean_val = 0.0f;
    mean_val_lpf = 0.0f;
    timer_is_animating = 0;
    animation_frame_nr = 0;
}

ThermalCamera::~ThermalCamera() {
    clean();
}

void ThermalCamera::init_sdl() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init() Failed: %s\n", SDL_GetError());
    }
    window = SDL_CreateWindow("ThermalCamera", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
                              SDL_WINDOW_FULLSCREEN);
    if (window == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow() Failed: %s\n", SDL_GetError());
        clean();
        exit(EXIT_FAILURE);
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateRenderer() Failed: %s\n", SDL_GetError());
        clean();
        exit(EXIT_FAILURE);
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, SENSOR_W, SENSOR_H);
    if (texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateTexture() Failed: %s\n", SDL_GetError());
        clean();
        exit(EXIT_FAILURE);
    }

    texture_r = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, SENSOR_H, SENSOR_H);
    if (texture_r == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateTexture() Failed: %s\n", SDL_GetError());
        clean();
        exit(EXIT_FAILURE);
    }
    // Load and create slider background
    std::string slider_bg_path = resource_path + "/images/slider_bg.bmp";
    SDL_Surface *image = SDL_LoadBMP(slider_bg_path.c_str());
    if (image == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_LoadBMP() Failed: %s\n", SDL_GetError());
        clean();
        exit(EXIT_FAILURE);
    }
    slider = SDL_CreateTextureFromSurface(renderer, image);
    SDL_FreeSurface(image);
    // Load animation
    for (int i=0; i<6; i++) {
        std::string file_path = resource_path + "/images/anim" + std::to_string(i+1) + ".bmp";
        SDL_Surface *_image = SDL_LoadBMP(file_path.c_str());
        if (_image == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_LoadBMP() Failed: %s\n", SDL_GetError());
            clean();
            exit(EXIT_FAILURE);
        }
        SDL_Texture *_frame = SDL_CreateTextureFromSurface(renderer, _image);
        animation.push_back(_frame);
    }
    // Hide cursor
    int SDL_ShowCursor(SDL_DISABLE);
    // Init fonts
    TTF_Init();
    font64 = TTF_OpenFont(FONT_PATH.c_str(), 64);
    font32 = TTF_OpenFont(FONT_PATH.c_str(), 36);
    if (font64 == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to load font %s", FONT_PATH, TTF_GetError());
        clean();
        exit(EXIT_FAILURE);
    }
    SDL_GetRendererOutputSize(renderer, &display_width, &display_height);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display dimension: (%d, %d)", display_width, display_height);
    // Set scaling and aspect ratio
    const double display_ratio = (double) display_width / display_height;
    const double sensor_ratio = (double) SENSOR_W / SENSOR_H;
    if (display_ratio >= sensor_ratio) {
        aspect_scale = display_height / SENSOR_H;
    } else {
        aspect_scale = display_width / SENSOR_W;
    }
    output_width = SENSOR_W * aspect_scale;
    output_height = SENSOR_H * aspect_scale;
    offset_left = (display_width - output_width) / 2;
    offset_top = (display_height - output_height) / 2;
    // Override offset top to align the image with the top edge.
    offset_top = 0;
    rect_preserve_aspect = (SDL_Rect) {.x = offset_left, .y = offset_top, .w = output_width, .h = output_height};
    rect_fullscreen = (SDL_Rect) {.x = 0, .y = 0, .w = display_width, .h = display_height};
}

void ThermalCamera::init_sensor() {
    MLX90640_SetDeviceMode(MLX_I2C_ADDR, 0);
    MLX90640_SetSubPageRepeat(MLX_I2C_ADDR, 0);
    switch (FPS) {
        case 1:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b001);
            break;
        case 2:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b010);
            break;
        case 4:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b011);
            break;
        case 8:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b100);
            break;
        case 16:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b101);
            break;
        case 32:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b110);
            break;
        case 64:
            MLX90640_SetRefreshRate(MLX_I2C_ADDR, 0b111);
            break;
        default:
            printf("Unsupported framerate: %d", FPS);
            clean();
            exit(EXIT_FAILURE);
    }
    MLX90640_SetChessMode(MLX_I2C_ADDR);
    MLX90640_DumpEE(MLX_I2C_ADDR, eeMLX90640);
    MLX90640_ExtractParameters(eeMLX90640, &mlx90640);
}

void ThermalCamera::clean() {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
    }
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    if (texture_r != nullptr) {
        SDL_DestroyTexture(texture_r);
    }
    SDL_Quit();
}

void ThermalCamera::update() {
    auto start = std::chrono::system_clock::now();
    MLX90640_GetFrameData(MLX_I2C_ADDR, frame);

    eTa = MLX90640_GetTa(frame, &mlx90640) - 6.0f;
    eTa = 20.0f;
    MLX90640_CalculateTo(frame, &mlx90640, EMISSIVITY, eTa, mlx90640To);

    MLX90640_BadPixelsCorrection((&mlx90640)->brokenPixels, mlx90640To, 1, &mlx90640);
    MLX90640_BadPixelsCorrection((&mlx90640)->outlierPixels, mlx90640To, 1, &mlx90640);

    // Scan the sensor and compute the mean skin temperature, assuming that skin temperature is between
    // MIN_TEMPERATURE and MAX_TEMPERATURE.
    float max_val = -1.0f;
    float sum_val = 0.0f;
    int n_val = 0;
    for (int y = 0; y < SENSOR_W; y++) {
        for (int x = 0; x < SENSOR_H; x++) {
            // Read the temperature value for this pixel.
            float val = mlx90640To[SENSOR_H * (SENSOR_W - 1 - y) + x];
            // Map the value to a color
            colormap(y, x, val, MIN_TEMPERATURE, MAX_TEMPERATURE);
            // Sum and count the temperatures within the skin temperature range.
            if (val > MIN_MEASURE_RANGE && val < MAX_MEASURE_RANGE) {
                sum_val += val;
                n_val += 1;
            }
        }
    }
    // Check if there are enough pixels within the temperature measuring range.
    bool is_measuring_prev = is_measuring;
    is_measuring = n_val > MEASURE_AREA_THRESHOLD;
    if (is_measuring_prev != is_measuring) {
        timer_is_measuring = 0;
    } else {
        timer_is_measuring++;
    }
    if (timer_is_measuring > TIMER_THRESHOLD_FRAMES) {
        is_measuring_lpf = is_measuring;
    }
    // Compute the mean of the temperatures in the range.
    if (n_val > 0) {
        mean_val = sum_val / (float) n_val;
    } else {
        mean_val = -1.0f;
    }
    // Correct the mean value with a moving mean over time.
    if (mean_val_lpf > 0 && mean_val > MIN_MEASURE_RANGE && mean_val < MAX_MEASURE_RANGE) {
        if (abs(mean_val_lpf - mean_val) > 0.6) {
            mean_val_lpf = mean_val;
        } else {
            mean_val_lpf = BETA * mean_val_lpf + (1 - BETA) * mean_val;
        }
    } else if (mean_val_lpf < 0 && mean_val > MIN_MEASURE_RANGE && mean_val < MAX_MEASURE_RANGE) {
        mean_val_lpf = mean_val;
    } else {
        mean_val_lpf = -1.0f;
    }
    // Format the temperature value to string
    std::stringstream message_ss;
    if (mean_val > MIN_MEASURE_RANGE && mean_val < MAX_MEASURE_RANGE) {
        message_ss << std::fixed << std::setprecision(1) << std::setw(4);
        message_ss << mean_val_lpf << "\xB0" << "C" << std::endl;
        message = message_ss.str();
    } else {
        message = "";
    }
}


void ThermalCamera::render() {

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    render_sensor_frame();
    if (is_measuring_lpf) {
        render_slider();
        render_temp_labels();
    } else {
        render_animation();
//        SDL_Point origin = {0, 0};
//        SDL_Color text_color = {255, 255, 255, 255};
//        std::string label = "Move closer...";
//        render_text(label, text_color, origin, 3, font64);
    }
    SDL_RenderPresent(renderer);
}

void ThermalCamera::render_temp_labels() const {
    SDL_Point origin = {0, 640 - 48};
    SDL_Color text_color = {255, 255, 255, 255};
    render_text(message, text_color, origin, 1, font32);
    std::string label = "Skin temperature:";
    render_text(label, text_color, origin, 0, font32);
    origin = {0, 0};
    if (mean_val_lpf <= 31.8) {
        label = "Low";
    }
    else if (mean_val_lpf > 31.8 && mean_val_lpf <= 34.2) {
        label = "Normal";
    }
    else if (mean_val_lpf > 34.2 && mean_val_lpf <= 35.0) {
        label = "High";
    }
    else if (mean_val_lpf > 35) {
        label = "Very high";
    }
    render_text(label, text_color, origin, 3, font64);
}

void
ThermalCamera::render_text(const std::string &text, const SDL_Color &text_color, const SDL_Point origin,
                           const int anchor,
                           TTF_Font *font) const {
    SDL_Surface *surf = TTF_RenderText_Solid(font, text.c_str(), text_color);
    SDL_Texture *texture_txt = SDL_CreateTextureFromSurface(renderer, surf);
    int text_width, text_height;
    SDL_QueryTexture(texture_txt, nullptr, nullptr, &text_width, &text_height);
    SDL_Rect dst;
    // top left
    if (anchor == 0) {
        dst = {origin.x, origin.y, text_width, text_height};
    }
        // top right
    else if (anchor == 1) {
        dst = {display_width - text_width - origin.x, origin.y, text_width, text_height};
    }
        // bottom right
    else if (anchor == 2) {
        dst = {display_width - text_width - origin.x, display_height - text_height - origin.y, text_width,
               text_height};
    }
        // bottom left
    else if (anchor == 3) {
        dst = {origin.x, display_height - text_height - origin.y, text_width, text_height};
    }
    SDL_RenderCopy(renderer, texture_txt, nullptr, &dst);
    SDL_FreeSurface(surf);
    SDL_DestroyTexture(texture_txt);
}

void ThermalCamera::render_sensor_frame() const {
    SDL_UpdateTexture(texture, nullptr, (uint8_t *) pixels, SENSOR_W * sizeof(uint32_t));
    SDL_SetRenderTarget(renderer, texture_r);
    SDL_RenderCopyEx(renderer, texture, nullptr, nullptr, rotation, nullptr, SDL_FLIP_NONE);
    SDL_SetRenderTarget(renderer, nullptr);
    if (preserve_aspect) {
        SDL_RenderCopy(renderer, texture_r, nullptr, &rect_preserve_aspect);
    } else {
        SDL_RenderCopy(renderer, texture_r, nullptr, &rect_fullscreen);
    }
}

void ThermalCamera::render_slider() const {
    if (!is_measuring_lpf) {
        return;
    }
    const int ys1 = 640 + 20;
    const int ys2 = 640 + 60;
    const int margin1 = 4;
    const int margin2 = margin1 + 2;
    SDL_Rect marker_rect;
    // slider background
    SDL_Rect rect_slider = {0, ys1, display_width, ys2 - ys1};
    SDL_RenderCopy(renderer, slider, nullptr, &rect_slider);
    // marker
    auto x_pos = (mean_val_lpf - 31) / (36 - 31);
    x_pos = fmin(1.0, x_pos);
    x_pos = fmax(0.0, x_pos);
    int x_marker = static_cast<int>(round(x_pos * (float) display_width));

    marker_rect = {x_marker - margin2, ys1 - margin2, ys2 - ys1 + 2 * margin2};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &marker_rect);

    marker_rect = {x_marker - margin1, ys1 - margin1, 2 * margin1, ys2 - ys1 + 2 * margin1};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(renderer, &marker_rect);

}

void ThermalCamera::handle_events() {
    SDL_Event event;
    SDL_PollEvent(&event);
    if (event.type == SDL_QUIT) {
        is_running = false;
    }
    if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                is_running = false;
                break;
            default:
                break;
        }
    }
}

void ThermalCamera::colormap(const int x, const int y, float v, float vmin, float vmax) {
    // Heatmap code borrowed from: http://www.andrewnoske.com/wiki/Code_-_heatmaps_and_color_gradients
    const int NUM_COLORS = 7;
    static float color[NUM_COLORS][3] = {{0, 0, 0},
                                         {0, 0, 1},
                                         {0, 1, 0},
                                         {1, 1, 0},
                                         {1, 0, 0},
                                         {1, 0, 1},
                                         {1, 1, 1}};
    int idx1, idx2;
    float fractBetween = 0;
    v = (v - vmin) / (vmax - vmin);
    if (v < 0) {
        idx1 = 0;
        idx2 = 0;
    } else if (v >= 1) {
        idx1 = NUM_COLORS - 1;
        idx2 = NUM_COLORS - 1;
    } else {
        v *= (NUM_COLORS - 1);
        idx1 = floor(v);
        idx2 = idx1 + 1;
        fractBetween = v - float(idx1);
    }
    const auto ir = static_cast<uint8_t>((((color[idx2][0] - color[idx1][0]) * fractBetween) + color[idx1][0]) * 255.0);
    const auto ig = static_cast<uint8_t>((((color[idx2][1] - color[idx1][1]) * fractBetween) + color[idx1][1]) * 255.0);
    const auto ib = static_cast<uint8_t>((((color[idx2][2] - color[idx1][2]) * fractBetween) + color[idx1][2]) * 255.0);
    const uint offset = (y * SENSOR_W + x);
    pixels[offset] = ib << 16u | ig << 8u | ir << 0u;
}

void ThermalCamera::render_animation() {

    if (timer_is_animating > TIMER_THRESHOLD_FRAMES) {
        timer_is_animating = 0;
        animation_frame_nr++;
        animation_frame_nr = animation_frame_nr >= animation.size() ? 0 : animation_frame_nr;
    }
    else {
        timer_is_animating++;
    }
    SDL_Rect animation_rect = {0, output_height, display_width, display_height - output_height};
    SDL_RenderCopy(renderer, animation.at(animation_frame_nr), nullptr, &animation_rect);

}
