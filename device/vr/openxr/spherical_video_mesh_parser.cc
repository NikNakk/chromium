// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/spherical_video_mesh_parser.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "third_party/zlib/zlib.h"

namespace device {

namespace {

constexpr uint32_t kTypeRaw = 0x72617720;   // 'raw '
constexpr uint32_t kTypeDfl8 = 0x64666c38;  // 'dfl8'
constexpr uint32_t kTypeMesh = 0x6d657368;  // 'mesh'

constexpr uint32_t kMaxCoordinateCount = 10000;
constexpr uint32_t kMaxVertexCount = 32 * 1000;
constexpr uint32_t kMaxSourceIndexCount = 128 * 1000;
constexpr uint32_t kMaxVertexListCount = 64;
constexpr size_t kMaxProjectionDataBytes = 4 * 1024 * 1024;
constexpr size_t kMaxInflatedBytes = 8 * 1024 * 1024;

class ByteReader {
 public:
  explicit ByteReader(base::span<const uint8_t> data) : data_(data) {}

  bool ReadU32(uint32_t* value) {
    if (remaining().size() < 4) {
      return false;
    }
    const uint8_t* p = remaining().data();
    *value = (static_cast<uint32_t>(p[0]) << 24) |
             (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[2]) << 8) |
             static_cast<uint32_t>(p[3]);
    position_ += 4;
    return true;
  }

  bool ReadFloat(float* value) {
    uint32_t bits = 0;
    if (!ReadU32(&bits)) {
      return false;
    }
    *value = std::bit_cast<float>(bits);
    return true;
  }

  bool ReadBytes(size_t size, base::span<const uint8_t>* value) {
    if (size > remaining().size()) {
      return false;
    }
    *value = remaining().first(size);
    position_ += size;
    return true;
  }

  base::span<const uint8_t> remaining() const {
    return data_.subspan(position_);
  }

 private:
  base::span<const uint8_t> data_;
  size_t position_ = 0;
};

class BitReader {
 public:
  explicit BitReader(base::span<const uint8_t> data) : data_(data) {}

  bool ReadBits(uint32_t count, uint32_t* value) {
    if (count > 32 || bit_position_ + count > data_.size() * 8u) {
      return false;
    }
    uint32_t result = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const size_t position = bit_position_++;
      const uint8_t byte = data_[position / 8];
      const uint8_t bit = (byte >> (7 - (position % 8))) & 1;
      result = (result << 1) | bit;
    }
    *value = result;
    return true;
  }

  void AlignToByte() {
    bit_position_ = (bit_position_ + 7u) & ~size_t{7u};
  }

 private:
  base::span<const uint8_t> data_;
  size_t bit_position_ = 0;
};

uint32_t BitsForDeltaIndex(uint32_t count) {
  if (count == 0) {
    return 0;
  }
  uint64_t values = static_cast<uint64_t>(count) * 2u;
  uint32_t bits = 0;
  --values;
  while (values != 0) {
    ++bits;
    values >>= 1;
  }
  return std::max<uint32_t>(bits, 1u);
}

int32_t DecodeZigZag(uint32_t value) {
  return static_cast<int32_t>((value >> 1) ^
                              (0u - static_cast<uint32_t>(value & 1u)));
}

bool InflateRawDeflate(base::span<const uint8_t> compressed,
                       std::vector<uint8_t>* output) {
  if (compressed.size() > std::numeric_limits<uInt>::max()) {
    return false;
  }

  z_stream stream = {};
  stream.next_in = const_cast<Bytef*>(
      reinterpret_cast<const Bytef*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());

  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
    return false;
  }

  bool ok = false;
  std::array<uint8_t, 32768> buffer;
  for (;;) {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    const int result = inflate(&stream, Z_NO_FLUSH);
    const size_t produced = buffer.size() - stream.avail_out;

    if (produced > kMaxInflatedBytes - output->size()) {
      break;
    }
    output->insert(output->end(), buffer.begin(), buffer.begin() + produced);

    if (result == Z_STREAM_END) {
      ok = stream.avail_in == 0;
      break;
    }
    if (result != Z_OK || (produced == 0 && stream.avail_in == 0)) {
      break;
    }
  }

  inflateEnd(&stream);
  return ok;
}

bool AppendTriangles(uint8_t primitive_type,
                     const std::vector<uint32_t>& source_indices,
                     std::vector<uint32_t>* triangles) {
  const size_t old_size = triangles->size();
  size_t additional = 0;
  switch (primitive_type) {
    case 0:  // triangles
      if (source_indices.size() % 3 != 0) {
        return false;
      }
      additional = source_indices.size();
      break;
    case 1:  // triangle strip
    case 2:  // triangle fan
      if (source_indices.size() >= 3) {
        additional = (source_indices.size() - 2) * 3;
      }
      break;
    default:
      return false;
  }

  if (additional > static_cast<size_t>(kMaxSourceIndexCount) * 3 ||
      old_size > static_cast<size_t>(kMaxSourceIndexCount) * 3 - additional) {
    return false;
  }

  if (primitive_type == 0) {
    triangles->insert(triangles->end(), source_indices.begin(),
                      source_indices.end());
    return true;
  }

  for (size_t i = 2; i < source_indices.size(); ++i) {
    if (primitive_type == 1) {
      // Winding alternates in a strip. Preserve it even though the current
      // Metal renderer does not cull, so this stays usable by other renderers.
      if ((i & 1u) == 0) {
        triangles->push_back(source_indices[i - 2]);
        triangles->push_back(source_indices[i - 1]);
      } else {
        triangles->push_back(source_indices[i - 1]);
        triangles->push_back(source_indices[i - 2]);
      }
      triangles->push_back(source_indices[i]);
    } else {
      triangles->push_back(source_indices[0]);
      triangles->push_back(source_indices[i - 1]);
      triangles->push_back(source_indices[i]);
    }
  }
  return true;
}

std::optional<SphericalVideoMesh> ParseMesh(
    base::span<const uint8_t> mesh_data) {
  ByteReader bytes(mesh_data);

  uint32_t coordinate_count = 0;
  if (!bytes.ReadU32(&coordinate_count) ||
      (coordinate_count & 0x80000000u) != 0) {
    return std::nullopt;
  }
  if (coordinate_count == 0 || coordinate_count > kMaxCoordinateCount) {
    return std::nullopt;
  }

  std::vector<float> coordinates(coordinate_count);
  for (float& coordinate : coordinates) {
    if (!bytes.ReadFloat(&coordinate)) {
      return std::nullopt;
    }
  }

  uint32_t vertex_count = 0;
  if (!bytes.ReadU32(&vertex_count) || (vertex_count & 0x80000000u) != 0) {
    return std::nullopt;
  }
  if (vertex_count == 0 || vertex_count > kMaxVertexCount) {
    return std::nullopt;
  }

  BitReader bits(bytes.remaining());
  const uint32_t coordinate_bits = BitsForDeltaIndex(coordinate_count);
  std::array<int64_t, 5> coordinate_indices = {};
  SphericalVideoMesh mesh;
  mesh.vertices.reserve(vertex_count);

  for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
    std::array<float, 5> values = {};
    for (size_t component = 0; component < values.size(); ++component) {
      uint32_t encoded_delta = 0;
      if (!bits.ReadBits(coordinate_bits, &encoded_delta)) {
        return std::nullopt;
      }
      coordinate_indices[component] += DecodeZigZag(encoded_delta);
      if (coordinate_indices[component] < 0 ||
          coordinate_indices[component] >= coordinate_count) {
        return std::nullopt;
      }
      values[component] =
          coordinates[static_cast<size_t>(coordinate_indices[component])];
    }
    mesh.vertices.push_back(
        {values[0], values[1], values[2], values[3], values[4]});
  }

  bits.AlignToByte();
  uint32_t vertex_list_count = 0;
  if (!bits.ReadBits(32, &vertex_list_count) ||
      (vertex_list_count & 0x80000000u) != 0) {
    return std::nullopt;
  }
  if (vertex_list_count == 0 || vertex_list_count > kMaxVertexListCount) {
    return std::nullopt;
  }

  const uint32_t vertex_bits = BitsForDeltaIndex(vertex_count);
  for (uint32_t list = 0; list < vertex_list_count; ++list) {
    uint32_t texture_id = 0;
    uint32_t primitive_type = 0;
    uint32_t index_count = 0;
    if (!bits.ReadBits(8, &texture_id) ||
        !bits.ReadBits(8, &primitive_type) ||
        !bits.ReadBits(32, &index_count) ||
        (index_count & 0x80000000u) != 0) {
      return std::nullopt;
    }

    // Texture ids above zero are reserved for static auxiliary textures. The
    // Chromium media path currently has only the decoded video texture.
    if (texture_id != 0 || index_count > kMaxSourceIndexCount) {
      return std::nullopt;
    }

    std::vector<uint32_t> indices;
    indices.reserve(index_count);
    int64_t index = 0;
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t encoded_delta = 0;
      if (!bits.ReadBits(vertex_bits, &encoded_delta)) {
        return std::nullopt;
      }
      index += DecodeZigZag(encoded_delta);
      if (index < 0 || index >= vertex_count) {
        return std::nullopt;
      }
      indices.push_back(static_cast<uint32_t>(index));
    }

    if (!AppendTriangles(static_cast<uint8_t>(primitive_type), indices,
                         &mesh.triangle_indices)) {
      return std::nullopt;
    }
    bits.AlignToByte();
  }

  if (mesh.triangle_indices.empty()) {
    return std::nullopt;
  }
  return mesh;
}

std::optional<std::vector<SphericalVideoMesh>> ParseRawMeshBoxes(
    base::span<const uint8_t> data) {
  ByteReader bytes(data);
  std::vector<SphericalVideoMesh> meshes;

  while (!bytes.remaining().empty()) {
    uint32_t box_size = 0;
    uint32_t box_type = 0;
    if (!bytes.ReadU32(&box_size) || !bytes.ReadU32(&box_type) ||
        box_size < 8) {
      return std::nullopt;
    }

    base::span<const uint8_t> box_data;
    if (!bytes.ReadBytes(box_size - 8, &box_data)) {
      return std::nullopt;
    }

    if (box_type != kTypeMesh) {
      continue;
    }
    if (meshes.size() >= 2) {
      return std::nullopt;
    }

    auto mesh = ParseMesh(box_data);
    if (!mesh) {
      return std::nullopt;
    }
    meshes.push_back(std::move(*mesh));
  }

  if (meshes.empty()) {
    return std::nullopt;
  }
  return meshes;
}

}  // namespace

std::optional<std::vector<SphericalVideoMesh>> ParseSphericalVideoMesh(
    base::span<const uint8_t> projection_data) {
  // FullBox version/flags + CRC + encoding FourCC.
  if (projection_data.size() < 12 ||
      projection_data.size() > kMaxProjectionDataBytes ||
      projection_data[0] != 0) {
    return std::nullopt;
  }

  ByteReader header(projection_data);
  base::span<const uint8_t> version_and_flags;
  uint32_t crc = 0;
  uint32_t encoding = 0;
  if (!header.ReadBytes(4, &version_and_flags) || !header.ReadU32(&crc) ||
      !header.ReadU32(&encoding)) {
    return std::nullopt;
  }

  // CRC validation is intentionally not required for compatibility with
  // existing Spherical Video V2 players, but all structural fields and sizes
  // below are validated before use.
  (void)crc;

  if (encoding == kTypeRaw) {
    return ParseRawMeshBoxes(header.remaining());
  }
  if (encoding != kTypeDfl8) {
    return std::nullopt;
  }

  std::vector<uint8_t> inflated;
  if (!InflateRawDeflate(header.remaining(), &inflated)) {
    return std::nullopt;
  }
  return ParseRawMeshBoxes(inflated);
}

}  // namespace device
