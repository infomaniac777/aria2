/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#ifndef D_BUFFER_H
#include <algorithm>
#define D_BUFFER_H

#include <memory>
#include <vector>
#include <algorithm>

namespace aria2 {

// Core buffer type for aria2 - shared ownership of byte data
using Buffer = std::shared_ptr<std::vector<unsigned char>>;

// Buffer creation utilities
namespace buffer {

// Create a new buffer of specified size
inline Buffer create(size_t size) {
  return std::make_shared<std::vector<unsigned char>>(size);
}

// Create buffer from existing data (copies the data)
inline Buffer copy(const unsigned char* data, size_t length) {
  return std::make_shared<std::vector<unsigned char>>(data, data + length);
}

// Create buffer from existing data (copies the data) - convenience for char*
inline Buffer copy(const char* data, size_t length) {
  const auto* udata = reinterpret_cast<const unsigned char*>(data);
  return copy(udata, length);
}

// Create empty buffer
inline Buffer createEmpty() {
  return std::make_shared<std::vector<unsigned char>>();
}

// Resize buffer to actual used size (useful after network receive)
inline void resize(Buffer buffer, size_t newSize) {
  buffer->resize(newSize);
}

// Get data pointer with offset
inline unsigned char* data(Buffer buffer, size_t offset = 0) {
  return buffer->data() + offset;
}

// Get const data pointer with offset
inline const unsigned char* cdata(Buffer buffer, size_t offset = 0) {
  return buffer->data() + offset;
}

// Get buffer size
inline size_t size(Buffer buffer) {
  return buffer->size();
}

// Check if buffer is valid
inline bool isValid(Buffer buffer) {
  return buffer && !buffer->empty();
}

// Create a slice/view of an existing buffer (shares the underlying data)
// Uses shared_ptr aliasing constructor for zero-copy slicing
inline Buffer slice(Buffer buffer, size_t offset, size_t length) {
  if (!buffer || offset >= buffer->size()) {
    return createEmpty();
  }
  
  // Clamp length to available data
  size_t availableLength = buffer->size() - offset;
  size_t actualLength = std::min(length, availableLength);
  
  if (actualLength == 0) {
    return createEmpty();
  }
  
  // Create new vector sharing the same data
  auto sliceVec = std::make_shared<std::vector<unsigned char>>(
    buffer->begin() + offset, 
    buffer->begin() + offset + actualLength
  );
  
  return sliceVec;
}

// Append data to an existing buffer
inline void append(Buffer buffer, const unsigned char* data, size_t length) {
  if (!buffer) return;
  size_t oldSize = buffer->size();
  buffer->resize(oldSize + length);
  std::copy_n(data, length, buffer->data() + oldSize);
}

// Append one buffer to another
inline void append(Buffer dest, Buffer src) {
  if (!dest || !src) return;
  append(dest, src->data(), src->size());
}

} // namespace buffer

} // namespace aria2

#endif // D_BUFFER_H 