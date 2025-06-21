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
#ifndef D_BINARY_STREAM_H
#define D_BINARY_STREAM_H

#include "common.h"
#include "Buffer.h"

#include <unistd.h>

namespace aria2 {

class BinaryStream {
public:
  virtual ~BinaryStream() = default;

  // Primary interface using shared buffers for async I/O
  virtual void writeData(Buffer buffer, size_t bufferOffset, size_t length,
                        int64_t fileOffset) = 0;

  virtual ssize_t readData(Buffer buffer, size_t bufferOffset, size_t length,
                          int64_t fileOffset) = 0;

  // Truncates a file to given length. The default implementation does
  // nothing.
  virtual void truncate(int64_t length) {}

  // Allocates given length bytes of disk space from given offset. The
  // default implementation does nothing. If sparse is true, the
  // implementation may create sparse file (with holes).
  virtual void allocate(int64_t offset, int64_t length, bool sparse) {}

  // Legacy compatibility interface - 3 parameter writeData
  virtual void writeData(const unsigned char* data, size_t length, int64_t fileOffset) {
    auto buffer = buffer::copy(data, length);
    writeData(buffer, 0, length, fileOffset);
  }

  // Legacy compatibility interface - 3 parameter readData  
  virtual ssize_t readData(unsigned char* data, size_t length, int64_t fileOffset) {
    auto buffer = buffer::create(length);
    auto result = readData(buffer, 0, length, fileOffset);
    if (result > 0) {
      std::copy_n(buffer->data(), result, data);
    }
    return result;
  }
};

} // namespace aria2

#endif // D_BINARY_STREAM_H
