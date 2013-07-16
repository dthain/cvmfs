#include "processor.h"

#include <cstdio>

#include <tbb/parallel_invoke.h>

#include "chunk.h"
#include "io_dispatcher.h"
#include "file.h"

void ChunkHasher::Crunch(Chunk                *chunk,
                         const unsigned char  *data,
                         const size_t          bytes,
                         const bool            finalize) {
  const int upd_rc = SHA1_Update(&chunk->sha1_context(), data, bytes);
  assert (upd_rc == 1);

  if (finalize) {
    const int fin_rc = SHA1_Final(chunk->sha1_digest(),
                                  &chunk->sha1_context());
    assert (fin_rc == 1);
  }
}


void ChunkCompressor::Crunch(Chunk                *chunk,
                             const unsigned char  *data,
                             const size_t          bytes,
                             const bool            finalize) {
  z_stream &stream = chunk->zlib_context();

  const size_t max_output_size = deflateBound(&stream, bytes);
  CharBuffer *compress_buffer  = new CharBuffer(max_output_size);

  stream.avail_in  = bytes;
  stream.next_in   = const_cast<unsigned char*>(data); // sry, but zlib forces me...
  const int flush = (finalize) ? Z_FINISH : Z_NO_FLUSH;

  int retcode = -1;
  bool done = false;
  while (true) {
    stream.avail_out = compress_buffer->size();
    stream.next_out  = compress_buffer->ptr();

    retcode = deflate(&stream, flush);
    assert (retcode == Z_OK || retcode == Z_STREAM_END);

    const size_t bytes_produced = compress_buffer->size() - stream.avail_out;
    compress_buffer->SetUsedBytes(bytes_produced);
    compress_buffer->SetBaseOffset(chunk->compressed_size());
    chunk->add_compressed_size(bytes_produced);
    chunk->ScheduleWrite(compress_buffer);

    if ((flush == Z_NO_FLUSH && retcode == Z_OK) ||
        (flush == Z_FINISH   && retcode == Z_STREAM_END)) {
      break;
    }

    if (stream.avail_out == 0) {
      compress_buffer = new CharBuffer(32768);
    }
  }

  if (finalize) {
    assert (flush == Z_FINISH);
    assert (stream.avail_in == 0);

    retcode = deflateEnd(&stream);
    assert (retcode == Z_OK);
  }
}



tbb::task* ChunkProcessingTask::execute() {
  assert (chunk_->IsInitialized());
  assert (buffer_->IsInitialized());

  const off_t internal_offset =
    std::max(off_t(0), chunk_->offset() - buffer_->base_offset());
  assert (internal_offset < buffer_->used_bytes());
  const unsigned char *data = buffer_->ptr() + internal_offset;

  const size_t byte_count = (chunk_->size() == 0)
    ? buffer_->used_bytes()
    :   std::min(buffer_->base_offset()  + buffer_->used_bytes(),
                 chunk_->offset()         + chunk_->size())
      - std::max(buffer_->base_offset(), chunk_->offset());
  assert (byte_count <= buffer_->used_bytes() - internal_offset);

  const bool finalize = (
    (chunk_->size() > 0) &&
    (buffer_->base_offset() + internal_offset + byte_count
      == chunk_->offset() + chunk_->size())
  );

  ChunkCruncher<ChunkHasher>     hasher    (chunk_,
                                            buffer_,
                                            internal_offset,
                                            byte_count,
                                            finalize);
  ChunkCruncher<ChunkCompressor> compressor(chunk_,
                                            buffer_,
                                            internal_offset,
                                            byte_count,
                                            finalize);

  tbb::parallel_invoke(hasher, compressor);

  if (finalize) {
    chunk_->Done();
    io_dispatcher_->ScheduleCommit(chunk_);
  }

  return NULL;
}



tbb::task* FileScrubbingTask::execute() {
  // find chunk cut marks in the current buffer and process all chunks that
  // are fully specified (i.e. not reaching beyond the current buffer)
  const CutMarks cut_marks = FindNextChunkCutMarks();
  CutMarks::const_iterator i    = cut_marks.begin();
  CutMarks::const_iterator iend = cut_marks.end();
  for (; i != iend; ++i) {
    Chunk *fully_defined_chunk = file_->CreateNextChunk(*i);
    assert (fully_defined_chunk->size() > 0);
    Process(fully_defined_chunk);
  }

  // check if the file has a bulk chunk and continue processing it using the
  // current buffer
  if (file_->HasBulkChunk()) {
    Process(file_->bulk_chunk());
  }

  // if we reached the last buffer this input file will produce all but the
  // last created chunk will be fully defined at this point
  if (IsLastBuffer()) {
    file_->FinalizeLastChunk();
  }

  // process the current chunk, i.e. the last created chunk that potentially
  // reaches beyond the current buffer or to the end of the file
  Process(file_->current_chunk());

  // wait for all scheduled chunk processing on the current buffer to be
  // finished and get rid of the buffer
  WaitForProcessing();
  delete buffer_;

  // if the last buffer was processed, we finalize the whole file
  // (Note: this does not mean, that the IoDispatcher already uploaded all
  //        chunk data)
  if (IsLastBuffer()) {
    file_->Finalize();
  }

  // go on with the next file buffer
  return Next();
}


bool FileScrubbingTask::IsLastBuffer() const {
  return file_->size() == buffer_->base_offset() + buffer_->used_bytes();
}

