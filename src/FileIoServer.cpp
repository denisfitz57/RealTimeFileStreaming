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
#include "FileIoServer.h"

///
#define NOMINMAX // suppress windows.h min/max
#include <Windows.h>
#include <process.h>
#include <errno.h>
///

#include "QwMPSCFifoQueue.h"
#include "QwNodePool.h"
#include "QwSPSCUnorderedResultQueue.h"
#include "SharedBuffer.h"
#include "DataBlock.h"

#include "FileIoRequest.h"

///////////////////////////////////////////////////////////////////////////////
// FileIoRequest allocation

static QwNodePool<FileIoRequest> *globalRequestPool_ = 0; // managed by startFileIoServer/shutDownFileIoServer

FileIoRequest *allocFileIoRequest()
{
    return globalRequestPool_->allocate();
}

void freeFileIoRequest( FileIoRequest *r )
{
    globalRequestPool_->deallocate(r);
}

///////////////////////////////////////////////////////////////////////////////
// Server thread routines

static void cleanupOneRequestResult( FileIoRequest *r ); // forward reference

static void completeRequestToClientResultQueue( FileIoRequest *clientResultQueueContainer, FileIoRequest *r )
{
    // Poll the state of the result queue *before* posting result back to client
    // because if the result queue is not being cleaned up the server doesn't own it
    // and can't use it after returning the result.
    bool resultQueueIsAwaitingCleanup = (clientResultQueueContainer->requestType == FileIoRequest::RESULT_QUEUE_IS_AWAITING_CLEANUP_);

    if (resultQueueIsAwaitingCleanup) {
        // Option A:
        cleanupOneRequestResult(r);
        if (clientResultQueueContainer->resultQueue.expectedResultCount() == 0)
            freeFileIoRequest(clientResultQueueContainer);
        
        /*
        // Option B:
        // the potential advantage of doing it this way is that the cleanup is deferred until
        // after all currently pending requests have been handled. this might reduce request 
        // handling latency slightly, at the expense of leaving unused resources allocated longer.
        pushResultToResultQueue(clientResultQueueContainer, r);

        // schedule the result queue for further cleanup
        clientResultQueueContainer->requestType = FileIoRequest::CLEANUP_RESULT_QUEUE;
        pushServerInternalFileIoRequest( clientResultQueueContainer );
        */

    } else {
        clientResultQueueContainer->resultQueue.push(r);
    }
}

///

static DataBlock* allocDataBlock()
{
    DataBlock *result = new DataBlock;
    assert( result );
    result->capacityBytes = IO_DATA_BLOCK_DATA_CAPACITY_BYTES;
    result->validCountBytes = 0;
    result->data = new int8_t[ IO_DATA_BLOCK_DATA_CAPACITY_BYTES ];
    assert( result->data );
    return result;
}

static void freeDataBlock( DataBlock *b )
{
    delete [] (int8_t*)b->data;
    delete b;
}

namespace {
    struct FileRecord{
        FILE *fp;
        int dependentClientCount;
    };
} // end anonymous namespace

static void handleOpenFileRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::OPEN_FILE );

    FileRecord *fileRecord = new FileRecord;
    if (fileRecord) {
        const char *fopenMode = (r->openFile.openMode == FileIoRequest::READ_WRITE_OVERWRITE_OPEN_MODE)
            ? "wb+"
            : "rb"; // default to read-only

        if (FILE *fp = std::fopen(r->openFile.path->data, fopenMode)) {
            fileRecord->fp = fp;
            fileRecord->dependentClientCount = 1;
            r->openFile.fileHandle = fileRecord;
            r->resultStatus = NOERROR;
        } else {
            delete fileRecord;
            r->openFile.fileHandle = 0;
            r->resultStatus = (errno==NOERROR) ? EIO : errno;
        }
    } else {
        r->openFile.fileHandle = 0;
        r->resultStatus = ENOMEM;
    }
    
    completeRequestToClientResultQueue(r->openFile.resultQueue, r);
}

static void releaseFileRecordClientRef( FileRecord *fileRecord )
{
    if (--fileRecord->dependentClientCount == 0) {
        std::fclose(fileRecord->fp);
        delete fileRecord;
    }
}

static void handleCloseFileRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::CLOSE_FILE );

    releaseFileRecordClientRef( static_cast<FileRecord*>(r->closeFile.fileHandle) );
    freeFileIoRequest(r);
}

static void handleReadBlockRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::READ_BLOCK );
    // Allocate the block, perform the fread, return the block to the client, 
    // increment the file record dependent count.

    FileRecord *fileRecord = static_cast<FileRecord*>(r->readBlock.fileHandle);
    if (fileRecord) {
        DataBlock *dataBlock = allocDataBlock();
        if (dataBlock) {

            // FIXME: we're only supporting 32 bit file positions here
            if (std::fseek(fileRecord->fp, r->readBlock.filePosition, SEEK_SET) != 0) {
                // seek failed
                r->resultStatus = (errno==NOERROR) ? EIO : errno;
                r->readBlock.dataBlock = 0;
                r->readBlock.isAtEof = false;
                freeDataBlock(dataBlock);
                dataBlock = 0;
            }else{

                dataBlock->validCountBytes = std::fread(dataBlock->data, 1, dataBlock->capacityBytes, fileRecord->fp);
            
                if (dataBlock->validCountBytes==dataBlock->capacityBytes) {
                    // normal case: read a whole block
                    r->resultStatus = NOERROR;
                    r->readBlock.dataBlock = dataBlock; // return the block
                    r->readBlock.isAtEof = false;
                } else {
                    // only return a partial block *if* we're at EOF, otherwise free the block and return an error.
                
                    if (feof(fileRecord->fp) != 0) {
                        // Note: this may return a block with zero valid bytes. Could maybe optimise this away.
                        r->resultStatus = NOERROR;
                        r->readBlock.dataBlock = dataBlock; // return the block
                        r->readBlock.isAtEof = true;
                    } else {
                        r->resultStatus = (errno==NOERROR) ? EIO : errno;
                        r->readBlock.dataBlock = 0;
                        r->readBlock.isAtEof = false;
                        freeDataBlock(dataBlock);
                        dataBlock = 0;
                    }
                }
            }
        }else{
            r->resultStatus = ENOMEM;
        }
    }else{
        r->resultStatus = EBADF;
    }

    if (r->readBlock.dataBlock)
        ++fileRecord->dependentClientCount;

    completeRequestToClientResultQueue(r->readBlock.resultQueue, r);
}

static void handleReleaseReadBlockRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::RELEASE_READ_BLOCK );
    // Free the data block, decrement file record dependent client count

    assert( r->releaseReadBlock.dataBlock != 0 );
    freeDataBlock(r->releaseReadBlock.dataBlock);
    releaseFileRecordClientRef( static_cast<FileRecord*>(r->releaseReadBlock.fileHandle) );
    freeFileIoRequest(r);
}

static void handleAllocateWriteBlockRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::ALLOCATE_WRITE_BLOCK );
    // Allocate the block, read existing data (if any), return the block to the client, increment the file record dependent count
    
    FileRecord *fileRecord = static_cast<FileRecord*>(r->allocateWriteBlock.fileHandle);

    DataBlock *dataBlock = allocDataBlock();
    if (dataBlock) {
        // FIXME: we're only supporting 32 bit file positions here
        if (std::fseek(fileRecord->fp, r->allocateWriteBlock.filePosition, SEEK_SET)==0) {
            dataBlock->validCountBytes = std::fread(dataBlock->data, 1, dataBlock->capacityBytes, fileRecord->fp);
        }else{
            // couldn't seek to position, return data block with no valid data
        }
    }

    r->allocateWriteBlock.dataBlock = dataBlock;
    completeRequestToClientResultQueue(r->allocateWriteBlock.resultQueue, r);
    ++fileRecord->dependentClientCount;
}

static void handleCommitModifiedWriteBlockRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::COMMIT_MODIFIED_WRITE_BLOCK );
    // Write valid data to the file, free the data block, decrement file record dependent client count

    FileRecord *fileRecord = static_cast<FileRecord*>(r->commitModifiedWriteBlock.fileHandle);

    // FIXME: we're only supporting 32 bit file positions here
    if (std::fseek(fileRecord->fp, r->commitModifiedWriteBlock.filePosition, SEEK_SET)==0) {
        DataBlock *dataBlock = r->commitModifiedWriteBlock.dataBlock;
        std::fwrite(dataBlock->data, 1, dataBlock->validCountBytes, fileRecord->fp); // silently ignore errors
    }else{
        // couldn't seek to position, silently fail
    }
   
    freeDataBlock(r->commitModifiedWriteBlock.dataBlock);
    releaseFileRecordClientRef( static_cast<FileRecord*>(r->commitModifiedWriteBlock.fileHandle) );
    freeFileIoRequest(r);
}

static void handleReleaseUnmodifiedWriteBlockRequest( FileIoRequest *r )
{
    assert( r->requestType == FileIoRequest::RELEASE_UNMODIFIED_WRITE_BLOCK );
    // Free the data block, decrement file record dependent client count

    freeDataBlock(r->releaseUnmodifiedWriteBlock.dataBlock);
    releaseFileRecordClientRef( static_cast<FileRecord*>(r->releaseUnmodifiedWriteBlock.fileHandle) );
    freeFileIoRequest(r);
}

static void cleanupOneRequestResult( FileIoRequest *r )
{
    switch (r->requestType) // we only need to handle requests that return results here
    {
    case FileIoRequest::OPEN_FILE:
        {
            // in any case, release the path
            r->openFile.path->release();
            r->openFile.path = 0;

            if (r->openFile.fileHandle != 0) { // the open was successful, close the handle
                // convert the open file request into a close request
                void *fileHandle = r->openFile.fileHandle;

                r->requestType = FileIoRequest::CLOSE_FILE;
                r->closeFile.fileHandle = fileHandle;
                handleCloseFileRequest(r);
            } else {
                freeFileIoRequest(r);
            }
        }
        break;

    case FileIoRequest::READ_BLOCK:
        {
            if (r->readBlock.dataBlock) { // the read was successful, release the block
                // convert the read block request into a release read block request
                void *fileHandle = r->readBlock.fileHandle;
                DataBlock *dataBlock = r->readBlock.dataBlock;

                r->requestType = FileIoRequest::RELEASE_READ_BLOCK;
                r->releaseReadBlock.fileHandle = fileHandle;
                r->releaseReadBlock.dataBlock = dataBlock;
                handleReleaseReadBlockRequest(r);
            } else {
                freeFileIoRequest(r);
            }
        }
        break;

    case FileIoRequest::ALLOCATE_WRITE_BLOCK:
        {
            if (r->allocateWriteBlock.dataBlock) { // the allocation was successful, release the block
                // convert the allocate write block request into a release write block request
                void *fileHandle = r->allocateWriteBlock.fileHandle;
                DataBlock *dataBlock = r->allocateWriteBlock.dataBlock;

                r->requestType = FileIoRequest::RELEASE_UNMODIFIED_WRITE_BLOCK;
                r->releaseUnmodifiedWriteBlock.fileHandle = fileHandle;
                r->releaseUnmodifiedWriteBlock.dataBlock = dataBlock;
                handleReleaseUnmodifiedWriteBlockRequest(r);
            } else {
                freeFileIoRequest(r);
            }
        }
        break;
    
    default:
        assert(false); // only requests that have results should be encountered
    }
}

static void handleCleanupResultQueueRequest( FileIoRequest *clientResultQueueContainer )
{
    // Cleanup any results that are in the queue, either free the queue now, or mark it for cleanup later.

    assert( clientResultQueueContainer->requestType == FileIoRequest::CLEANUP_RESULT_QUEUE );

    if (clientResultQueueContainer->resultQueue.expectedResultCount() > 0)
    {
        while (FileIoRequest *r = clientResultQueueContainer->resultQueue.pop())
        {
            cleanupOneRequestResult(r);
        }

        if (clientResultQueueContainer->resultQueue.expectedResultCount() == 0) {
            freeFileIoRequest(clientResultQueueContainer);
        } else {
            // mark the queue for cleanup. 
            // cleanup is resumed by completeRequestToClientResultQueue() the next time that a request completes
            clientResultQueueContainer->requestType = FileIoRequest::RESULT_QUEUE_IS_AWAITING_CLEANUP_;
        }
    } else {
        freeFileIoRequest(clientResultQueueContainer);
    }
}


///////////////////////////////////////////////////////////////////////////////
// Server thread setup and teardown

mint_atomic32_t shutdownFlag_;
HANDLE serverMailboxEvent_;
HANDLE serverThreadHandle_;
QwMPSCFifoQueue<FileIoRequest*, FileIoRequest::TRANSIT_NEXT_LINK_INDEX> serverMailboxQueue_;

static void handleAllPendingRequests()
{
    while (FileIoRequest *r = serverMailboxQueue_.pop()) {
        switch (r->requestType) {
        case FileIoRequest::OPEN_FILE:
            handleOpenFileRequest(r);
            break;
        case FileIoRequest::CLOSE_FILE:
            handleCloseFileRequest(r);
            break;
        case FileIoRequest::READ_BLOCK:
            handleReadBlockRequest(r);
            break;
        case FileIoRequest::RELEASE_READ_BLOCK:
            handleReleaseReadBlockRequest(r);
            break;
        case FileIoRequest::ALLOCATE_WRITE_BLOCK:
            handleAllocateWriteBlockRequest(r);
            break;
        case FileIoRequest::COMMIT_MODIFIED_WRITE_BLOCK:
            handleCommitModifiedWriteBlockRequest(r);
            break;
        case FileIoRequest::RELEASE_UNMODIFIED_WRITE_BLOCK:
            handleReleaseUnmodifiedWriteBlockRequest(r);
            break;
        case FileIoRequest::CLEANUP_RESULT_QUEUE:
            handleCleanupResultQueueRequest(r);
            break;
        }
    }
}


static unsigned int __stdcall serverThreadProc( void * )
{
    while (mint_load_32_relaxed(&shutdownFlag_) == 0) {
        WaitForSingleObject( serverMailboxEvent_, 1000 ); // note: only wait when the incoming queue is empty
        handleAllPendingRequests();
    }

    return 0;
}


void startFileIoServer( size_t fileIoRequestCount )
{
    globalRequestPool_ = new QwNodePool<FileIoRequest>( fileIoRequestCount );
    
    shutdownFlag_._nonatomic = 0;

    serverMailboxEvent_ = CreateEvent( NULL, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE, NULL ); // auto-reset event

    unsigned threadId;
    serverThreadHandle_ = (HANDLE)_beginthreadex( NULL, 0, serverThreadProc, NULL, 0, &threadId );
    SetThreadPriority(serverThreadHandle_,THREAD_PRIORITY_TIME_CRITICAL);
}


void shutDownFileIoServer()
{
    mint_store_32_relaxed(&shutdownFlag_, 1);
    SetEvent(serverMailboxEvent_);

    WaitForSingleObject( serverThreadHandle_, 2000 );
    CloseHandle( serverThreadHandle_ );

    CloseHandle( serverMailboxEvent_ );

    delete globalRequestPool_;
}


void sendFileIoRequestToServer( FileIoRequest *r )
{
    bool wasEmpty=false;
    serverMailboxQueue_.push(r,wasEmpty);
    if (wasEmpty)
        SetEvent( serverMailboxEvent_ );
}



/*
TODO:
    o- factor server mailbox out into separate module, this includes:
        treiber pop-all part
        local reversed stack part
        signalling and waiting mechanism


    o- reduce or eliminate dependence on list templates (to simplify the code)


    The queues we need are:

        - full Treiber LIFO for request allocation (use this in FileIoRequestAllocator indexing a fixed size request buffer)

        - spsc for result queue

        - reversed list for inbound queue to server

        - pop-all list for shared buffer reclamation


    o- ensure server mailbox is cache aligned and that the server-local queue is separated from the global lifo




    x- example read stream routines (all asynchronous O(1) or near to)
        x- allocate a stream
        x- read data from the stream
        x- close the stream

        enum StreamState { STREAM_STATE_BUFFERING, STREAM_STATE_STREAMING };

        typedef void ReadStream;
        ReadStream* ReadStream_open( const char *filePath );
        void ReadStream_close( ReadStream *s );
        StreamState ReadStream_getState( ReadStream *s );
        size_t ReadStream_read( ReadStream *s, void *dest, size_t countBytes );

        o- optional, later: support seeking

    o- example write stream routines (all asynchronous O(1) or near to)
        o- allocate a stream
        o- write data to the stream
        o- close the stream





    o- write an example program that can record and play. 
        press r to start recording, s to stop recording. p to play the recording.



    // TODO: handle requests at two priority levels: 0 and 1 (0 is higher?)
    // lower priority requests go onto a secondary FIFO and only get processed after
    // the higher priority requests. 
    // always process COMMIT_MODIFIED_WRITE_BLOCK at highest priority to avoid issues
    // with file length extending writes

*/