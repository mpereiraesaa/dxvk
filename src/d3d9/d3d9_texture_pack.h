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
   *    upload once, as <dir>/<w>x<h>-<hash>.tga, the art a pack starts from;
   *    d3d9.textureDumpMaxSize leaves out textures wider or taller than it.
   *  - d3d9.texturePack / DXVK_TEXTURE_PACK=<dir> replaces an upload whose
   *    size and hash have a <dir>/<w>x<h>-<hash>.png. The image may be any
   *    size; games that sample with normalised coordinates show it at its
   *    own resolution. Uploads of a size the pack has no image for are not
   *    hashed at all.
   *
   * A relative directory is relative to the game's working directory.
   *
   * The hash is XXH3-64 (xxHash 0.8) of the level-0 texels in the game's
   * own format, rows back to back without padding, as 16 hex digits; <w>
   * and <h> are the texture's size in texels.
   */
  class D3D9TexturePack {

  public:

    struct Image {
      uint32_t             width  = 0;
      uint32_t             height = 0;
      std::vector<uint8_t> bgra;
    };

    void configure(const std::string& dumpDir, uint32_t dumpMaxSize, const std::string& packDir);

    bool dumping() const { return !m_dumpDir.empty(); }

    /// Whether uploads of this size are dumped
    bool dumpsSize(uint32_t width, uint32_t height) const {
      return dumping() && (!m_dumpMaxSize || (width <= m_dumpMaxSize && height <= m_dumpMaxSize));
    }

    bool replacing() const { return !m_packDir.empty(); }

    /// Whether the pack has any image for textures of this size
    bool hasSize(uint32_t width, uint32_t height) const {
      return m_sizes.count(sizeKey(width, height)) != 0;
    }

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
    std::unique_ptr<Image> load(uint32_t width, uint32_t height, uint64_t hash) const;

  private:

    std::string                   m_dumpDir;
    uint32_t                      m_dumpMaxSize = 0;
    std::string                   m_packDir;
    struct Key {
      uint64_t hash;
      uint32_t size;
      bool operator == (const Key& other) const { return hash == other.hash && size == other.size; }
    };

    struct KeyHash {
      size_t operator () (const Key& key) const { return size_t(key.hash ^ (uint64_t(key.size) << 32)); }
    };

    static uint32_t sizeKey(uint32_t width, uint32_t height) { return (width << 16) | height; }

    std::unordered_set<Key, KeyHash> m_available;
    std::unordered_set<uint32_t>  m_sizes;

    dxvk::mutex                   m_dumpMutex;
    std::unordered_set<uint64_t>  m_dumped;

  };

}
