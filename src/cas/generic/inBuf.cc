/*
 *      $Id$
 *
 *      Author  Jeffrey O. Hill
 *              johill@lanl.gov
 *              505 665 1831
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *              The Controls and Automation Group (AT-8)
 *              Ground Test Accelerator
 *              Accelerator Technology Division
 *              Los Alamos National Laboratory
 *
 *      Co-developed with
 *              The Controls and Computing Group
 *              Accelerator Systems Division
 *              Advanced Photon Source
 *              Argonne National Laboratory
 *
 */

#include "server.h"
#include "inBufIL.h" // inBuf in line func

//
// inBuf::inBuf()
//
inBuf::inBuf ( inBufClient &clientIn, bufSizeT ioMinSizeIn ) :
    client ( clientIn ), pBuf ( 0 ), bufSize ( 0u ), bytesInBuffer ( 0u ), nextReadIndex ( 0u ),
    ioMinSize ( ioMinSizeIn ), ctxRecursCount ( 0u )
{
    if ( this->ioMinSize == 0 ) {
        this->ioMinSize = 1;
    }
    if ( this->ioMinSize <= pGlobalBufferFactoryCAS->smallBufferSize () ) {
        this->pBuf = pGlobalBufferFactoryCAS->newSmallBuffer ();
        this->bufSize = pGlobalBufferFactoryCAS->smallBufferSize ();
    }
    else {
        this->pBuf = new char [ this->ioMinSize ];
        this->bufSize = this->ioMinSize;
    }
}

//
// inBuf::~inBuf()
// (virtual destructor)
//
inBuf::~inBuf ()
{
    assert ( this->ctxRecursCount == 0 );
    if ( this->bufSize == pGlobalBufferFactoryCAS->smallBufferSize () ) {
        pGlobalBufferFactoryCAS->destroySmallBuffer ( this->pBuf );
    }
    else if ( this->bufSize == pGlobalBufferFactoryCAS->largeBufferSize () ) {
        pGlobalBufferFactoryCAS->destroyLargeBuffer ( this->pBuf );
    }
    else {
        delete [] this->pBuf;
    }
}

//
// inBuf::show()
//
void inBuf::show (unsigned level) const
{
    if (level>1u) {
        printf(
            "\tUnprocessed request bytes = %d\n",
            this->bytesAvailable());
    }
}

//
// inBuf::fill()
//
inBufClient::fillCondition inBuf::fill ( inBufClient::fillParameter parm )
{
	bufSizeT bytesOpen;
	bufSizeT bytesRecv;
	inBufClient::fillCondition stat;

	//
	// move back any prexisting data to the start of the buffer
	//
	if ( this->nextReadIndex > 0 ) {
		bufSizeT unprocessedBytes;
		unprocessedBytes = this->bytesInBuffer - this->nextReadIndex;
		//
		// memmove() handles overlapping buffers
		//
		if (unprocessedBytes>0u) {
			memmove (this->pBuf, this->pBuf+this->nextReadIndex, 
				unprocessedBytes);
		}
		this->bytesInBuffer = unprocessedBytes;
		this->nextReadIndex = 0u;
	}

	//
	// noop if the buffer is full
	//
	bytesOpen = this->bufSize - this->bytesInBuffer;
	if ( bytesOpen < this->ioMinSize ) {
        return inBufClient::casFillNone;
	}

	stat = this->client.xRecv ( &this->pBuf[this->bytesInBuffer], 
			bytesOpen, parm, bytesRecv );
    if ( stat == inBufClient::casFillProgress ) {
	    assert (bytesRecv<=bytesOpen);
	    this->bytesInBuffer += bytesRecv;

	    if ( this->client.getDebugLevel() > 2u ) {
		    char buf[64];

		    this->client.hostName ( buf, sizeof ( buf ) );

		    printf ("CAS: incoming %u byte msg from %s\n",
			    bytesRecv, buf);
	    }
	}

	return stat;
}

//
// inBuf::pushCtx ()
//
const inBufCtx inBuf::pushCtx ( bufSizeT headerSize, // X aCC 361
                               bufSizeT bodySize )
{
    if ( headerSize + bodySize > ( this->bytesInBuffer - this->nextReadIndex ) || 
        this->ctxRecursCount == UINT_MAX ) {
        return inBufCtx ();
    }
    else {
        inBufCtx result (*this);
        bufSizeT effectiveNextReadIndex = this->nextReadIndex + headerSize;
        this->pBuf = this->pBuf + effectiveNextReadIndex;
        this->bytesInBuffer -= effectiveNextReadIndex;
        this->nextReadIndex = 0;
        this->bytesInBuffer = bodySize;
        this->bufSize = this->bytesInBuffer;
        this->ctxRecursCount++;
        return result;
    }
}

//
// inBuf::popCtx ()
//
bufSizeT inBuf::popCtx (const inBufCtx &ctx) // X aCC 361
{
    if ( ctx.stat==inBufCtx::pushCtxSuccess ) {
        this->mutex.lock();
        bufSizeT bytesRemoved = this->nextReadIndex;
        this->pBuf = ctx.pBuf;
        this->bufSize = ctx.bufSize;
        this->bytesInBuffer = ctx.bytesInBuffer;
        this->nextReadIndex = ctx.nextReadIndex;
        assert (this->ctxRecursCount>0);
        this->ctxRecursCount--;
        this->mutex.unlock();
        return bytesRemoved;
    }
    else {
        return 0;
    }
}

void inBuf::expandBuffer ()
{
    if ( this->bufSize < pGlobalBufferFactoryCAS->largeBufferSize () ) {
        char * pNewBuf = pGlobalBufferFactoryCAS->newLargeBuffer ();
        memcpy ( pNewBuf, &this->pBuf[this->nextReadIndex], this->bytesPresent () );
        this->nextReadIndex = 0u;
        pGlobalBufferFactoryCAS->destroySmallBuffer ( this->pBuf );
        this->pBuf = pNewBuf;
        this->bufSize = pGlobalBufferFactoryCAS->largeBufferSize ();
    }
}

unsigned inBuf::bufferSize () const
{
    return this->bufSize;
}


