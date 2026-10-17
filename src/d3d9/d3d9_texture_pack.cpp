#include "d3d9_texture_pack.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "../util/log/log.h"
#include "../util/util_string.h"

#define XXH_INLINE_ALL
#include "../util/xxhash.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "../util/stb_image.h"
#pragma GCC diagnostic pop

namespace dxvk {

  static std::string fileName(uint32_t width, uint32_t height, uint64_t hash, const char* extension) {
    char name[64];
    std::snprintf(name, sizeof(name), "%ux%u-%016llx.%s", width, height,
      static_cast<unsigned long long>(hash), extension);
    return name;
  }


  void D3D9TexturePack::configure(const std::string& dumpDir, uint32_t dumpMaxSize, const std::string& packDir) {
    m_dumpDir = dumpDir;
    m_dumpMaxSize = dumpMaxSize;
    m_packDir = packDir;

    if (m_packDir.empty())
      return;

    // Index the pack once, so a game's uploads never touch the file system
    // for textures the pack does not have.
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(str::topath(m_packDir.c_str()), ec)) {
      std::string name = entry.path().filename().string();

      unsigned width = 0, height = 0;
      unsigned long long value = 0;
      char extension[8] = { };

      if (std::sscanf(name.c_str(), "%ux%u-%16llx.%7s", &width, &height, &value, extension) != 4
       || std::strcmp(extension, "png") || !width || !height || width > 0xffff || height > 0xffff)
        continue;

      m_available.insert({ value, sizeKey(width, height) });
      m_sizes.insert(sizeKey(width, height));
    }

    Logger::info(str::format("D3D9: Texture pack ", m_packDir, ": ", m_available.size(), " images"));
  }


  bool D3D9TexturePack::supportsFormat(D3D9Format format) {
    switch (format) {
      case D3D9Format::A8R8G8B8:
      case D3D9Format::X8R8G8B8:
      case D3D9Format::A1R5G5B5:
      case D3D9Format::X1R5G5B5:
      case D3D9Format::R5G6B5:
      case D3D9Format::A4R4G4B4:
        return true;
      default:
        return false;
    }
  }


  uint64_t D3D9TexturePack::hash(
    const void*   data,
          size_t  rowBytes,
          uint32_t rows,
          size_t  pitch) {
    // XXH3-64 of the rows back to back, without the pitch's padding.
    if (pitch == rowBytes)
      return XXH3_64bits(data, rowBytes * rows);

    XXH3_state_t state;
    XXH3_64bits_reset(&state);

    for (uint32_t y = 0; y < rows; y++)
      XXH3_64bits_update(&state, reinterpret_cast<const uint8_t*>(data) + y * pitch, rowBytes);

    return XXH3_64bits_digest(&state);
  }


  void D3D9TexturePack::dump(
          uint64_t    hash,
          D3D9Format  format,
    const void*       data,
          uint32_t    width,
          uint32_t    height,
          size_t      pitch) {
    if (!supportsFormat(format) || !width || !height || width > 0xffff || height > 0xffff)
      return;

    { std::lock_guard<dxvk::mutex> lock(m_dumpMutex);
      if (!m_dumped.insert(hash).second)
        return;
    }

    // Uncompressed 32-bit TGA, top-left origin, 8 alpha bits.
    std::vector<uint8_t> out(18 + size_t(width) * height * 4);
    out[2] = 2;
    out[12] = width & 0xff;  out[13] = width >> 8;
    out[14] = height & 0xff; out[15] = height >> 8;
    out[16] = 32;            out[17] = 0x28;

    bool wide = format == D3D9Format::A8R8G8B8 || format == D3D9Format::X8R8G8B8;

    for (uint32_t y = 0; y < height; y++) {
      auto row = reinterpret_cast<const uint8_t*>(data) + y * pitch;
      uint8_t* dst = &out[18 + size_t(y) * width * 4];

      for (uint32_t x = 0; x < width; x++, dst += 4) {
        uint32_t r, g, b, a = 0xff;

        if (wide) {
          b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2];
          if (format == D3D9Format::A8R8G8B8)
            a = row[x * 4 + 3];
        } else {
          uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);

          if (format == D3D9Format::R5G6B5) {
            r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3;
          } else if (format == D3D9Format::A4R4G4B4) {
            a = (v >> 12) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17;
          } else {
            r = ((v >> 10) & 31) << 3; g = ((v >> 5) & 31) << 3; b = (v & 31) << 3;
            if (format == D3D9Format::A1R5G5B5)
              a = (v & 0x8000) ? 0xff : 0;
          }
        }

        dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = a;
      }
    }

    std::string path = str::format(m_dumpDir, "/", fileName(width, height, hash, "tga"));

    if (FILE* file = std::fopen(path.c_str(), "wb")) {
      std::fwrite(out.data(), 1, out.size(), file);
      std::fclose(file);
    }
  }


  std::unique_ptr<D3D9TexturePack::Image> D3D9TexturePack::load(uint32_t width, uint32_t height, uint64_t hash) const {
    if (!m_available.count({ hash, sizeKey(width, height) }))
      return nullptr;

    std::string path = str::format(m_packDir, "/", fileName(width, height, hash, "png"));
    std::vector<uint8_t> file;

    if (FILE* handle = std::fopen(path.c_str(), "rb")) {
      std::fseek(handle, 0, SEEK_END);
      long size = std::ftell(handle);
      std::fseek(handle, 0, SEEK_SET);

      if (size > 0) {
        file.resize(size);
        file.resize(std::fread(file.data(), 1, file.size(), handle));
      }

      std::fclose(handle);
    }

    int w = 0, h = 0, channels = 0;
    stbi_uc* rgba = file.empty() ? nullptr
      : stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &channels, 4);

    if (!rgba) {
      Logger::warn(str::format("D3D9: Texture pack: cannot decode ", path));
      return nullptr;
    }

    auto image = std::make_unique<Image>();
    image->width = uint32_t(w);
    image->height = uint32_t(h);
    image->bgra.resize(size_t(w) * h * 4);

    for (size_t i = 0; i < size_t(w) * h; i++) {
      image->bgra[i * 4 + 0] = rgba[i * 4 + 2];
      image->bgra[i * 4 + 1] = rgba[i * 4 + 1];
      image->bgra[i * 4 + 2] = rgba[i * 4 + 0];
      image->bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }

    stbi_image_free(rgba);
    return image;
  }

}
