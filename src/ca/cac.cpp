/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/*  
 *  $Id$
 *
 *                    L O S  A L A M O S
 *              Los Alamos National Laboratory
 *               Los Alamos, New Mexico 87545
 *
 *  Copyright, 1986, The Regents of the University of California.
 *
 *  Author: Jeff Hill
 */

#define epicsAssertAuthor "Jeff Hill johill@lanl.gov"

#include <new>

#include "epicsMemory.h"
#include "epicsGuard.h"
#include "epicsVersion.h"
#include "osiProcess.h"
#include "osiSigPipeIgnore.h"
#include "envDefs.h"

#define epicsExportSharedSymbols
#include "iocinf.h"
#include "cac.h"
#include "inetAddrID.h"
#include "caServerID.h"
#include "virtualCircuit.h"
#include "syncGroup.h"
#include "nciu.h"
#include "autoPtrRecycle.h"
#include "msgForMultiplyDefinedPV.h"
#include "udpiiu.h"
#include "bhe.h"
#include "net_convert.h"

static const char *id = "@(#) " EPICS_VERSION_STRING ", CA Portable Server Library" __DATE__;

// TCP response dispatch table
const cac::pProtoStubTCP cac::tcpJumpTableCAC [] = 
{
    &cac::versionAction,
    &cac::eventRespAction,
    &cac::badTCPRespAction,
    &cac::readRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::exceptionRespAction,
    &cac::clearChannelRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::readNotifyRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::claimCIURespAction,
    &cac::writeNotifyRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::accessRightsRespAction,
    &cac::echoRespAction,
    &cac::badTCPRespAction,
    &cac::badTCPRespAction,
    &cac::verifyAndDisconnectChan,
    &cac::verifyAndDisconnectChan
};

// TCP exception dispatch table
const cac::pExcepProtoStubTCP cac::tcpExcepJumpTableCAC [] = 
{
    &cac::defaultExcep,     // CA_PROTO_VERSION
    &cac::eventAddExcep,    // CA_PROTO_EVENT_ADD
    &cac::defaultExcep,     // CA_PROTO_EVENT_CANCEL
    &cac::readExcep,        // CA_PROTO_READ
    &cac::writeExcep,       // CA_PROTO_WRITE
    &cac::defaultExcep,     // CA_PROTO_SNAPSHOT
    &cac::defaultExcep,     // CA_PROTO_SEARCH
    &cac::defaultExcep,     // CA_PROTO_BUILD
    &cac::defaultExcep,     // CA_PROTO_EVENTS_OFF
    &cac::defaultExcep,     // CA_PROTO_EVENTS_ON
    &cac::defaultExcep,     // CA_PROTO_READ_SYNC
    &cac::defaultExcep,     // CA_PROTO_ERROR
    &cac::defaultExcep,     // CA_PROTO_CLEAR_CHANNEL
    &cac::defaultExcep,     // CA_PROTO_RSRV_IS_UP
    &cac::defaultExcep,     // CA_PROTO_NOT_FOUND
    &cac::readNotifyExcep,  // CA_PROTO_READ_NOTIFY
    &cac::defaultExcep,     // CA_PROTO_READ_BUILD
    &cac::defaultExcep,     // REPEATER_CONFIRM
    &cac::defaultExcep,     // CA_PROTO_CLAIM_CIU 
    &cac::writeNotifyExcep, // CA_PROTO_WRITE_NOTIFY
    &cac::defaultExcep,     // CA_PROTO_CLIENT_NAME
    &cac::defaultExcep,     // CA_PROTO_HOST_NAME
    &cac::defaultExcep,     // CA_PROTO_ACCESS_RIGHTS
    &cac::defaultExcep,     // CA_PROTO_ECHO
    &cac::defaultExcep,     // REPEATER_REGISTER
    &cac::defaultExcep,     // CA_PROTO_SIGNAL
    &cac::defaultExcep,     // CA_PROTO_CLAIM_CIU_FAILED
    &cac::defaultExcep      // CA_PROTO_SERVER_DISCONN
};

epicsThreadPrivateId caClientCallbackThreadId;

static epicsThreadOnceId cacOnce = EPICS_THREAD_ONCE_INIT;

extern "C" void cacExitHandler ()
{
    epicsThreadPrivateDelete ( caClientCallbackThreadId );
}

// runs once only for each process
extern "C" void cacOnceFunc ( void * )
{
    caClientCallbackThreadId = epicsThreadPrivateCreate ();
    assert ( caClientCallbackThreadId );
    atexit ( cacExitHandler );
}

//
// cac::cac ()
//
cac::cac ( cacNotify & notifyIn, bool enablePreemptiveCallbackIn ) :
    ipToAEngine ( "dnsQuery" ), 
    programBeginTime ( epicsTime::getCurrent() ),
    connTMO ( CA_CONN_VERIFY_PERIOD ),
    cbMutex ( ! enablePreemptiveCallbackIn ),
    timerQueue ( epicsTimerQueueActive::allocate ( false, 
        lowestPriorityLevelAbove(epicsThreadGetPrioritySelf()) ) ),
    pUserName ( 0 ),
    pudpiiu ( 0 ),
    tcpSmallRecvBufFreeList ( 0 ),
    tcpLargeRecvBufFreeList ( 0 ),
    notify ( notifyIn ),
    initializingThreadsId ( epicsThreadGetIdSelf() ),
    initializingThreadsPriority ( epicsThreadGetPrioritySelf() ),
    maxRecvBytesTCP ( MAX_TCP ),
    preemptiveCallbackEnabled ( enablePreemptiveCallbackIn )
{
	if ( ! osiSockAttach () ) {
        throwWithLocation ( caErrorCode (ECA_INTERNAL) );
	}

    epicsThreadOnce ( &cacOnce, cacOnceFunc, 0 );

    try {
	    long status;

        installSigPipeIgnore ();

        {
            char tmp[256];
            size_t len;
            osiGetUserNameReturn gunRet;

            gunRet = osiGetUserName ( tmp, sizeof (tmp) );
            if ( gunRet != osiGetUserNameSuccess ) {
                tmp[0] = '\0';
            }
            len = strlen ( tmp ) + 1;
            this->pUserName = new char [ len ];
            strncpy ( this->pUserName, tmp, len );
        }

        status = envGetDoubleConfigParam ( &EPICS_CA_CONN_TMO, &this->connTMO );
        if ( status ) {
            this->connTMO = CA_CONN_VERIFY_PERIOD;
            this->printf ( "EPICS \"%s\" double fetch failed\n", EPICS_CA_CONN_TMO.name);
            this->printf ( "Defaulting \"%s\" = %f\n", EPICS_CA_CONN_TMO.name, this->connTMO);
        }

        long maxBytesAsALong;
        status =  envGetLongConfigParam ( &EPICS_CA_MAX_ARRAY_BYTES, &maxBytesAsALong );
        if ( status || maxBytesAsALong < 0 ) {
            errlogPrintf ( "cac: EPICS_CA_MAX_ARRAY_BYTES was not a positive integer\n" );
        }
        else {
            /* allow room for the protocol header so that they get the array size they requested */
            static const unsigned headerSize = sizeof ( caHdr ) + 2 * sizeof ( ca_uint32_t );
            ca_uint32_t maxBytes = ( unsigned ) maxBytesAsALong;
            if ( maxBytes < 0xffffffff - headerSize ) {
                maxBytes += headerSize;
            }
            else {
                maxBytes = 0xffffffff;
            }
            if ( maxBytes < MAX_TCP ) {
                errlogPrintf ( "cac: EPICS_CA_MAX_ARRAY_BYTES was rounded up to %u\n", MAX_TCP );
            }
            else {
                this->maxRecvBytesTCP = maxBytes;
            }
        }
        freeListInitPvt ( &this->tcpSmallRecvBufFreeList, MAX_TCP, 1 );
        if ( ! this->tcpSmallRecvBufFreeList ) {
            throw std::bad_alloc ();
        }

        freeListInitPvt ( &this->tcpLargeRecvBufFreeList, this->maxRecvBytesTCP, 1 );
        if ( ! this->tcpLargeRecvBufFreeList ) {
            throw std::bad_alloc ();
        }
    }
    catch ( ... ) {
        osiSockRelease ();
        delete [] this->pUserName;
        if ( this->tcpSmallRecvBufFreeList ) {
            freeListCleanup ( this->tcpSmallRecvBufFreeList );
        }
        if ( this->tcpLargeRecvBufFreeList ) {
            freeListCleanup ( this->tcpLargeRecvBufFreeList );
        }
        this->timerQueue.release ();
        throw;
    }
}

cac::~cac ()
{
    // this blocks until the UDP thread exits so that
    // it will not sneak in any new clients
    //
    // lock intentionally not held here so that we dont deadlock 
    // waiting for the UDP thread to exit while it is waiting to 
    // get the lock.
    if ( this->pudpiiu ) {
        this->pudpiiu->shutdown ();
    }

    //
    // shutdown all tcp circuits
    //
    {
        epicsGuard < cacMutex > guard ( this->mutex );
        resTableIter < tcpiiu, caServerID > iter = this->serverTable.firstIter ();
        while ( iter.valid() ) {
            iter->initiateCleanShutdown (  guard );
            iter++;
        }
    }

    //
    // wait for all tcp threads to exit
    //
    // this will block for oustanding sends to go out so dont 
    // hold a lock while waiting
    //
    while ( this->serverTable.numEntriesInstalled() ) {
        this->iiuUninstall.wait ();
    }

    if ( this->pudpiiu ) {
        delete this->pudpiiu;
    }

    freeListCleanup ( this->tcpSmallRecvBufFreeList );
    freeListCleanup ( this->tcpLargeRecvBufFreeList );

    delete [] this->pUserName;

    this->beaconTable.traverse ( &bhe::destroy );

    osiSockRelease ();

    this->timerQueue.release ();

    errlogFlush ();

    // its ok for channels and subscriptions to still
    // exist at this point. The user created them and 
    // its his responsibility to clean them up.
}

unsigned cac::lowestPriorityLevelAbove ( unsigned priority )
{
    unsigned abovePriority;
    epicsThreadBooleanStatus tbs;
    tbs = epicsThreadLowestPriorityLevelAbove ( 
        priority, & abovePriority );
    if ( tbs != epicsThreadBooleanStatusSuccess ) {
        abovePriority = priority;
    }
    return abovePriority;
}

unsigned cac::highestPriorityLevelBelow ( unsigned priority )
{
    unsigned belowPriority;
    epicsThreadBooleanStatus tbs;
    tbs = epicsThreadHighestPriorityLevelBelow ( 
        priority, & belowPriority );
    if ( tbs != epicsThreadBooleanStatusSuccess ) {
        belowPriority = priority;
    }
    return belowPriority;
}

//
// set the push pending flag on all virtual circuits
//
void cac::flushRequest ()
{
    epicsGuard < cacMutex > guard ( this->mutex );
    this->serverTable.traverse ( & tcpiiu::flushRequest );
}

unsigned cac::connectionCount () const
{
    epicsGuard < cacMutex > guard ( this->mutex );
    return this->serverTable.numEntriesInstalled ();
}

void cac::show ( unsigned level ) const
{
    epicsGuard < cacMutex > autoMutex2 ( this->mutex );

    ::printf ( "Channel Access Client Context at %p for user %s\n", 
        static_cast <const void *> ( this ), this->pUserName );
    if ( level > 0u ) {
        this->serverTable.show ( level - 1u );
        ::printf ( "\tconnection time out watchdog period %f\n", this->connTMO );
        ::printf ( "list of installed services:\n" );
        this->services.show ( level - 1u );
    }

    if ( level > 1u ) {
        if ( this->pudpiiu ) {
            this->pudpiiu->show ( level - 2u );
        }
    }

    if ( level > 2u ) {
        ::printf ( "Program begin time:\n");
        this->programBeginTime.show ( level - 3u );
        ::printf ( "Channel identifier hash table:\n" );
        this->chanTable.show ( level - 3u );
        ::printf ( "IO identifier hash table:\n" );
        this->ioTable.show ( level - 3u );
        ::printf ( "Synchronous group identifier hash table:\n" );
        this->sgTable.show ( level - 3u );
        ::printf ( "Beacon source identifier hash table:\n" );
        this->beaconTable.show ( level - 3u );
        ::printf ( "Timer queue:\n" );
        this->timerQueue.show ( level - 3u );
        ::printf ( "IP address to name conversion engine:\n" );
        this->ipToAEngine.show ( level - 3u );
    }

    if ( level > 3u ) {
        ::printf ( "Default mutex:\n");
        this->mutex.show ( level - 4u );
        ::printf ( "mutex:\n" );
        this->mutex.show ( level - 3u );
    }
}

/*
 *  cac::beaconNotify
 */
void cac::beaconNotify ( const inetAddrID & addr, const epicsTime & currentTime,
                        unsigned beaconNumber, unsigned protocolRevision  )
{
    epicsGuard < cacMutex > guard ( this->mutex );

    if ( ! this->pudpiiu ) {
        return;
    }

    /*
     * look for it in the hash table
     */
    bhe *pBHE = this->beaconTable.lookup ( addr );
    if ( pBHE ) {
        /*
         * return if the beacon period has not changed significantly
         */
        if ( ! pBHE->updatePeriod ( this->programBeginTime, currentTime, 
                beaconNumber, protocolRevision ) ) {
            return;
        }
    }
    else {
        /*
         * This is the first beacon seen from this server.
         * Wait until 2nd beacon is seen before deciding
         * if it is a new server (or just the first
         * time that we have seen a server's beacon
         * shortly after the program started up)
         */
        pBHE = new bhe ( currentTime, beaconNumber, addr );
        if ( pBHE ) {
            if ( this->beaconTable.add ( *pBHE ) < 0 ) {
                pBHE->destroy ();
            }
        }
        return;
    }

    this->pudpiiu->beaconAnomalyNotify ();

#   if DEBUG
    {
        char buf[64];
        ipAddrToDottedIP (pnet_addr, buf, sizeof ( buf ) );
        printf ("new server available: %s\n", buf);
    }
#   endif
}

void cac::installCASG ( CASG &sg )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    this->sgTable.add ( sg );
}

void cac::uninstallCASG ( CASG & sg )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    this->sgTable.remove ( sg );
}

CASG * cac::lookupCASG ( unsigned id )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    CASG * psg = this->sgTable.lookup ( id );
    if ( psg ) {
        if ( ! psg->verify () ) {
            psg = 0;
        }
    }
    return psg;
}

void cac::registerService ( cacService &service )
{
    this->services.registerService ( service );
}

cacChannel & cac::createChannel ( const char * pName, 
    cacChannelNotify & chan, cacChannel::priLev pri )
{
    cacChannel *pIO;

    if ( pri > cacChannel::priorityMax ) {
        throw cacChannel::badPriority ();
    }

    if ( pName == 0 || pName[0] == '\0' ) {
        throw cacChannel::badString ();
    }

    pIO = this->services.createChannel ( pName, chan, pri );
    if ( ! pIO ) {
        pIO = pGlobalServiceListCAC->createChannel ( pName, chan, pri );
        if ( ! pIO ) {
            if ( ! this->pudpiiu ) {
                epicsGuard < cacMutex > guard ( this->mutex );
                if ( ! this->pudpiiu ) {
                    this->pudpiiu = new udpiiu ( this->timerQueue, this->cbMutex, *this );
                }
            }
            epicsGuard < cacMutex > guard ( this->mutex );
            epics_auto_ptr < nciu > pNetChan 
                ( new nciu ( *this, *this->pudpiiu, chan, pName, pri ) );
            this->chanTable.add ( *pNetChan );
            return *pNetChan.release ();
        }
    }
    return *pIO;
}

void cac::repeaterSubscribeConfirmNotify ()
{
    if ( this->pudpiiu ) {
        this->pudpiiu->repeaterConfirmNotify ();
    }
}

bool cac::lookupChannelAndTransferToTCP ( 
             epicsGuard < callbackMutex > & cbGuard, unsigned cid, unsigned sid, 
             ca_uint16_t typeCode, arrayElementCount count, 
             unsigned minorVersionNumber, const osiSockAddr & addr,
             const epicsTime & currentTime )
{
    bool newIIU = false;
    tcpiiu * piiu = 0;

    if ( addr.sa.sa_family != AF_INET ) {
        return false;
    }

    bool v41Ok, v42Ok;
    nciu *pChan;
    {
        epicsGuard < cacMutex > guard ( this->mutex );

        /*
         * ignore search replies for deleted channels
         */
        pChan = this->chanTable.lookup ( cid );
        if ( ! pChan ) {
            return true;
        }

        /*
         * Ignore duplicate search replies
         */
        osiSockAddr chanAddr = pChan->getPIIU()->getNetworkAddress ();
        if ( chanAddr.sa.sa_family != AF_UNSPEC ) {
            if ( ! sockAddrAreIdentical ( &addr, &chanAddr ) ) {
                char acc[64];
                pChan->getPIIU()->hostName ( acc, sizeof ( acc ) );
                msgForMultiplyDefinedPV *pMsg = new msgForMultiplyDefinedPV ( 
                    this->cbMutex, *this, pChan->pName (), acc, addr );
                if ( pMsg ) {
                    this->ipAddrToAsciiAsynchronousRequestInstall ( *pMsg );
                }
            }
            return true;
        }

        /*
         * look for an existing virtual circuit
         */
        caServerID servID ( addr.ia, pChan->getPriority() );
        piiu = this->serverTable.lookup ( servID );
        if ( piiu ) {
            if ( ! piiu->alive () ) {
                return true;
            }
        }
        else {
            try {
                epics_auto_ptr < tcpiiu > pnewiiu ( new tcpiiu ( 
                            *this, this->cbMutex, this->connTMO, this->timerQueue,
                            addr, minorVersionNumber, this->ipToAEngine,
                            pChan->getPriority() ) );
                if ( pnewiiu.get() == 0 ) {
                    return true;
                }
                bhe * pBHE = this->beaconTable.lookup ( addr.ia );
                if ( ! pBHE ) {
                    epics_auto_ptr < bhe > pNewBHE ( new bhe ( epicsTime (), 0u, addr.ia ) );
                    if ( pNewBHE.get () ) {
                        if ( this->beaconTable.add ( *pNewBHE ) >= 0 ) {
                            pBHE = pNewBHE.release ();
                        }
                        else {
                            return true;
                        }
                    } 
                    else {
                        return true;
                    }
                }
                this->serverTable.add ( *pnewiiu );
                pBHE->registerIIU ( *pnewiiu );
                piiu = pnewiiu.release ();
                newIIU = true;
            }
            catch ( ... ) {
                this->printf ( "CAC: Exception during virtual circuit creation\n" );
                return true;
            }
        }

        this->pudpiiu->uninstallChan ( guard, *pChan );
        piiu->installChannel ( guard, *pChan, sid, typeCode, count );

        v41Ok = piiu->ca_v41_ok ();
        v42Ok = piiu->ca_v42_ok ();

        if ( ! v42Ok ) {
            // connect to old server with lock applied
            pChan->connect ();
            // resubscribe for monitors from this channel 
            this->connectAllIO ( guard, *pChan );
        }
    }

    if ( ! v42Ok ) {
        // channel uninstal routine grabs the callback lock so
        // a channel will not be deleted while a call back is 
        // in progress
        //
        // the callback lock is also taken when a channel 
        // disconnects to prevent a race condition with the 
        // code below - ie we hold the callback lock here
        // so a chanel cant be destroyed out from under us.
        pChan->connectStateNotify ( cbGuard );

        /*
         * if less than v4.1 then the server will never
         * send access rights and we know that there
         * will always be access and also need to call 
         * their call back here
         */
        if ( ! v41Ok ) {
            pChan->accessRightsNotify ( cbGuard );
        }
    }

    if ( newIIU ) {
        piiu->start ( cbGuard );
    }

    if ( this->pudpiiu ) {
        this->pudpiiu->notifySearchResponse ( currentTime );
    }

    return true;
}

void cac::uninstallChannel ( nciu & chan )
{
    tsDLList < baseNMIU > tmpList;

    // uninstall channel and any subsiderary IO so that recv threads
    // will not start a new callback for this channel's IO. Send any 
    // side effect IO requests w/o holding the callback lock so that 
    // we do not dead lock
    {
        epicsGuard < cacMutex > guard ( this->mutex );

        // if the send backlog is too high send some frames before we get entagled
        // in the channel shutdown sequence below. There is special protection in
        // this routine that releases the callback lock if we are already holding it
        // when this is the tcp receive thread or if this is the main thread and
        // preemptive callback is disabled.
        this->flushIfRequired ( guard, *chan.getPIIU() );

        // unregister the channel
        if ( this->chanTable.remove ( chan ) != &chan ) {
            errlogPrintf ( 
                "CAC: Attemt to uninstall unregisterred channel ID=%u ignored.\n",
                chan.getId () );
            return;
        }

        // for each outstanding IO
        //
        // IO must be removed from the list and also uninstalled while holding the
        // lock so that ioCancel() does not break in while postponing the destroy 
        // waiting for outstanding callbacks to complete
        //
        while ( baseNMIU *pIO = chan.cacPrivateListOfIO::eventq.get() ) {
            // unregister IO class
            if ( pIO != this->ioTable.remove ( *pIO ) ) {
                errlogPrintf ( 
                    "CAC: Unregister IO ID=%u found when uninstalling channel?\n",
                    pIO->getId () );
                continue;
            }
            // connected subscriptions must be canceled in the server
            class netSubscription *pSubscr = pIO->isSubscription ();
            if ( pSubscr && chan.connected() ) {
                // we will deadlock if we hold the callback lock here
                chan.getPIIU()->subscriptionCancelRequest ( guard, chan, *pSubscr );
            }
            tmpList.add ( *pIO );
        }        

        // if the claim reply has not returned yet then we will issue
        // the clear channel request to the server when the claim reply
        // arrives and there is no matching nciu in the client
        if ( chan.connected() ) {
            chan.getPIIU()->clearChannelRequest ( guard, chan.getSID(), chan.getCID() );
        }
    }

    {
        // taking this mutex prior to deleting the IO and channel guarantees 
        // that we will not delete a channel out from under a callback
        epicsGuard < callbackMutex > cbGuard ( this->cbMutex );

        // destroy subsiderary IO now that it is safe to do so
        while ( baseNMIU *pIO = tmpList.get() ) {
            // If they call ioCancel() here it will be ignored
            // because the IO has been unregistered above.
            // This must be done after outstanding callbacks 
            // for this channel have completed.
            pIO->exception ( ECA_CHANDESTROY, chan.pName() );
            pIO->destroy ( *this );
        }        

        // this must be done after the following
        // o subscription cancel requests
        // o clear channel request 
        // o outstanding callbacks using this channel have completed
        // o chan destroy exception has been delivered
        {
            epicsGuard < cacMutex > guard ( this->mutex );
            chan.getPIIU()->uninstallChan ( guard, chan );
        }
    }
}

int cac::printf ( const char *pformat, ... ) const
{
    va_list theArgs;
    int status;

    va_start ( theArgs, pformat );
    
    status = this->vPrintf ( pformat, theArgs );
    
    va_end ( theArgs );
    
    return status;
}

// lock must be applied before calling this cac private routine
void cac::flushIfRequired ( epicsGuard < cacMutex > & guard, netiiu & iiu )
{
    if ( iiu.flushBlockThreshold ( guard ) ) {
        iiu.flushRequest ();
        // the process thread is not permitted to flush as this
        // can result in a push / pull deadlock on the TCP pipe.
        // Instead, the process thread scheduals the flush with the 
        // send thread which runs at a higher priority than the 
        // send thread. The same applies to the UDP thread for
        // locking hierarchy reasons.
        if ( ! epicsThreadPrivateGet ( caClientCallbackThreadId ) ) {
            // enable / disable of call back preemption must occur here
            // because the tcpiiu might disconnect while waiting and its
            // pointer to this cac might become invalid
            iiu.blockUntilSendBacklogIsReasonable ( this->notify, guard );
        }
    }
    else {
        iiu.flushRequestIfAboveEarlyThreshold ( guard );
    }
}

void cac::writeRequest ( nciu & chan, unsigned type, unsigned nElem, const void * pValue )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    this->flushIfRequired ( guard, *chan.getPIIU() );
    chan.getPIIU()->writeRequest ( guard, chan, type, nElem, pValue );
}

cacChannel::ioid 
cac::writeNotifyRequest ( nciu &chan, unsigned type, // X aCC 361
                          unsigned nElem, const void *pValue, cacWriteNotify &notifyIn )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    autoPtrRecycle  < netWriteNotifyIO > pIO ( this->ioTable, chan.cacPrivateListOfIO::eventq,
        *this, netWriteNotifyIO::factory ( this->freeListWriteNotifyIO, chan, notifyIn ) );
    this->ioTable.add ( *pIO );
    chan.cacPrivateListOfIO::eventq.add ( *pIO );
    this->flushIfRequired ( guard, *chan.getPIIU() );
    chan.getPIIU()->writeNotifyRequest ( 
        guard, chan, *pIO, type, nElem, pValue );
    return pIO.release()->getId ();
}

cacChannel::ioid
cac::readNotifyRequest ( nciu &chan, unsigned type, // X aCC 361
                         unsigned nElem, cacReadNotify &notifyIn )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    autoPtrRecycle  < netReadNotifyIO > pIO ( this->ioTable,
        chan.cacPrivateListOfIO::eventq, *this,
        netReadNotifyIO::factory ( this->freeListReadNotifyIO, chan, notifyIn ) );
    this->ioTable.add ( *pIO );
    chan.cacPrivateListOfIO::eventq.add ( *pIO );
    this->flushIfRequired ( guard, *chan.getPIIU() );
    chan.getPIIU()->readNotifyRequest ( guard, chan, *pIO, type, nElem );
    return pIO.release()->getId ();
}

void cac::ioCancel ( nciu &chan, const cacChannel::ioid &id )
{   
    baseNMIU * pmiu;

    // unistall the IO object so that a receive thread will not find it,
    // but do _not_ hold the callback lock here because this could result 
    // in deadlock
    {
        epicsGuard < cacMutex > guard ( this->mutex );
        pmiu = this->ioTable.remove ( id );
        if ( ! pmiu ) {
            return;
        }
        class netSubscription *pSubscr = pmiu->isSubscription ();
        if ( pSubscr ) {
            this->flushIfRequired ( guard, *chan.getPIIU() );
            if ( chan.connected() ) {
                chan.getPIIU()->subscriptionCancelRequest ( guard, chan, *pSubscr );  
            }
        }
        // must be uninstalled and also removed from the table 
        // while holding the lock to prevent a channel delete 
        // from destroying this IO object after we release the lock
        chan.cacPrivateListOfIO::eventq.remove ( *pmiu );
    }
    // wait for any IO callbacks in progress to complete
    // prior to destroying the IO object
    {
        epicsGuard < callbackMutex > cbGuard ( this->cbMutex );
    }
    // now it is safe to destroy the IO object
    {
        epicsGuard < cacMutex > guard ( this->mutex );
        pmiu->destroy ( *this );
    }
}

void cac::ioShow ( const cacChannel::ioid &id, unsigned level ) const
{
    epicsGuard < cacMutex > autoMutex ( this->mutex );
    baseNMIU * pmiu = this->ioTable.lookup ( id );
    if ( pmiu ) {
        pmiu->show ( level );
    }
}

void cac::ioCompletionNotify ( unsigned id, unsigned type, 
                              arrayElementCount count, const void *pData )
{
    baseNMIU * pmiu;
    
    {
        epicsGuard < cacMutex > autoMutex ( this->mutex );
        pmiu = this->ioTable.lookup ( id );
        if ( ! pmiu ) {
            return;
        }
    }

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //
    pmiu->completion ( type, count, pData );
}

void cac::ioExceptionNotify ( unsigned id, int status, const char *pContext )
{
    baseNMIU * pmiu;
    {
        epicsGuard < cacMutex > autoMutex ( this->mutex );
        pmiu = this->ioTable.lookup ( id );
    }

    if ( ! pmiu ) {
        return;
    }

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //

    pmiu->exception ( status, pContext );
}

void cac::ioExceptionNotify ( unsigned id, int status, 
                   const char *pContext, unsigned type, arrayElementCount count )
{
    baseNMIU * pmiu;
    
    {
        epicsGuard < cacMutex > autoMutex ( this->mutex );
        pmiu = this->ioTable.lookup ( id );
        if ( ! pmiu ) {
            return;
        }
    }

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //
    pmiu->exception ( status, pContext, type, count );
}

void cac::ioCompletionNotifyAndDestroy ( unsigned id )
{
    epicsGuard < cacMutex > autoMutex ( this->mutex );
    baseNMIU * pmiu = this->ioTable.remove ( id );
    if ( ! pmiu ) {
        return;
    }

    pmiu->channel().cacPrivateListOfIO::eventq.remove ( *pmiu );

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //
    {
        epicsGuardRelease < cacMutex > autoMutexRelease ( autoMutex );
        pmiu->completion ();
    }

    pmiu->destroy ( *this );
}

void cac::ioCompletionNotifyAndDestroy ( unsigned id, 
    unsigned type, arrayElementCount count, const void *pData )
{
    epicsGuard < cacMutex > autoMutex ( this->mutex );
    baseNMIU * pmiu = this->ioTable.remove ( id );
    if ( ! pmiu ) {
        return;
    }
    pmiu->channel().cacPrivateListOfIO::eventq.remove ( *pmiu );

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //
    {
        epicsGuardRelease < cacMutex > autoMutexRelease ( autoMutex );
        pmiu->completion ( type, count, pData );
    }

    pmiu->destroy ( *this );
}

void cac::ioExceptionNotifyAndDestroy ( unsigned id, int status, 
                                       const char *pContext )
{
    epicsGuard < cacMutex > autoMutex ( this->mutex );
    baseNMIU * pmiu = this->ioTable.remove ( id );
    if ( ! pmiu ) {
        return;
    }
    pmiu->channel().cacPrivateListOfIO::eventq.remove ( *pmiu );

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //
    {
        epicsGuardRelease < cacMutex > autoMutexRelease ( autoMutex );
        pmiu->exception ( status, pContext );
    }

    pmiu->destroy ( *this );
}

void cac::ioExceptionNotifyAndDestroy ( unsigned id, int status, 
                        const char *pContext, unsigned type, arrayElementCount count )
{
    epicsGuard < cacMutex > autoMutex ( this->mutex );
    baseNMIU * pmiu = this->ioTable.remove ( id );
    if ( ! pmiu ) {
        return;
    }
    pmiu->channel().cacPrivateListOfIO::eventq.remove ( *pmiu );

    //
    // The IO destroy routines take the call back mutex 
    // when uninstalling and deleting the baseNMIU so there is 
    // no need to worry here about the baseNMIU being deleted while
    // it is in use here.
    //

    {
        epicsGuardRelease < cacMutex > autoMutexRelease ( autoMutex );
        pmiu->exception ( status, pContext, type, count );
    }

    pmiu->destroy ( *this );
}

// resubscribe for monitors from this channel
// (always called from a udp thread)
void cac::connectAllIO ( epicsGuard < cacMutex > & guard, nciu & chan )
{
    tsDLIter < baseNMIU > pNetIO = 
        chan.cacPrivateListOfIO::eventq.firstIter ();
    while ( pNetIO.valid () ) {
        tsDLIter < baseNMIU > next = pNetIO;
        next++;
        class netSubscription *pSubscr = pNetIO->isSubscription ();
        // disconnected channels should have only subscription IO attached
        assert ( pSubscr );
        try {
            chan.getPIIU()->subscriptionRequest ( guard, chan, *pSubscr );
        }
        catch ( ... ) {
            this->printf ( "CAC: failed to send subscription request during channel connect\n" );
        }
        pNetIO = next;
    }
    chan.getPIIU()->requestRecvProcessPostponedFlush ();
}

// cancel IO operations and monitor subscriptions
// -- callback lock and cac lock must be applied here
void cac::disconnectAllIO ( epicsGuard < cacMutex > &locker, nciu & chan, bool enableCallbacks )
{
    tsDLIter<baseNMIU> pNetIO = chan.cacPrivateListOfIO::eventq.firstIter();
    while ( pNetIO.valid() ) {
        tsDLIter<baseNMIU> pNext = pNetIO;
        pNext++;
        if ( ! pNetIO->isSubscription() ) {
            // no use after disconnected - so uninstall it
            this->ioTable.remove ( *pNetIO );
            chan.cacPrivateListOfIO::eventq.remove ( *pNetIO );
        }
        if ( enableCallbacks ) {
            char buf[128];
            sprintf ( buf, "host = %.100s", chan.pHostName() );
            epicsGuardRelease < cacMutex > unlocker ( locker );
            pNetIO->exception ( ECA_DISCONN, buf );
        }
        if ( ! pNetIO->isSubscription() ) {
            pNetIO->destroy ( *this );
        }
        pNetIO = pNext;
    }
}

void cac::recycleReadNotifyIO ( netReadNotifyIO &io )
{
    this->freeListReadNotifyIO.release ( &io, sizeof ( io ) );
}

void cac::recycleWriteNotifyIO ( netWriteNotifyIO &io )
{
    this->freeListWriteNotifyIO.release ( &io, sizeof ( io ) );
}

void cac::recycleSubscription ( netSubscription &io )
{
    this->freeListSubscription.release ( &io, sizeof ( io ) );
}

cacChannel::ioid
cac::subscriptionRequest ( nciu &chan, unsigned type, // X aCC 361
                           arrayElementCount nElem, unsigned mask,
                           cacStateNotify &notifyIn )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    autoPtrRecycle  < netSubscription > pIO ( this->ioTable,
        chan.cacPrivateListOfIO::eventq, *this, 
        netSubscription::factory ( this->freeListSubscription,
                                   chan, type, nElem, mask, notifyIn ) );
    this->ioTable.add ( *pIO );
    chan.cacPrivateListOfIO::eventq.add ( *pIO );
    if ( chan.connected () ) {
        this->flushIfRequired ( guard, *chan.getPIIU() );
        chan.getPIIU()->subscriptionRequest ( guard, chan, *pIO );
    }
    cacChannel::ioid id = pIO->getId ();
    pIO.release ();
    return id;
}

bool cac::versionAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
                      const caHdrLargeArray &, void * /* pMsgBdy */ )
{
    return true;
}
 
bool cac::echoRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
                      const caHdrLargeArray &, void * /* pMsgBdy */ )
{
    return true;
}

bool cac::writeNotifyRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
                      const caHdrLargeArray &hdr, void * /* pMsgBdy */ )
{
    int caStatus = hdr.m_cid;
    if ( caStatus == ECA_NORMAL ) {
        this->ioCompletionNotifyAndDestroy ( hdr.m_available );
    }
    else {
        this->ioExceptionNotifyAndDestroy ( hdr.m_available, 
                    caStatus, "write notify request rejected" );
    }
    return true;
}

bool cac::readNotifyRespAction ( epicsGuard < callbackMutex > &, tcpiiu &iiu, 
                      const caHdrLargeArray &hdr, void *pMsgBdy )
{
    /*
     * the channel id field is abused for
     * read notify status starting with CA V4.1
     */
    int caStatus;
    if ( iiu.ca_v41_ok() ) {
        caStatus = hdr.m_cid;
    }
    else {
        caStatus = ECA_NORMAL;
    }

    /*
     * convert the data buffer from net
     * format to host format
     */
#   ifdef CONVERSION_REQUIRED 
        if ( hdr.m_dataType < NELEMENTS ( cac_dbr_cvrt ) ) {
            ( *cac_dbr_cvrt[ hdr.m_dataType ] ) (
                 pMsgBdy, pMsgBdy, false, hdr.m_count);
        }
        else {
            caStatus = ECA_BADTYPE;
        }
#   endif

    if ( caStatus == ECA_NORMAL ) {
        this->ioCompletionNotifyAndDestroy ( hdr.m_available,
            hdr.m_dataType, hdr.m_count, pMsgBdy );
    }
    else {
        this->ioExceptionNotifyAndDestroy ( hdr.m_available,
            caStatus, "read failed", hdr.m_dataType, hdr.m_count );
    }
    return true;
}

bool cac::eventRespAction (epicsGuard < callbackMutex > &, tcpiiu &iiu, 
                const caHdrLargeArray &hdr, void *pMsgBdy )
{   
    int caStatus;

    /*
     * m_postsize = 0 used to be a subscription cancel confirmation, 
     * but is now a noop because the IO block is immediately deleted
     */
    if ( ! hdr.m_postsize ) {
        return true;
    }

    /*
     * the channel id field is abused for
     * read notify status starting with CA V4.1
     */
    if ( iiu.ca_v41_ok() ) {
        caStatus = hdr.m_cid;
    }
    else {
        caStatus = ECA_NORMAL;
    }

    /*
     * convert the data buffer from net format to host format
     */
#   ifdef CONVERSION_REQUIRED 
        if ( hdr.m_dataType < NELEMENTS ( cac_dbr_cvrt ) ) {
            ( *cac_dbr_cvrt [ hdr.m_dataType ] )(
                 pMsgBdy, pMsgBdy, false, hdr.m_count);
        }
        else {
            caStatus = epicsHTON32 ( ECA_BADTYPE );
        }
#   endif

    if ( caStatus == ECA_NORMAL ) {
        this->ioCompletionNotify ( hdr.m_available,
            hdr.m_dataType, hdr.m_count, pMsgBdy );
    }
    else {
        this->ioExceptionNotify ( hdr.m_available,
                caStatus, "subscription update failed", 
                hdr.m_dataType, hdr.m_count );
    }
    return true;
}

bool cac::readRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
                        const caHdrLargeArray &hdr, void *pMsgBdy )
{
    this->ioCompletionNotifyAndDestroy ( hdr.m_available,
        hdr.m_dataType, hdr.m_count, pMsgBdy );
    return true;
}

bool cac::clearChannelRespAction ( epicsGuard < callbackMutex > &, tcpiiu &, 
                        const caHdrLargeArray &, void * /* pMsgBdy */ )
{
    return true; // currently a noop
}

bool cac::defaultExcep ( epicsGuard < callbackMutex > &, tcpiiu &iiu, 
                    const caHdrLargeArray &, 
                    const char *pCtx, unsigned status )
{
    char buf[512];
    char hostName[64];
    iiu.hostName ( hostName, sizeof ( hostName ) );
    sprintf ( buf, "host=%s ctx=%.400s", hostName, pCtx );
    this->notify.exception ( status, buf, 0, 0u );
    return true;
}

bool cac::eventAddExcep ( epicsGuard < callbackMutex > &, tcpiiu & /* iiu */, 
                         const caHdrLargeArray &hdr, 
                         const char *pCtx, unsigned status )
{
    this->ioExceptionNotify ( hdr.m_available, status, pCtx, 
        hdr.m_dataType, hdr.m_count );
    return true;
}

bool cac::readExcep ( epicsGuard < callbackMutex > &, tcpiiu &, 
                     const caHdrLargeArray &hdr, 
                     const char *pCtx, unsigned status )
{
    this->ioExceptionNotifyAndDestroy ( hdr.m_available, 
        status, pCtx, hdr.m_dataType, hdr.m_count );
    return true;
}

bool cac::writeExcep ( epicsGuard < callbackMutex > &cbLocker, // X aCC 431
                       tcpiiu &,
                       const caHdrLargeArray &hdr, 
                       const char *pCtx, unsigned status )
{
    nciu * pChan = this->chanTable.lookup ( hdr.m_available );
    if ( pChan ) {
        pChan->writeException ( cbLocker, status, pCtx, 
            hdr.m_dataType, hdr.m_count );
    }
    return true;
}

bool cac::readNotifyExcep ( epicsGuard < callbackMutex > &, tcpiiu &, 
                           const caHdrLargeArray &hdr, 
                           const char *pCtx, unsigned status )
{
    this->ioExceptionNotifyAndDestroy ( hdr.m_available, 
        status, pCtx, hdr.m_dataType, hdr.m_count );
    return true;
}

bool cac::writeNotifyExcep ( epicsGuard < callbackMutex > &, tcpiiu &, 
                            const caHdrLargeArray &hdr, 
                            const char *pCtx, unsigned status )
{
    this->ioExceptionNotifyAndDestroy ( hdr.m_available, 
        status, pCtx, hdr.m_dataType, hdr.m_count );
    return true;
}

bool cac::exceptionRespAction ( epicsGuard < callbackMutex > & cbMutexIn, tcpiiu & iiu, 
                            const caHdrLargeArray & hdr, void * pMsgBdy )
{
    const caHdr * pReq = reinterpret_cast < const caHdr * > ( pMsgBdy );
    unsigned bytesSoFar = sizeof ( *pReq );
    if ( hdr.m_postsize < bytesSoFar ) {
        return false;
    }
    caHdrLargeArray req;
    req.m_cmmd = epicsNTOH16 ( pReq->m_cmmd );
    req.m_postsize = epicsNTOH16 ( pReq->m_postsize );
    req.m_dataType = epicsNTOH16 ( pReq->m_dataType );
    req.m_count = epicsNTOH16 ( pReq->m_count );
    req.m_cid = epicsNTOH32 ( pReq->m_cid );
    req.m_available = epicsNTOH32 ( pReq->m_available );
    const ca_uint32_t * pLW = reinterpret_cast < const ca_uint32_t * > ( pReq + 1 );
    if ( req.m_postsize == 0xffff ) {
        static const unsigned annexSize = 
            sizeof ( req.m_postsize ) + sizeof ( req.m_count );
        bytesSoFar += annexSize;
        if ( hdr.m_postsize < bytesSoFar ) {
            return false;
        }
        req.m_postsize = epicsNTOH32 ( pLW[0] );
        req.m_count = epicsNTOH32 ( pLW[1] );
        pLW += 2u;
    }

    // execute the exception message
    pExcepProtoStubTCP pStub;
    if ( hdr.m_cmmd >= NELEMENTS ( cac::tcpExcepJumpTableCAC ) ) {
        pStub = &cac::defaultExcep;
    }
    else {
        pStub = cac::tcpExcepJumpTableCAC [req.m_cmmd];
    }
    const char *pCtx = reinterpret_cast < const char * > ( pLW );
    return ( this->*pStub ) ( cbMutexIn, iiu, req, pCtx, hdr.m_available );
}

bool cac::accessRightsRespAction (
        epicsGuard < callbackMutex > & cbGuard, tcpiiu &, // X aCC 431
        const caHdrLargeArray &hdr, void * /* pMsgBdy */ )
{
    nciu * pChan;
    {
        epicsGuard < cacMutex > guard ( this->mutex );
        pChan = this->chanTable.lookup ( hdr.m_cid );
        if ( pChan ) {
            unsigned ar = hdr.m_available;
            caAccessRights accessRights ( 
                ( ar & CA_PROTO_ACCESS_RIGHT_READ ) ? true : false, 
                ( ar & CA_PROTO_ACCESS_RIGHT_WRITE ) ? true : false); 
            pChan->accessRightsStateChange ( accessRights );
        }
    }

    //
    // the channel delete routine takes the call back lock so
    // that this will not be called when the channel is being
    // deleted.
    //       
    if ( pChan ) {
        pChan->accessRightsNotify ( cbGuard );
    }

    return true;
}

bool cac::claimCIURespAction (
        epicsGuard < callbackMutex > &cbGuard, tcpiiu & iiu, // X aCC 431
        const caHdrLargeArray & hdr, void * /* pMsgBdy */ )
{
    nciu * pChan;

    {
        epicsGuard < cacMutex > guard ( this->mutex );
        pChan = this->chanTable.lookup ( hdr.m_cid );
        if ( pChan ) {
            unsigned sidTmp;
            if ( iiu.ca_v44_ok() ) {
                sidTmp = hdr.m_available;
            }
            else {
                sidTmp = pChan->getSID ();
            }
            pChan->connect ( hdr.m_dataType, hdr.m_count, sidTmp, iiu.ca_v41_ok() );
            this->connectAllIO ( guard, *pChan );
        }
        else if ( iiu.ca_v44_ok() ) {
            // this indicates a claim response for a resource that does
            // not exist in the client - so just remove it from the server
            iiu.clearChannelRequest ( guard, hdr.m_available, hdr.m_cid );
        }
    }
    // the callback lock is taken when a channel is unistalled or when
    // is disconnected to prevent race conditions here
    if ( pChan ) {
        pChan->connectStateNotify ( cbGuard );
    }
    return true; 
}

bool cac::verifyAndDisconnectChan ( 
              epicsGuard < callbackMutex > & cbGuard, tcpiiu & /* iiu */, 
              const caHdrLargeArray & hdr, void * /* pMsgBdy */ )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    nciu * pChan = this->chanTable.lookup ( hdr.m_cid );
    if ( ! pChan ) {
        return true;
    }
    this->disconnectChannel ( cbGuard, guard, *pChan );
    return true;
}

void cac::disconnectChannel (
        epicsGuard < callbackMutex > & cbGuard, // X aCC 431
        epicsGuard < cacMutex > & guard, nciu & chan )
{
    assert ( this->pudpiiu );
    this->disconnectAllIO ( guard, chan, true );
    chan.getPIIU()->uninstallChan ( guard, chan );
    chan.disconnect ( *this->pudpiiu );
    this->pudpiiu->installChannel ( chan );
    epicsGuardRelease < cacMutex > autoMutexRelease ( guard );
    chan.connectStateNotify ( cbGuard );
    chan.accessRightsNotify ( cbGuard );
}

bool cac::badTCPRespAction ( epicsGuard < callbackMutex > &, tcpiiu & iiu, 
                            const caHdrLargeArray & hdr, void * /* pMsgBdy */ )
{
    char hostName[64];
    iiu.hostName ( hostName, sizeof ( hostName ) );
    this->printf ( "CAC: Undecipherable TCP message ( bad response type %u ) from %s\n", 
        hdr.m_cmmd, hostName );
    return false;
}

bool cac::executeResponse ( epicsGuard < callbackMutex > & cbLocker, tcpiiu & iiu, 
                           caHdrLargeArray & hdr, char * pMshBody )
{
    // execute the response message
    pProtoStubTCP pStub;
    if ( hdr.m_cmmd >= NELEMENTS ( cac::tcpJumpTableCAC ) ) {
        pStub = &cac::badTCPRespAction;
    }
    else {
        pStub = cac::tcpJumpTableCAC [hdr.m_cmmd];
    }
    return ( this->*pStub ) ( cbLocker, iiu, hdr, pMshBody );
}

void cac::signal ( int ca_status, const char *pfilenm, 
                     int lineno, const char *pFormat, ... )
{
    va_list theArgs;
    va_start ( theArgs, pFormat );  
    this->vSignal ( ca_status, pfilenm, lineno, pFormat, theArgs);
    va_end ( theArgs );
}

void cac::vSignal ( int ca_status, const char *pfilenm, 
                     int lineno, const char *pFormat, va_list args )
{
    static const char *severity[] = 
    {
        "Warning",
        "Success",
        "Error",
        "Info",
        "Fatal",
        "Fatal",
        "Fatal",
        "Fatal"
    };
    
    this->printf ( "CA.Client.Exception...............................................\n" );
    
    this->printf ( "    %s: \"%s\"\n", 
        severity[ CA_EXTRACT_SEVERITY ( ca_status ) ], 
        ca_message ( ca_status ) );

    if  ( pFormat ) {
        this->printf ( "    Context: \"" );
        this->vPrintf ( pFormat, args );
        this->printf ( "\"\n" );
    }
        
    if ( pfilenm ) {
        this->printf ( "    Source File: %s line %d\n",
            pfilenm, lineno );    
    } 

    epicsTime current = epicsTime::getCurrent ();
    char date[64];
    current.strftime ( date, sizeof ( date ), "%a %b %d %Y %H:%M:%S.%f");
    this->printf ( "    Current Time: %s\n", date );
    
    /*
     *  Terminate execution if unsuccessful
     */
    if( ! ( ca_status & CA_M_SUCCESS ) && 
        CA_EXTRACT_SEVERITY ( ca_status ) != CA_K_WARNING ){
        errlogFlush ();
        abort ();
    }
    
    this->printf ( "..................................................................\n" );
}

void cac::selfTest () const
{
    this->chanTable.verify ();
    this->ioTable.verify ();
    this->sgTable.verify ();
    this->beaconTable.verify ();
}

void cac::notifyNewFD ( epicsGuard < callbackMutex > &, SOCKET sock ) const
{
    this->notify.fdWasCreated ( sock );
}

void cac::notifyDestroyFD ( epicsGuard < callbackMutex > &, SOCKET sock ) const
{
    this->notify.fdWasDestroyed ( sock );
}

void cac::disconnectNotify ( tcpiiu & iiu )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    iiu.disconnectNotify ( guard );
}

void cac::initiateAbortShutdown ( tcpiiu & iiu )
{
    // generate some NOOP UDP traffic so that ca_pend_event()
    // will get called in preemptive callback disabled 
    // applications, and therefore the callback lock below
    // will not block
    if ( this->pudpiiu ) {
        this->pudpiiu->wakeupMsg ();
    }
    {
        epicsGuard < callbackMutex > cbGuard ( this->cbMutex );
        epicsGuard < cacMutex > guard ( this->mutex );
        iiu.initiateAbortShutdown ( cbGuard, guard );
    }
}

void cac::uninstallIIU ( tcpiiu & iiu )
{
    epicsGuard < callbackMutex > cbGuard ( this->cbMutex );
    this->privateUninstallIIU ( cbGuard, iiu );
}

void cac::privateUninstallIIU ( epicsGuard < callbackMutex > & cbGuard, tcpiiu & iiu )
{
    epicsGuard < cacMutex > guard ( this->mutex );
    if ( iiu.channelCount() ) {
        char hostNameTmp[64];
        iiu.hostName ( hostNameTmp, sizeof ( hostNameTmp ) );
        genLocalExcep ( cbGuard, *this, ECA_DISCONN, hostNameTmp );
    }
    osiSockAddr addr = iiu.getNetworkAddress();
    if ( addr.sa.sa_family == AF_INET ) {
        inetAddrID tmp ( addr.ia );
        bhe *pBHE = this->beaconTable.lookup ( tmp );
        if ( pBHE ) {
            pBHE->unregisterIIU ( iiu );
        }
    }
   
    assert ( this->pudpiiu );
    iiu.removeAllChannels ( cbGuard, guard, *this );

    this->serverTable.remove ( iiu );

    // signal iiu uninstal event so that cac can properly shut down
    this->iiuUninstall.signal();
}

double cac::beaconPeriod ( const nciu & chan ) const
{
    epicsGuard < cacMutex > guard ( this->mutex );
    const netiiu * pIIU = chan.getConstPIIU ();
    if ( pIIU ) {
        osiSockAddr addr = pIIU->getNetworkAddress ();
        if ( addr.sa.sa_family == AF_INET ) {
            inetAddrID tmp ( addr.ia );
            bhe *pBHE = this->beaconTable.lookup ( tmp );
            if ( pBHE ) {
                return pBHE->period ();
            }
        }
    }
    return - DBL_MAX;
}

void cac::initiateConnect ( nciu & chan )
{
    assert ( this->pudpiiu );
    this->pudpiiu->installChannel ( chan );
}

