#pragma once

#include <SharkSDR/export.h>

#include <imgui.h>

#include <cstdint>

namespace SharkSDR::ui {

// RGBA8 GPU texture usable with ImGui::Image(). Create, update and destroy it
// from the UI thread only.
class SHARKSDR_API Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // (Re)creates the texture. `repeatY` enables wrapping in the vertical
    // direction (useful for scrolling images such as waterfalls).
    void create(int width, int height, bool repeatY = false);
    void destroy();

    // Upload `rows` rows starting at row `y`. `pixels` holds width*rows RGBA values.
    void updateRows(int y, int rows, const uint32_t* pixels);

    bool valid() const { return id_ != 0; }
    int width() const { return width_; }
    int height() const { return height_; }
    ImTextureID id() const { return ImTextureID(id_); }

private:
    unsigned int id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace SharkSDR::ui
