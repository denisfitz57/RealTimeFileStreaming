/* 
    Real Time File Streaming copyright (c) 2014 Ross Bencina

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/
#include "FileIoReadStream.h"

#undef min
#undef max
#include <algorithm>
#include <cassert>

#include "FileIoServer.h"
#include "QwSPSCUnorderedResultQueue.h"
#include "SharedBuffer.h"
#include "DataBlock.h"

//#define IO_USE_CONSTANT_TIME_RESULT_POLLING


struct BlockRequestBehavior {

    // when not in-flight, the request type field represents the state of the block request.
    // the acquire request type is mapped to the PENDING state.
    enum BlockState {
        BLOCK_STATE_PENDING = FileIoRequest::READ_BLOCK,
        BLOCK_STATE_READY = FileIoRequest::CLIENT_USE_BASE_,
        BLOCK_STATE_MODIFIED, // not used for read streams
        BLOCK_STATE_ERROR
    };

    // L-value aliases. Map/alias request fields to our use of them.

    static FileIoRequest*& next_(FileIoRequest *r) { return r->links_[FileIoRequest::CLIENT_NEXT_LINK_INDEX]; }
    static int& state_(FileIoRequest *r) { return r->requestType; }
    static size_t& bytesCopied_(FileIoRequest *r) { return r->clientInt; }

    // predicates

    static bool isAcquireBlockRequest(FileIoRequest *r) { return (r->requestType == FileIoRequest::READ_BLOCK); }
    static bool hasDataBlock(FileIoRequest *r) { return (r->readBlock.dataBlock != 0); }

    // fields

    static bool isDiscarded(FileIoRequest *r) { return bytesCopied_(r)==-1; }
    static void setDiscarded(FileIoRequest *r) { bytesCopied_(r)=-1; }

    static size_t filePosition(FileIoRequest *r) { return r->readBlock.filePosition; }

};


struct ReadBlockRequestBehavior : public BlockRequestBehavior {

    // In abstract terms, data blocks are /acquired/ from the server and /released/ back to the server.
    // For read-only streams this means READ_BLOCK --> RELEASE_READ_BLOCK
    // For write-only streams this means ALLOCATE_WRITE_BLOCK --> (RELEASE_UNMODIFIED_WRITE_BLOCK | COMMIT_MODIFIED_WRITE_BLOCK)

    static void initAcquire( FileIoRequest *blockReq, void *fileHandle, size_t pos, FileIoRequest *resultQueueReq )
    {
        next_(blockReq) = 0;
        blockReq->resultStatus = 0;
        bytesCopied_(blockReq) = 0;
        blockReq->requestType = FileIoRequest::READ_BLOCK;

        blockReq->readBlock.fileHandle = fileHandle;
        blockReq->readBlock.filePosition = pos;
        blockReq->readBlock.dataBlock = 0;
        blockReq->readBlock.resultQueue = resultQueueReq;
    }

    static void transformToReleaseUnmodified( FileIoRequest *blockReq )
    {
        void *fileHandle = blockReq->readBlock.fileHandle;
        DataBlock *dataBlock = blockReq->readBlock.dataBlock;

        blockReq->requestType = FileIoRequest::RELEASE_READ_BLOCK;
        blockReq->releaseReadBlock.fileHandle = fileHandle;
        blockReq->releaseReadBlock.dataBlock = dataBlock;
    }

    static void transformToCommitModified( FileIoRequest *readBlockReq )
    {
        assert(false); // read blocks can't be committed, and won't be. because we never mark blocks as modified
    }


    // copyBlockData: Copy data into or out of the block. (out-of: read, in-to: write)

    enum CopyStatus { CAN_CONTINUE, AT_BLOCK_END, AT_FINAL_BLOCK_END };
    /*
        NOTE if we wanted to support items that span multiple blocks we could
        introduce an extra status: NEED_NEXT_BLOCK that we would return if the
        next block is pending but we needed data from it to copy an item.
        NEED_NEXT_BLOCK would cause the wrapper to go into the buffering state.
    */

    typedef void * user_items_ptr_t; // will be const for write streams

    // TODO: change interface to use items rather than bytes: maxItemsToCopy, itemSize, itemsCopiedResult
    static CopyStatus copyBlockData( FileIoRequest *blockReq, user_items_ptr_t userItemsPtr, size_t maxBytesToCopy, size_t itemSize, size_t *bytesCopiedResult )
    {
        size_t blockBytesCopiedSoFar = bytesCopied_(blockReq);
        size_t bytesRemainingInBlock = blockReq->readBlock.dataBlock->validCountBytes - blockBytesCopiedSoFar;

        size_t N = std::min<size_t>( bytesRemainingInBlock, maxBytesToCopy );

        std::memcpy(userItemsPtr, static_cast<int8_t*>(blockReq->readBlock.dataBlock->data)+blockBytesCopiedSoFar, N);

        bytesCopied_(blockReq) += N;

        *bytesCopiedResult = N;

        // sanity check:
        bytesRemainingInBlock -= N;
        assert( bytesRemainingInBlock == 0 || bytesRemainingInBlock >= itemSize ); // HACK: assume that itemSize divides block size, otherwise we need to deal with items overlapping blocks and that wouldn't be time-efficient

        if (bytesRemainingInBlock==0) {
            if (blockReq->readBlock.isAtEof)
                return AT_FINAL_BLOCK_END;
            else
               return AT_BLOCK_END;
        } else {
            return CAN_CONTINUE;
        }
    }
};


template< typename BlockReq >
class FileIoStreamWrapper { // Object-oriented wrapper for a read and write streams

    FileIoRequest *resultQueueReq_; // The data structure is represented by a linked structure of FileIoRequest objects

    /*
        The stream data structure is composed of linked request nodes.
        OpenFileReq is linked by the result queue's transit link. This works because
        the transit link is not used unless the RQ is posted to the server for cleanup.

             READSTREAM*
                 |
                 | (resultQueueReq_)
                 V 
         [ result queue ] -> [ OPEN_FILE ] ------------------
                 |         (openFileReq)                    |
          (head) |                                   (tail) | 
                 V                                          V
          [ READ_BLOCK ] -> [ READ_BLOCK ] -> ... -> [ READ_BLOCK ] -> NULL    } (prefetchQueue)


       "[ ... ]" indicates a FileIoRequest structure.

       For a write stream, the prefetch queue contains ALLOCATE_WRITE_BLOCK requests.
    */

    // Stream field lvalue aliases. Map/alias request fields to fields of our pseudo-class.

    FileIoRequest*& openFileReqLink_() { return resultQueueReq_->links_[FileIoRequest::TRANSIT_NEXT_LINK_INDEX]; }
    FileIoRequest* openFileReq() { return openFileReqLink_(); }
    int& state_() { return resultQueueReq_->requestType; }
    int& error_() { return resultQueueReq_->resultStatus; }
    FileIoRequest*& prefetchQueueHead_() { return resultQueueReq_->links_[FileIoRequest::CLIENT_NEXT_LINK_INDEX]; }
    FileIoRequest*& prefetchQueueTail_() { return openFileReq()->links_[FileIoRequest::CLIENT_NEXT_LINK_INDEX]; }
    size_t& waitingForBlocksCount_() { return resultQueueReq_->clientInt; }
    FileIoRequest::result_queue_t& resultQueue() { return resultQueueReq_->resultQueue; }

    FileIoStreamWrapper( FileIoRequest *resultQueueReq )
        : resultQueueReq_( resultQueueReq ) {}

    FileIoRequest* prefetchQueue_front()
    {
        return prefetchQueueHead_();
    }

    void prefetchQueue_pop_front()
    {
        FileIoRequest *x = prefetchQueueHead_();
        prefetchQueueHead_() = BlockReq::next_(x);
        BlockReq::next_(x) = 0;
    }

    void prefetchQueue_push_back( FileIoRequest *blockReq )
    {
        assert( prefetchQueueTail_() != 0 ); // doesn't deal with an empty queue, doesn't need to
        BlockReq::next_(prefetchQueueTail_()) = blockReq;
        prefetchQueueTail_() = blockReq;
    }

    void sendBlockRequestToServer( FileIoRequest *blockReq )
    {
        ::sendFileIoRequestToServer( blockReq );
        resultQueue().incrementExpectedResultCount();
        ++waitingForBlocksCount_();
    }

    // Init, link and send sequential block request
    void initLinkAndSendSequentialBlockRequest( FileIoRequest *blockReq )
    {
        // Init, link, and send a sequential data block acquire request (READ_BLOCK or ALLOCATE_WRITE_BLOCK).
        // Init the block request so that it's file position directly follows the tail block in the prefetch queue;
        // link the request onto the back of the prefetch queue; send the request to the server.

        // precondition: prefetch queue is non-empty
        assert( prefetchQueueHead_() !=0 && prefetchQueueTail_() !=0 );

        BlockReq::initAcquire( blockReq, openFileReq()->openFile.fileHandle,
                BlockReq::filePosition(prefetchQueueTail_()) + IO_DATA_BLOCK_DATA_CAPACITY_BYTES, resultQueueReq_ );

        prefetchQueue_push_back(blockReq);

        sendBlockRequestToServer(blockReq);
    }

    void flushBlock( FileIoRequest *blockReq )
    {
        switch (BlockReq::state_(blockReq))
        {
        case BlockReq::BLOCK_STATE_PENDING:
            // Forget the block, it will show up in the result queue later, 
            // and will be cleaned up from there.

            // Setting the "discarded" flag is used when the stream is still alive and we 
            // need to remove a pending request from the prefetch queue. 
            // See receiveOneBlock() for discarded block handling.
            BlockReq::setDiscarded(blockReq);
            --waitingForBlocksCount_();
            break;

        case BlockReq::BLOCK_STATE_READY:
            assert( BlockReq::hasDataBlock(blockReq) );
            BlockReq::transformToReleaseUnmodified(blockReq);
            ::sendFileIoRequestToServer( blockReq );
            break;

        case BlockReq::BLOCK_STATE_MODIFIED:
            assert( BlockReq::hasDataBlock(blockReq) );
            BlockReq::transformToCommitModified(blockReq);
            ::sendFileIoRequestToServer( blockReq );
            break;

        case BlockReq::BLOCK_STATE_ERROR:
            assert( !BlockReq::hasDataBlock(blockReq) );
            freeFileIoRequest( blockReq );
            break;
        }
    }

    void flushPrefetchQueue()
    {
        // For each block in the prefetch queue, pop the block from the 
        // head of the queue and clean up the block.
        while (prefetchQueueHead_()) {
            FileIoRequest *blockReq = prefetchQueue_front();
            prefetchQueue_pop_front();
            flushBlock( blockReq );
        }

        prefetchQueueTail_() = 0;
        assert( waitingForBlocksCount_() == 0 );
    }

    static size_t roundDownToBlockSizeAlignedPosition( size_t pos )
    {
        size_t blockNumber = pos / IO_DATA_BLOCK_DATA_CAPACITY_BYTES;
        return blockNumber * IO_DATA_BLOCK_DATA_CAPACITY_BYTES;
    }

    // Should only be called after the stream has been opened and before it is closed.
    // returns true if a block was processed.
    bool receiveOneBlock()
    {
        if (FileIoRequest *r=resultQueue().pop()) {
            assert( BlockReq::isAcquireBlockRequest(r) );

            if (BlockReq::isDiscarded(r)) {
                // the block was discarded. i.e. is no longer in the prefetch block queue

                if (r->resultStatus==NOERROR) {
                    BlockReq::transformToReleaseUnmodified(r);
                    ::sendFileIoRequestToServer(r);
                } else {
                    assert( BlockReq::hasDataBlock(r) );
                    ::freeFileIoRequest(r);
                    // (errors on discarded blocks don't affect the stream state)
                }
                // (discarded blocks do not count against waitingForBlocksCount_

            } else {
                if (--waitingForBlocksCount_() == 0)
                    state_() = STREAM_STATE_OPEN_STREAMING;

                if (r->resultStatus==NOERROR) {
                    assert( BlockReq::hasDataBlock(r) );
                    BlockReq::state_(r) = BlockReq::BLOCK_STATE_READY;
                } else {
                    // Mark the request as in ERROR. The stream state will switch to ERROR when the client tries to read/write the block.
                    BlockReq::state_(r) = BlockReq::BLOCK_STATE_ERROR;
                }
            }

            return true;
        }

        return false;
    }

public:
    FileIoStreamWrapper( READSTREAM *fp )
        : resultQueueReq_( static_cast<FileIoRequest*>(fp) ) {}
    
    static READSTREAM* open( SharedBuffer *path, FileIoRequest::OpenMode openMode )
    {
        // Allocate two requests. Return 0 if allocation fails.

        FileIoRequest *resultQueueReq = allocFileIoRequest();
        if (!resultQueueReq)
            return 0;

        FileIoRequest *openFileReq = allocFileIoRequest();
        if (!openFileReq) {
            freeFileIoRequest(resultQueueReq);
            return 0;
        }

        // Initialise the stream data structure

        FileIoReadStreamWrapper stream(resultQueueReq);
        //std::memset( &resultQueueReq, 0, sizeof(FileIoRequest) );
        stream.resultQueue().init();

        stream.openFileReqLink_() = openFileReq;
        stream.state_() = STREAM_STATE_OPENING;
        stream.error_() = 0;
        stream.prefetchQueueHead_() = 0;
        stream.prefetchQueueTail_() = 0;
        stream.waitingForBlocksCount_() = 0;
        
        // Issue the OPEN_FILE request

        openFileReq->resultStatus = 0;
        openFileReq->requestType = FileIoRequest::OPEN_FILE;
        path->addRef();
        openFileReq->openFile.path = path;
        openFileReq->openFile.openMode = openMode;
        openFileReq->openFile.fileHandle = IO_INVALID_FILE_HANDLE;
        openFileReq->openFile.resultQueue = resultQueueReq;

        ::sendFileIoRequestToServer(openFileReq);
        stream.resultQueue().incrementExpectedResultCount();

        return static_cast<READSTREAM*>(resultQueueReq);
    }

    void close()
    {
        // (Don't poll state, just dispose current state)

        if (state_()==STREAM_STATE_OPENING) {
            // Still waiting for OPEN_FILE to return. Send the result queue to the server for cleanup.
            openFileReqLink_() = 0;

            resultQueueReq_->requestType = FileIoRequest::CLEANUP_RESULT_QUEUE;
            ::sendFileIoRequestToServer(resultQueueReq_);
            resultQueueReq_ = 0;
        } else {
            // Stream is open. The prefetch queue may contain requests.

            // Dispose the prefetch queue, if it's populated

            flushPrefetchQueue();

            // Clean up the open file request

            {
                FileIoRequest *openFileReq = openFileReqLink_();
                openFileReqLink_() = 0;
                if (openFileReq->openFile.fileHandle != IO_INVALID_FILE_HANDLE) {
                    // Transform openFileReq to CLOSE_FILE and send to server
                    void *fileHandle = openFileReq->openFile.fileHandle;
                    
                    FileIoRequest *closeFileReq = openFileReq;
                    closeFileReq->requestType = FileIoRequest::CLOSE_FILE;
                    closeFileReq->closeFile.fileHandle = fileHandle;
                    ::sendFileIoRequestToServer(closeFileReq);
                } else {
                    freeFileIoRequest(openFileReq);
                }
            }

            // Clean up the result queue

            if (resultQueue().expectedResultCount() > 0) {
                // Send the result queue to the server for cleanup
                resultQueueReq_->requestType = FileIoRequest::CLEANUP_RESULT_QUEUE;
                ::sendFileIoRequestToServer(resultQueueReq_);
                resultQueueReq_ = 0;
            } else {
                freeFileIoRequest(resultQueueReq_);
                resultQueueReq_ = 0;
            }
        }
    }

    int seek( size_t pos )
    {
        assert( pos >= 0 );

        if (state_() == STREAM_STATE_OPENING || state_() == STREAM_STATE_ERROR)
            return -1;

        // Straight-forward implementation of seek: dump all blocks from the prefetch queue, 
        // then request the needed blocks.
        // A more optimised version would retain any needed blocks from the current prefetch queue.

        flushPrefetchQueue();

        // FIXME HACK: hardcode prefetch queue length. 
        // The prefetch queue length should be computed from the stream data rate and the 
        // desired prefetch buffering length (in seconds).
        
        const int prefetchQueueBlockCount = 20;

        // Request blocks on block-size-aligned boundaries
        size_t blockFilePositionBytes = roundDownToBlockSizeAlignedPosition(pos);

        // request the first block 

        FileIoRequest *firstBlockReq = allocFileIoRequest();
        if (!firstBlockReq) {
            state_() = STREAM_STATE_ERROR;
            return -1;
        }

        BlockReq::initAcquire( firstBlockReq, openFileReq()->openFile.fileHandle, blockFilePositionBytes, resultQueueReq_ );

        BlockReq::bytesCopied_(firstBlockReq) = pos - blockFilePositionBytes; // compensate for block-size-aligned request
        
        prefetchQueueHead_() = firstBlockReq;
        prefetchQueueTail_() = firstBlockReq;

        // FIXME TODO: queue all requests at once with enqueue-multiple
        // Use push-multiple when performing a seek operation. That minimises contention.
        // TODO: also use queue multiple for closing the stream

        sendBlockRequestToServer(firstBlockReq);

        for (int i=1; i < prefetchQueueBlockCount; ++i) {
            FileIoRequest *readBlockReq = allocFileIoRequest();
            if (!readBlockReq) {
                // fail. couldn't allocate request
                state_() = STREAM_STATE_ERROR;
                return -1;
            }

            initLinkAndSendSequentialBlockRequest( readBlockReq );
        }

        state_() = STREAM_STATE_OPEN_BUFFERING;

        return 0;
    }

    typedef typename BlockReq::user_items_ptr_t user_items_ptr_t;

    size_t read_or_write( user_items_ptr_t userItemsPtr, size_t itemSize, size_t itemCount ) // for a read stream this is read(), for a write stream this is write()
    {
        // Always process at least one expected reply per read/write call.
        pollState(); // Updates state based on received replies. e.g. from BUFFERING to STREAMING

        switch (state_())
        {
        case STREAM_STATE_OPENING:
        case STREAM_STATE_OPEN_IDLE:
        case STREAM_STATE_OPEN_EOF:
        case STREAM_STATE_ERROR:
            return 0;
            break;

        case STREAM_STATE_OPEN_BUFFERING:
            {
#if defined(IO_USE_CONSTANT_TIME_RESULT_POLLING)
                return 0; // we're BUFFERING. output nothing
#else
                // The call to pollState() above only deals with at most one pending buffer.
                // Try to transition from BUFFERING to STREAMING as quickly as possible by draining the result queue.
                // This is O(N) in the number of expected results.
                
                while (receiveOneBlock()) 
                    /* loop until all replies have been processed */ ;

                if (state_() != STREAM_STATE_OPEN_STREAMING)
                    return 0;
                /* FALLS THROUGH */
#endif         
            }  

        case STREAM_STATE_OPEN_STREAMING:
            {
                // TODO: switch to working in whole numbers of items here

                int8_t *userBytesPtr = (int8_t*)userItemsPtr;
                size_t totalBytesToCopy = itemSize * itemCount;
                size_t bytesCopiedSoFar = 0;

                while (bytesCopiedSoFar < totalBytesToCopy) {
                    FileIoRequest *frontBlockReq = prefetchQueue_front();
                    assert( frontBlockReq != 0 );

#if !defined(IO_USE_CONSTANT_TIME_RESULT_POLLING)
                    // Last-ditch effort to determine whether the front block has been returned.
                    // O(n) in the maximum number of expected replies.
                    // Since we always poll at least one block per read/write operation (call to 
                    // pollState() above), the following loop is not strictly necessary.
                    // It serves two purposes: (1) it reduces the latency of transitioning from 
                    // BUFFERING to STREAMING, (2) it lessens the likelihood of a buffer underrun. 

                    // Process replies until the front block is not pending or there are no more replies
                    while (BlockReq::state_(frontBlockReq) == BlockReq::BLOCK_STATE_PENDING) {
                        if (!receiveOneBlock())
                            break;
                    }
#endif

                    if (BlockReq::state_(frontBlockReq) == BlockReq::BLOCK_STATE_READY) {
                        // copy data to/from userItemsPtr and the front block in the prefetch queue

                        size_t bytesRemainingToCopy = totalBytesToCopy - bytesCopiedSoFar;

                        size_t bytesCopied = 0;
                        BlockReq::CopyStatus copyStatus = BlockReq::copyBlockData(frontBlockReq, userBytesPtr, bytesRemainingToCopy, itemSize, &bytesCopied);

                        userBytesPtr += bytesCopied;
                        bytesCopiedSoFar += bytesCopied;

                        switch (copyStatus) {
                        case BlockReq::AT_BLOCK_END:
                            {
                                // request and link the next block...
                                FileIoRequest *readBlockReq = allocFileIoRequest();
                                if (!readBlockReq) {
                                    // fail. couldn't allocate request
                                    state_() = STREAM_STATE_ERROR;
                                    return bytesCopiedSoFar / itemSize;
                                }

                                // issue next block request, link it on to the tail of the prefetch queue
                                initLinkAndSendSequentialBlockRequest( readBlockReq );

                                // unlink the old block...

                                // Notice that we link the new request on the back of the prefetch queue before unlinking 
                                // the old one off the front, so there is no chance of having to deal with the special case of 
                                // linking to an empty queue.

                                prefetchQueue_pop_front(); // advance head to next block
                                flushBlock( frontBlockReq ); // send the old block back to the server

                                // try to receive one of the blocks requested earlier...
                                receiveOneBlock();
                            }
                            break;
                        case BlockReq::AT_FINAL_BLOCK_END:
                            state_() = STREAM_STATE_OPEN_EOF;
                            return bytesCopiedSoFar / itemSize;
                            break;
                        case BlockReq::CAN_CONTINUE:
                            /* NOTHING */
                            break;
                        }
                    } else {
                        if (BlockReq::state_(frontBlockReq) == BlockReq::BLOCK_STATE_ERROR)
                            state_() = STREAM_STATE_ERROR;
                        else
                            state_() = STREAM_STATE_OPEN_BUFFERING;

                        // head block is pending, or we've entered the error state
                        return bytesCopiedSoFar / itemSize;
                    }
                }

                return itemCount;
            }
            break;
        }

        return 0;
    }

    FileIoReadStreamState pollState()
    {
        if (resultQueue().expectedResultCount() > 0) {
            if (state_()==STREAM_STATE_OPENING) {
                if (FileIoRequest *r=resultQueue().pop()) {
                    assert( r == openFileReq() ); // When opening, the only possible result is the open file request.
                    
                    r->openFile.path->release();
                    r->openFile.path = 0;

                    if (r->resultStatus==NOERROR) {
                        assert( r->openFile.fileHandle != 0 );

                        state_() = STREAM_STATE_OPEN_IDLE;
                        // NOTE: In principle we could seek here. at the moment we require the client to poll for idle.
                    } else {
                        error_() = r->resultStatus;
                        state_() = STREAM_STATE_ERROR;
                    }

                    // Leave openFileReq linked to the structure, even if there's an error.
                }
            } else {
                assert( state_()==STREAM_STATE_OPEN_IDLE 
                    || state_()==STREAM_STATE_OPEN_EOF
                    || state_()==STREAM_STATE_OPEN_BUFFERING 
                    || state_()==STREAM_STATE_OPEN_STREAMING
                    || state_()==STREAM_STATE_ERROR );

                receiveOneBlock();
            }
        }

        return (FileIoReadStreamState)state_();
    }

    int getError()
    {
        return error_();
    }
};

typedef FileIoStreamWrapper<ReadBlockRequestBehavior> FileIoReadStreamWrapper;


READSTREAM *FileIoReadStream_open( SharedBuffer *path, FileIoRequest::OpenMode openMode )
{
    return FileIoReadStreamWrapper::open(path, openMode);
}

void FileIoReadStream_close( READSTREAM *fp )
{
    FileIoReadStreamWrapper(fp).close();
}

int FileIoReadStream_seek( READSTREAM *fp, size_t pos )
{
    return FileIoReadStreamWrapper(fp).seek(pos);
}

size_t FileIoReadStream_read( void *dest, size_t itemSize, size_t itemCount, READSTREAM *fp )
{
    return FileIoReadStreamWrapper(fp).read_or_write(dest, itemSize, itemCount);
}

FileIoReadStreamState FileIoReadStream_pollState( READSTREAM *fp )
{
    return FileIoReadStreamWrapper(fp).pollState();
}

int FileIoReadStream_getError( READSTREAM *fp )
{
    return FileIoReadStreamWrapper(fp).getError();
}


void FileIoReadStream_test()
{
    printf( "> FileIoReadStream_test()\n" );

    printf( "opening " );

    // in msvc this is a path relative to the project directory:
    SharedBuffer *path = SharedBufferAllocator::alloc("..\\..\\..\\src\\FileIoReadStream.cpp"); // print out the source code of this file
    READSTREAM *fp = FileIoReadStream_open(path, FileIoRequest::READ_ONLY_OPEN_MODE);
    path->release();
    assert( fp != 0 );

    while (FileIoReadStream_pollState(fp) == STREAM_STATE_OPENING) {
        printf(".");
        Sleep(10);
    }

    printf( "\ndone.\n" );

    assert( FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_IDLE );

    printf( "seeking " );

    FileIoReadStream_seek(fp, 0);

    while (FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_BUFFERING) {
        printf(".");
        Sleep(10);
    }
    
    assert( FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_STREAMING );

    printf( "\ndone.\n" );

    printf( "reading:\n" );

    while (FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_STREAMING || FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_BUFFERING) {

        /*
        // make sure we're always streaming:
        while (FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_BUFFERING) {
            printf(".");
            Sleep(10);
        }
        */

        char c[512];
        size_t bytesToRead = rand() & 0xFF;
        size_t bytesRead = FileIoReadStream_read( c, 1, bytesToRead, fp );
        if (bytesRead>0) {
            fwrite(c, 1, bytesRead, stdout);
        }
    }

    assert( FileIoReadStream_pollState(fp) == STREAM_STATE_OPEN_EOF );

    printf( "\nclosing.\n" );

    FileIoReadStream_close(fp);

    printf( "< FileIoReadStream_test()\n" );
}
