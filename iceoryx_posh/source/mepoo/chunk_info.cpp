// Copyright (c) 2019 by Robert Bosch GmbH. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iceoryx_posh/mepoo/chunk_info.hpp"
#include "iceoryx_posh/internal/mepoo/mem_pool.hpp"

namespace iox
{
namespace mepoo
{
ChunkInfo::ChunkInfo() noexcept
    : m_payload(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) + sizeof(ChunkInfo)))
{
}

ChunkInfo* convertPayloadPointerToChunkInfo(void* const payload) noexcept
{
    return reinterpret_cast<ChunkInfo*>(reinterpret_cast<uintptr_t>(payload) - sizeof(ChunkInfo));
}

} // namespace mepoo
} // namespace iox
