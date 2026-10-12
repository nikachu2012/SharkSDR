#include <SharkSDR/ui/texture.h>

#if defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>  // pulls in the system OpenGL header (GL 1.1 functions are enough here)

#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace SharkSDR::ui {

Texture::~Texture() { destroy(); }

void Texture::create(int width, int height, bool repeatY) {
    destroy();
    if (width <= 0 || height <= 0) return;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeatY ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    id_ = id;
    width_ = width;
    height_ = height;
}

void Texture::destroy() {
    if (id_) {
        GLuint id = id_;
        glDeleteTextures(1, &id);
    }
    id_ = 0;
    width_ = height_ = 0;
}

void Texture::updateRows(int y, int rows, const uint32_t* pixels) {
    if (!id_ || rows <= 0 || y < 0 || y + rows > height_) return;
    glBindTexture(GL_TEXTURE_2D, id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width_, rows, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace SharkSDR::ui
