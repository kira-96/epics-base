
//
// $Id$
// Author Jeff Hill
//
// IO Blocked list class
// (for single threaded version of the server)
//

#include <stdio.h>

#include "casdef.h"
#include "osiMutexNOOP.h"


//
// ioBlocked::ioBlocked ()
//
ioBlocked::ioBlocked () :
	pList(NULL)
{
}

//
// ioBlocked::~ioBlocked ()
//
ioBlocked::~ioBlocked ()
{
}

//
// ioBlockedList::ioBlockedList ()
//
ioBlockedList::ioBlockedList () 
{
}
 
//
// ioBlockedList::~ioBlockedList ()
//
ioBlockedList::~ioBlockedList ()
{
    ioBlocked *pB;
    
    while ( (pB = this->get ()) ) {
        pB->pList = NULL;
    }
}
 
//
// ioBlockedList::signal ()
//
// works from a temporary list to avoid problems
// where the virtual function adds items to the
// list
//
void ioBlockedList::signal ()
{
    tsDLList<ioBlocked> tmp;
    ioBlocked *pB;
    
    //
    // move all of the items onto tmp
    //
    tmp.add(*this);
    
    while ( (pB = tmp.get ()) ) {
        pB->pList = NULL;
        pB->ioBlockedSignal ();
    }
}

//
// ioBlockedList::removeItemFromIOBLockedList ()
//
void ioBlockedList::removeItemFromIOBLockedList (ioBlocked &item)
{
    if (item.pList==this) {
        this->remove (item);
        item.pList = NULL;
    }
}

//
// ioBlockedList::addItemToIOBLockedList ()
//
void ioBlockedList::addItemToIOBLockedList (ioBlocked &item)
{
    if (item.pList==NULL) {
        this->add (item);
        item.pList = this;
    }
    else {
        assert (item.pList == this);
    }
}
 

