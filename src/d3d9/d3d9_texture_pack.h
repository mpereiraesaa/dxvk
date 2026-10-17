#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../util/thread.h"
#include "../dxvk/dxvk_image.h"

#include "d3d9_format.h"

namespace dxvk {

  /**
   * \brief Views of a texture pack image, plain and sRGB
   */
  struct D3D9TexturePackView {
    Rc<DxvkImageView> color;
    Rc<DxvkImageView> srgb;
  };

  /**
   * \brief Texture pack for D3D9 games
   *
   * Replaces the textures a game uploads with larger images of the same
   * content, chosen by a hash of the game's texels. Both directories come
   * from dxvk.conf, or the environment, which wins:
   *
   *  - d3d9.textureDump / DXVK_TEXTURE_DUMP=<dir> writes each distinct
   *    upload once, as <dir>/<hash>.tga, the art a pack starts from.
   *  - d3d9.texturePack / DXVK_TEXTURE_PACK=<dir> replaces an upload whose
   *    hash has a <dir>/<hash>.png. The image may be any size; games that
   *    sample with normalised coordinates show it at its own resolution.
   *
   * A relative directory is relative to the game's working directory.
   *
   * The hash is FNV-1a 64 over the level-0 texels in the game's own
   * format, row by row, taking each row as little-endian 64-bit words and
   * then its remaining bytes, printed as 16 hex digits.
   */
  class D3D9TexturePack {

  public:

    struct Image {
      uint32_t             width  = 0;
      uint32_t             height = 0;
      std::vector<uint8_t> bgra;
    };

    void configure(const std::string& dumpDir, const std::string& packDir);

    bool dumping() const { return !m_dumpDir.empty(); }

    bool replacing() const { return !m_packDir.empty(); }

    static bool supportsFormat(D3D9Format format);

    static uint64_t hash(
      const void*   data,
            size_t  rowBytes,
            uint32_t rows,
            size_t  pitch);

    void dump(
            uint64_t    hash,
            D3D9Format  format,
      const void*       data,
            uint32_t    width,
            uint32_t    height,
            size_t      pitch);

    /**
     * \brief Decodes the pack's image for a hash
     * \returns The image, or null when the pack has none
     */
    std::unique_ptr<Image> load(uint64_t hash) const;

  private:

    std::string                   m_dumpDir;
    std::string                   m_packDir;
    std::unordered_set<uint64_t>  m_available;

    dxvk::mutex                   m_dumpMutex;
    std::unordered_set<uint64_t>  m_dumped;

  };

}
