
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
 *
 */

#include "server.h"
#include "casPVIIL.h"

//
// casPVListChan::casPVListChan()
//
casPVListChan::casPVListChan (const casCtx &ctx) :
        casChannelI(ctx)
{
        this->pv.installChannel(*this);
}

//
// casPVListChan::~casPVListChan()
//
casPVListChan::~casPVListChan()
{
        this->pv.removeChannel(*this);
        //
        // delete signal to PV occurs in
        // casChannelI::~casChannelI
        //
}

