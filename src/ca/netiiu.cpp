
/*  $Id$
 *
 *                    L O S  A L A M O S
 *              Los Alamos National Laboratory
 *               Los Alamos, New Mexico 87545
 *
 *  Copyright, 1986, The Regents of the University of California.
 *
 *  Author: Jeff Hill
 */

#include <limits.h>
#include <float.h>

#include "iocinf.h"
#include "netiiu_IL.h"
#include "nciu_IL.h"
#include "baseNMIU_IL.h"

netiiu::netiiu ( cac *pClientCtxIn ) : pClientCtx ( pClientCtxIn )
{
}

netiiu::~netiiu ()
{
}

void netiiu::show ( unsigned level ) const
{
    epicsAutoMutex autoMutex ( this->mutex );

    printf ( "network IO base class\n" );
    if ( level > 1 ) {
        tsDLIterConstBD < nciu > pChan = this->channelList.firstIter ();
	    while ( pChan.valid () ) {
            pChan->show ( level - 1u );
            pChan++;
        }
    }
    if ( level > 2u ) {
        printf ( "\tcac pointer %p\n", 
            static_cast <void *> ( this->pClientCtx ) );
        this->mutex.show ( level - 2u );
    }
}

// cac lock must also be applied when
// calling this
void netiiu::attachChannel ( nciu &chan )
{
    epicsAutoMutex autoMutex ( this->mutex );
    this->channelList.add ( chan );
}

// cac lock must also be applied when
// calling this
void netiiu::detachChannel ( nciu &chan )
{
    epicsAutoMutex autoMutex ( this->mutex );
    this->channelList.remove ( chan );
    if ( this->channelList.count () == 0u ) {
        this->lastChannelDetachNotify ();
    }
}

// cac lock must also be applied when
// calling this
void netiiu::disconnectAllChan ( netiiu & newiiu )
{
    tsDLList < nciu > list;

    {
        epicsAutoMutex autoMutex ( this->mutex );
        tsDLIterBD < nciu > chan = this->channelList.firstIter ();
        while ( chan.valid () ) {
            tsDLIterBD < nciu > next = chan;
            next++;
            this->clearChannelRequest ( *chan );
            this->channelList.remove ( *chan );
            chan->disconnect ( newiiu );
            list.add ( *chan );
            chan = next;
        }
    }

    {
        epicsAutoMutex autoMutex ( newiiu.mutex );
        newiiu.channelList.add ( list );
    }
}

//
// netiiu::destroyAllIO ()
//
// care is taken to not hold the lock while deleting the
// IO so that subscription delete request (sent by the
// IO's destructor) do not deadlock
//
bool netiiu::destroyAllIO ( nciu &chan )
{
    tsDLList < baseNMIU > eventQ;
    {
        epicsAutoMutex autoMutex ( this->mutex );
        if ( chan.verifyIIU ( *this ) ) {
            eventQ.add ( chan.tcpiiuPrivateListOfIO::eventq );
        }
        else {
            return false;
        }
    }
    while ( baseNMIU *pIO = eventQ.get () ) {
        delete pIO;
    }
    return true;
}

void netiiu::connectTimeoutNotify ()
{
    epicsAutoMutex autoMutex ( this->mutex );
    tsDLIterBD < nciu > chan = this->channelList.firstIter ();
    while ( chan.valid () ) {
        chan->connectTimeoutNotify ();
        chan++;
    }
}

void netiiu::resetChannelRetryCounts ()
{
    epicsAutoMutex autoMutex ( this->mutex );
    tsDLIterBD < nciu > chan = this->channelList.firstIter ();
    while ( chan.valid () ) {
        chan->resetRetryCount ();
        chan++;
    }
}

bool netiiu::searchMsg ( unsigned short retrySeqNumber, unsigned &retryNoForThisChannel )
{
    bool success;

    epicsAutoMutex autoMutex ( this->mutex );

    if ( nciu *pChan = this->channelList.get () ) {
        success = pChan->searchMsg ( retrySeqNumber, retryNoForThisChannel );
        if ( success ) {
            this->channelList.add ( *pChan );
        }
        else {
            this->channelList.push ( *pChan );
        }
    }
    else {
        success = false;
    }

    return success;
}

bool netiiu::ca_v42_ok () const
{
    return false;
}

bool netiiu::ca_v41_ok () const
{
    return false;
}

bool netiiu::pushDatagramMsg ( const caHdr &, const void *, ca_uint16_t )
{
    return false;
}

bool netiiu::isVirtaulCircuit ( const char *, const osiSockAddr & ) const
{
    return false;
}

void netiiu::lastChannelDetachNotify ()
{
}

int netiiu::writeRequest ( nciu &, unsigned, unsigned, const void * )
{
    return ECA_DISCONNCHID;
}

int netiiu::writeNotifyRequest ( nciu &, cacNotify &, unsigned, unsigned, const void * )
{
    return ECA_DISCONNCHID;
}

int netiiu::readNotifyRequest ( nciu &, cacNotify &, unsigned, unsigned )
{
    return ECA_DISCONNCHID;
}

int netiiu::createChannelRequest ( nciu & )
{
    return ECA_DISCONNCHID;
}

int netiiu::clearChannelRequest ( nciu & )
{
    return ECA_DISCONNCHID;
}

void netiiu::subscriptionRequest ( netSubscription &, bool )
{
}

void netiiu::subscriptionCancelRequest ( netSubscription &, bool )
{
}

void netiiu::installSubscription ( netSubscription &subscr )
{
    bool connectedWhenInstalled;
    {
        epicsAutoMutex autoMutex ( this->mutex );
        subscr.channel ().tcpiiuPrivateListOfIO::eventq.add ( subscr );
        connectedWhenInstalled = subscr.channel ().connected ();
    }
    // iiu pointer briefly points at tcpiiu before the channel is connected
    if ( connectedWhenInstalled ) {
        this->subscriptionRequest ( subscr, true );
    }
}

void netiiu::hostName ( char *pBuf, unsigned bufLength ) const
{
    if ( bufLength ) {
        strncpy ( pBuf, this->pHostName (), bufLength );
        pBuf[bufLength - 1u] = '\0';
    }
}

const char * netiiu::pHostName () const
{
    return "<disconnected>";
}

void netiiu::disconnectAllIO ( nciu & )
{
}

void netiiu::connectAllIO ( nciu & )
{
}

bool netiiu::uninstallIO ( baseNMIU &io )
{
    epicsAutoMutex autoMutex ( this->mutex );
    if ( io.channel ().verifyIIU ( *this ) ) {
        io.channel ().tcpiiuPrivateListOfIO::eventq.remove ( io );
        return true;
    }
    return false;
}

double netiiu::beaconPeriod () const
{
    return ( - DBL_MAX );
}
