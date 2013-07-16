#ifndef FILE_H
#define FILE_H

#include <string>
#include <vector>

#include "../cvmfs/platform.h"

class IoDispatcher;
class Chunk;
typedef std::vector<Chunk*> ChunkVector;



static const size_t kMinChunkSize = 2 * 1024 * 1024;;
static const size_t kAvgChunkSize = 4 * 1024 * 1024;;
static const size_t kMaxChunkSize = 6 * 1024 * 1024;;



class File {
 public:
  File(const std::string      &path,
       const platform_stat64  &info,
       IoDispatcher           *io_dispatcher) :
    path_(path), size_(info.st_size),
    might_become_chunked_(size_ > kMinChunkSize),
    bulk_chunk_(NULL), current_chunk_(NULL),
    io_dispatcher_(io_dispatcher)
  {
    CreateNextChunk(0);
  }

  ~File();

  /**
   * This creates a next chunk which will be the successor of the current chunk
   *
   * @param   offset  the offset at which the new chunk should be created
   *                  Note: this implictly defines the size of the current chunk
   * @return  the predecessor (!) of the just created chunk
   */
  Chunk* CreateNextChunk(const off_t offset);

  /**
   * This will fix the current_chunk_ as being the last chunk of the file.
   *
   * @return  the finalized chunk
   */
  Chunk* FinalizeLastChunk();
  void Finalize();

  bool HasBulkChunk()          const { return bulk_chunk_ != NULL; }

  size_t size()                const { return size_;               }
  const std::string& path()    const { return path_;               }

        Chunk* current_chunk()       { return current_chunk_;      }
  const Chunk* current_chunk() const { return current_chunk_;      }

        Chunk* bulk_chunk()          { return bulk_chunk_;         }
  const Chunk* bulk_chunk()    const { return bulk_chunk_;         }
  const ChunkVector& chunks()  const { return chunks_;             }

 protected:
  void CreateBulkChunk();

 private:
  const std::string  path_;
  const size_t       size_;
  const bool         might_become_chunked_;

  ChunkVector        chunks_;

  Chunk              *bulk_chunk_;
  Chunk              *current_chunk_;

  IoDispatcher       *io_dispatcher_;
};

#endif /* FILE_H */
