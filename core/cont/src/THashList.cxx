// @(#)root/cont:$Id$
// Author: Fons Rademakers   10/08/95

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class THashList
\ingroup Containers
THashList implements a hybrid collection class consisting of a
hash table and a list to store TObject's. The hash table is used for
quick access and lookup of objects while the list allows the objects
to be ordered. The hash value is calculated using the value returned
by the TObject's Hash() function. Each class inheriting from TObject
can override Hash() as it sees fit.
*/

#include "THashList.h"
#include "THashTable.h"
#include "TClass.h"


ClassImp(THashList);

////////////////////////////////////////////////////////////////////////////////
/// Create a THashList object. Capacity is the initial hashtable capacity
/// (i.e. number of slots), by default kInitHashTableCapacity = 17, and
/// rehash is the value at which a rehash will be triggered. I.e. when the
/// average size of the linked lists at a slot becomes longer than rehash
/// then the hashtable will be resized and refilled to reduce the collision
/// rate to about 1. The higher the collision rate, i.e. the longer the
/// linked lists, the longer lookup will take. If rehash=0 the table will
/// NOT automatically be rehashed. Use Rehash() for manual rehashing.
///
/// WARNING !!!
/// If the name of an object in the HashList is modified, The hashlist
/// must be Rehashed

THashList::THashList(Int_t capacity, Int_t rehash)
{
   fTable = new THashTable(capacity, rehash);
}

////////////////////////////////////////////////////////////////////////////////
/// For backward compatibility only. Use other ctor.

THashList::THashList(TObject *, Int_t capacity, Int_t rehash)
{
   fTable = new THashTable(capacity, rehash);
}

////////////////////////////////////////////////////////////////////////////////
/// Delete a hashlist. Objects are not deleted unless the THashList is the
/// owner (set via SetOwner()).

THashList::~THashList()
{
   Clear();
   SafeDelete(fTable);
}

////////////////////////////////////////////////////////////////////////////////
/// Add object at the beginning of the list.

void THashList::AddFirst(TObject *obj)
{
   // follow similar pattern as THashList to avoid calling Hash functions
   // while holding the lock

   if (IsArgNull("AddFirst", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddFirstImpl(obj);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Add object at the beginning of the list and also store option.
/// Storing an option is useful when one wants to change the behaviour
/// of an object a little without having to create a complete new
/// copy of the object. This feature is used, for example, by the Draw()
/// method. It allows the same object to be drawn in different ways.

void THashList::AddFirst(TObject *obj, Option_t *opt)
{
   if (IsArgNull("AddFirst", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddFirstImpl(obj, opt);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Add object at the end of the list.

void THashList::AddLast(TObject *obj)
{
   if (IsArgNull("AddLast", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddLastImpl(obj);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Add object at the end of the list and also store option.
/// Storing an option is useful when one wants to change the behaviour
/// of an object a little without having to create a complete new
/// copy of the object. This feature is used, for example, by the Draw()
/// method. It allows the same object to be drawn in different ways.

void THashList::AddLast(TObject *obj, Option_t *opt)
{
   if (IsArgNull("AddLast", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddLastImpl(obj, opt);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Insert object before object before in the list.

void THashList::AddBefore(const TObject *before, TObject *obj)
{
   if (IsArgNull("AddBefore", before) || IsArgNull("AddBefore", obj)) return;

   ULong_t beforehash = before->Hash();
   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddBeforeImpl(before, obj);
      fTable->AddBeforeImpl(beforehash, hash, before, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Insert object before object before in the list.

void THashList::AddBefore(TObjLink *before, TObject *obj)
{
   if (!before) return;
   if (IsArgNull("AddBefore", before->GetObject()) || IsArgNull("AddBefore", obj)) return;

   ULong_t beforehash = before->GetObject()->Hash();
   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddBeforeImpl(before, obj);
      fTable->AddBeforeImpl(beforehash, hash, before->GetObject(), obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Insert object after object after in the list.

void THashList::AddAfter(const TObject *after, TObject *obj)
{
   if (IsArgNull("AddAfter", after) || IsArgNull("AddAfter", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddAfterImpl(after, obj);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Insert object after object after in the list.

void THashList::AddAfter(TObjLink *after, TObject *obj)
{
   if (!after) return;
   if (IsArgNull("AddAfter", after->GetObject()) || IsArgNull("AddAfter", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddAfterImpl(after, obj);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Insert object at location idx in the list.

void THashList::AddAt(TObject *obj, Int_t idx)
{
   if (IsArgNull("AddFirst", obj)) return;

   ULong_t hash = obj->CheckedHash();

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      TList::AddAtImpl(obj, idx);
      fTable->AddImpl(hash, obj);
   }

   if (rehash) {
      RehashIfNeeded(fTable->GetSize());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Return the average collision rate. The higher the number the longer
/// the linked lists in the hashtable, the slower the lookup. If the number
/// is high, or lookup noticeably too slow, perform a Rehash().

Float_t THashList::AverageCollisions() const
{
   R__COLLECTION_READ_LOCKGUARD();

   return fTable->AverageCollisions();
}

////////////////////////////////////////////////////////////////////////////////
/// Remove all objects from the list. Does not delete the objects unless
/// the THashList is the owner (set via SetOwner()).

void THashList::ClearImpl(Option_t *option, local_gc_t *gc)
{
   fTable->ClearImpl("nodelete", gc);  // clear table so not more lookups
   if (IsOwner())
      TList::DeleteImpl(option, gc);
   else
      TList::ClearImpl(option, gc);
}

void THashList::Clear(Option_t *option)
{
   local_gc_t gc;
   local_gc_t *gcp = nullptr;
   if (IsUsingRWLock()) {
      gc.reserve(GetEntries());
      gcp = &gc;
   }

   {
      R__COLLECTION_WRITE_LOCKGUARD();
      R__COLLECTION_WRITE_GUARD();
      ClearImpl(option, gcp);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Remove all objects from the list AND delete all heap based objects.
/// If option="slow" then keep list consistent during delete. This allows
/// recursive list operations during the delete (e.g. during the dtor
/// of an object in this list one can still access the list to search for
/// other not yet deleted objects).

void THashList::DeleteImpl(Option_t *option, local_gc_t *gc)
{
   Bool_t slow = option ? (!strcmp(option, "slow") ? kTRUE : kFALSE) : kFALSE;

   if (!slow) {
      fTable->ClearImpl("nodelete", gc);     // clear table so no more lookups
      TList::DeleteImpl(option, gc);         // this deletes the objects
   } else {
      TList removeDirectory; // need to deregister these from their directory

      while (fFirst) {
         auto tlk = fFirst;
         fFirst = fFirst->NextSP();
         fSize--;
         // remove object from table
         fTable->Remove(tlk->GetObject());

         // delete only heap objects
         auto obj = tlk->GetObject();
         // In case somebody else access it.
         tlk->SetObject(nullptr);
         if (obj && !obj->TestBit(kNotDeleted))
            Error("Delete", "A list is accessing an object (%p) already deleted (list name = %s)",
                  obj, GetName());
         else if (obj && obj->IsOnHeap())
            TCollection::GarbageCollect(obj, gc);
         else if (obj && obj->IsA()->GetDirectoryAutoAdd())
            removeDirectory.Add(obj);

         // tlk reference count goes down 1.
      }
      fFirst.reset();
      fLast.reset();
      fCache.reset();
      fSize  = 0;

      // These objects cannot expect to have a valid TDirectory anymore;
      // e.g. because *this is the TDirectory's list of objects. Even if
      // not, they are supposed to be deleted, so we can as well unregister
      // them from their directory, even if they are stack-based:
      TIter iRemDir(&removeDirectory);
      TObject* dirRem = 0;
      while ((dirRem = iRemDir())) {
            (*dirRem->IsA()->GetDirectoryAutoAdd())(dirRem, 0);
      }
      Changed();
   }
}

void THashList::Delete(Option_t *option)
{
   local_gc_t gc;
   local_gc_t *gcp = nullptr;
   if (IsUsingRWLock()) {
      gc.reserve(GetEntries());
      gcp = &gc;
   }

   {
      R__COLLECTION_WRITE_LOCKGUARD();
      R__COLLECTION_WRITE_GUARD();
      DeleteImpl(option, gcp);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Find object using its name. Uses the hash value returned by the
/// TString::Hash() after converting name to a TString.

TObject *THashList::FindObject(const char *name) const
{
   const ULong_t hash = ::Hash(name);

   R__COLLECTION_READ_LOCKGUARD();

   return fTable->FindObjectImpl(hash, name);
}

////////////////////////////////////////////////////////////////////////////////
/// Find object using its hash value (returned by its Hash() member).

TObject *THashList::FindObject(const TObject *obj) const
{
   const ULong_t hash = obj->Hash();

   R__COLLECTION_READ_LOCKGUARD();

   return fTable->FindObjectImpl(hash, obj);
}

////////////////////////////////////////////////////////////////////////////////
/// Return the THashTable's list (bucket) in which obj can be found based on
/// its hash; see THashTable::GetListForObject().

const TList *THashList::GetListForObject(const char *name) const
{
   const ULong_t hash = ::Hash(name);

   R__COLLECTION_READ_LOCKGUARD();

   return fTable->GetListForHashImpl(hash);
}

////////////////////////////////////////////////////////////////////////////////
/// Return the THashTable's list (bucket) in which obj can be found based on
/// its hash; see THashTable::GetListForObject().

const TList *THashList::GetListForObject(const TObject *obj) const
{
   const ULong_t hash = obj->Hash();

   R__COLLECTION_READ_LOCKGUARD();

   return fTable->GetListForHashImpl(hash);
}

////////////////////////////////////////////////////////////////////////////////
/// Remove object from this collection and recursively remove the object
/// from all other objects (and collections).
/// This function overrides TCollection::RecursiveRemove that calls
/// the Remove function. THashList::Remove cannot be called because
/// it uses the hash value of the hash table. This hash value
/// is not available anymore when RecursiveRemove is called from
/// the TObject destructor.

void THashList::RecursiveRemove(TObject *obj)
{
//    R__COLLECTION_CHECK_GLOBAL_LOCK();
   if (!obj) return;

   // It might not be safe to rely on TROOT::RecursiveRemove to take the readlock in case user code
   // is calling directly gROOT->GetListOfCleanups()->RecursiveRemove(...)
   // However this can become a significant bottleneck if there are a very large number of
   // TDirectory object.

   if (obj->HasInconsistentHash()) {
      R__COLLECTION_WRITE_LOCKGUARD();

      // Remove obj in the list itself
      TObject *object = TList::RemoveImpl(obj);
      if (object)
         fTable->RemoveSlow(object);

   } else {
      // this function handles the locking itself
      Remove(obj);
   }

   // TODO check here that the global lock is not held since this could lead to deadlocks

   R__COLLECTION_READ_LOCKGUARD_GLOBAL(ROOT::gCoreMutex);
   R__COLLECTION_READ_LOCKGUARD();

   // if the collection is empty now nothing else to do
   if (!fFirst.get()) {
      return;
   }

   // Scan again the list and invoke RecursiveRemove for all objects
   // We need to make sure to go through all the node even those
   // marked as empty by another thread (Eventhough we hold the
   // read lock if one of the call to RecursiveRemove request
   // the write lock then the read lock will be suspended and
   // another thread can modify the list; thanks to the shared_pointer
   // forward-and-backward links, our view of the list is still intact
   // but might contains node will nullptr payload)
   auto lnk  = fFirst;
   decltype(lnk) next;
   while (lnk.get()) {
      next = lnk->NextSP();
      TObject *ob = lnk->GetObject();
      if (ob && ob->TestBit(kNotDeleted)) {
         ob->RecursiveRemove(obj);
      }
      lnk = next;
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Rehash the hashlist. If the collision rate becomes too high (i.e.
/// the average size of the linked lists become too long) then lookup
/// efficiency decreases since relatively long lists have to be searched
/// every time. To improve performance rehash the hashtable. This resizes
/// the table to newCapacity slots and refills the table. Use
/// AverageCollisions() to check if you need to rehash.

void THashList::Rehash(Int_t newCapacity)
{
   R__COLLECTION_READ_LOCKGUARD_GLOBAL(ROOT::gCoreMutex);
   R__COLLECTION_WRITE_LOCKGUARD();

   fTable->RehashImpl(newCapacity);
}

void THashList::RehashIfNeeded(Int_t newCapacity)
{
   R__COLLECTION_READ_LOCKGUARD_GLOBAL(ROOT::gCoreMutex);
   R__COLLECTION_WRITE_LOCKGUARD();

   if (fTable->GetRehashLevel() && fTable->AverageCollisions() > fTable->GetRehashLevel()) {
      fTable->RehashImpl(newCapacity);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Remove object from the list.

TObject *THashList::Remove(TObject *obj)
{
   if (!obj) return 0;

   const ULong_t hash = obj->Hash();

   {
      // check first if object is present to avoid taking the write lock
      // unnecessarily
      R__COLLECTION_READ_LOCKGUARD();
      if (!fTable->FindObjectImpl(hash, obj)) return 0;
   }

   R__COLLECTION_WRITE_LOCKGUARD();
   TList::RemoveImpl(obj);
   return fTable->RemoveImpl(hash, obj);
}

////////////////////////////////////////////////////////////////////////////////
/// Remove object via its objlink from the list.

TObject *THashList::Remove(TObjLink *lnk)
{
   if (!lnk) return 0;

   TObject *obj = lnk->GetObject();

   const ULong_t hash = obj->Hash();

   // don't bother checking if object is in the collection since we already
   // have the link from the list

   R__COLLECTION_WRITE_LOCKGUARD();
   TList::RemoveImpl(lnk);
   return fTable->RemoveImpl(hash, obj);
}
