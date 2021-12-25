// @(#)root/cont:$Id$
// Author: Fons Rademakers   27/09/95

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class THashTable
\ingroup Containers
THashTable implements a hash table to store TObject's. The hash
value is calculated using the value returned by the TObject's
Hash() function. Each class inheriting from TObject can override
Hash() as it sees fit.

THashTable does not preserve the insertion order of the objects.
If the insertion order is important AND fast retrieval is needed
use THashList instead.
*/

#include "THashTable.h"
#include "TObjectTable.h"
#include "TList.h"
#include "TError.h"
#include "TROOT.h"

ClassImp(THashTable);

////////////////////////////////////////////////////////////////////////////////
/// Create a THashTable object. Capacity is the initial hashtable capacity
/// (i.e. number of slots), by default kInitHashTableCapacity = 17, and
/// rehashlevel is the value at which a rehash will be triggered. I.e. when
/// the average size of the linked lists at a slot becomes longer than
/// rehashlevel then the hashtable will be resized and refilled to reduce
/// the collision rate to about 1. The higher the collision rate, i.e. the
/// longer the linked lists, the longer lookup will take. If rehashlevel=0
/// the table will NOT automatically be rehashed. Use Rehash() for manual
/// rehashing.

THashTable::THashTable(Int_t capacity, Int_t rehashlevel)
{
   if (capacity < 0) {
      Warning("THashTable", "capacity (%d) < 0", capacity);
      capacity = TCollection::kInitHashTableCapacity;
   } else if (capacity == 0)
      capacity = TCollection::kInitHashTableCapacity;

   fSize = (Int_t)TMath::NextPrime(TMath::Max(capacity,(int)TCollection::kInitHashTableCapacity));
   fCont = new TList* [fSize];
   memset(fCont, 0, fSize*sizeof(TList*));

   fEntries   = 0;
   fUsedSlots = 0;
   if (rehashlevel < 2) rehashlevel = 0;
   fRehashLevel = rehashlevel;
}

////////////////////////////////////////////////////////////////////////////////
/// Delete a hashtable. Objects are not deleted unless the THashTable is the
/// owner (set via SetOwner()).

THashTable::~THashTable()
{
   if (fCont) Clear();
   delete [] fCont;
   fCont = 0;
   fSize = 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Helper function doing the actual add to the table give a slot and object.
/// This does not take any lock.

void THashTable::AddImpl(ULong_t hash, TObject *obj)
{
   const Int_t slot = hash % fSize;

   if (!fCont[slot]) {
      fCont[slot] = new TList;
      ++fUsedSlots;
   }
   fCont[slot]->Add(obj);
   ++fEntries;
}

////////////////////////////////////////////////////////////////////////////////
/// Add object to the hash table. Its position in the table will be
/// determined by the value returned by its Hash() function.

void THashTable::Add(TObject *obj)
{
   if (IsArgNull("Add", obj)) return;

   // avoid calling hash calculation while taking the lock
   const ULong_t hash = obj->CheckedHash();

   // make sure Rehash is not called while holding the lock
   bool rehash = false;

   {
      R__COLLECTION_WRITE_LOCKGUARD();

      AddImpl(hash,obj);
      rehash = fRehashLevel && AverageCollisions() > fRehashLevel;
   }

   if (rehash)
      RehashIfNeeded(fEntries);
}

////////////////////////////////////////////////////////////////////////////////
/// Add object to the hash table. Its position in the table will be
/// determined by the value returned by its Hash() function.
/// If and only if 'before' is in the same bucket as obj, obj is added
/// in front of 'before' within the bucket's list.

void THashTable::AddBeforeImpl(ULong_t beforehash, ULong_t hash, const TObject *before, TObject *obj)
{
   const Int_t slot = hash % fSize;
   const Int_t beforeslot = beforehash % fSize;
   if (!fCont[slot]) {
      fCont[slot] = new TList;
      fUsedSlots++;
   }
   if (beforeslot == slot) {
      fCont[slot]->AddBefore(before,obj);
   } else {
      fCont[slot]->Add(obj);
   }
   fEntries++;
}

void THashTable::AddBefore(const TObject *before, TObject *obj)
{
   if (IsArgNull("Add", before) || IsArgNull("Add", obj)) return;

   const ULong_t beforehash = before->Hash();
   const ULong_t hash = obj->CheckedHash();

   // make sure Rehash is not called while holding the lock
   bool rehash = false;

   {
      R__COLLECTION_WRITE_LOCKGUARD();

      AddBeforeImpl(beforehash, hash, before, obj);

      rehash = fRehashLevel && AverageCollisions() > fRehashLevel;
   }

   if (rehash)
      RehashIfNeeded(fEntries);
}

////////////////////////////////////////////////////////////////////////////////
/// Add all objects from collection col to this collection.
/// Implemented for more efficient rehashing.

void THashTable::AddAll(const TCollection *col)
{

   bool rehashBefore = false;
   {
      R__COLLECTION_READ_LOCKGUARD();

      // Hashing after AddAll can be much more expensive than
      // hashing before, as we need to add more elements.
      // We assume an ideal hash, i.e. fUsedSlots==fSize.
      Int_t sumEntries=fEntries+col->GetEntries();
      rehashBefore=fRehashLevel && (sumEntries > fSize*fRehashLevel);
   }

   if (rehashBefore) {
      R__COLLECTION_READ_LOCKGUARD_GLOBAL(ROOT::gCoreMutex);
      R__COLLECTION_WRITE_LOCKGUARD();

      // check again if rehashing is needed in case another thread did it meanwhile
      Int_t sumEntries=fEntries+col->GetEntries();
      rehashBefore = fRehashLevel && (sumEntries > fSize*fRehashLevel);
      if (rehashBefore) {
         RehashImpl(sumEntries);
      }
   }

   bool rehash = false;
   {
      R__COLLECTION_WRITE_LOCKGUARD();

      // prevent Add from Rehashing
      Int_t saveRehashLevel=fRehashLevel;
      fRehashLevel=0;

      TCollection::AddAll(col);

      fRehashLevel=saveRehashLevel;

      rehash = !rehashBefore && fRehashLevel && AverageCollisions() > fRehashLevel;
   }

   // If we didn't Rehash before, we might have to do it
   // now, due to a non-perfect hash function.
   if (rehash)
      RehashIfNeeded(fEntries);
}

////////////////////////////////////////////////////////////////////////////////
/// Remove all objects from the table. Does not delete the objects
/// unless the THashTable is the owner (set via SetOwner()).

void THashTable::ClearImpl(Option_t *option, local_gc_t *gc)
{
   for (int i = 0; i < fSize; i++) {
      // option "nodelete" is passed when Clear is called from
      // THashList::Clear() or THashList::Delete() or Rehash().
      if (fCont[i]) {
         if (IsOwner())
            fCont[i]->SetOwner();
         fCont[i]->ClearImpl(option, gc);
      }
      SafeDelete(fCont[i]);
   }

   fEntries   = 0;
   fUsedSlots = 0;
}

void THashTable::Clear(Option_t *option)
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
/// Returns the number of collisions for an object with a certain name
/// (i.e. number of objects in same slot in the hash table, i.e. length
/// of linked list).

Int_t THashTable::Collisions(const char *name) const
{
   const ULong_t hash = ::Hash(name);

   R__COLLECTION_READ_LOCKGUARD();

   const Int_t slot = hash % fSize;

   if (fCont[slot]) return fCont[slot]->GetSize();
   return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Returns the number of collisions for an object (i.e. number of objects
/// in same slot in the hash table, i.e. length of linked list).

Int_t THashTable::Collisions(TObject *obj) const
{
   if (IsArgNull("Collisions", obj)) return 0;

   const ULong_t hash = obj->Hash();

   R__COLLECTION_READ_LOCKGUARD();

   const Int_t slot = hash % fSize;

   if (fCont[slot]) return fCont[slot]->GetSize();
   return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Remove all objects from the table AND delete all heap based objects.

void THashTable::DeleteImpl(Option_t *, local_gc_t *gc)
{
   for (int i = 0; i < fSize; i++)
      if (fCont[i]) {
         fCont[i]->DeleteImpl("", gc);
         SafeDelete(fCont[i]);
      }

   fEntries   = 0;
   fUsedSlots = 0;
}

void THashTable::Delete(Option_t *option)
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

TObject *THashTable::FindObjectImpl(ULong_t hash, const char *name) const
{
   const Int_t slot = hash % fSize;

   if (fCont[slot]) return fCont[slot]->FindObject(name);
   return 0;
}

TObject *THashTable::FindObject(const char *name) const
{
   const ULong_t hash = ::Hash(name);

   R__COLLECTION_READ_LOCKGUARD();

   return FindObjectImpl(hash, name);
}

////////////////////////////////////////////////////////////////////////////////
/// Find object using its hash value (returned by its Hash() member).

TObject *THashTable::FindObjectImpl(ULong_t hash, const TObject *obj) const
{
   const Int_t slot = hash % fSize;

   if (fCont[slot]) return fCont[slot]->FindObject(obj);
   return 0;
}

TObject *THashTable::FindObject(const TObject *obj) const
{
   if (IsArgNull("FindObject", obj)) return 0;

   const ULong_t hash = obj->Hash();

   R__COLLECTION_READ_LOCKGUARD();

   return FindObjectImpl(hash, obj);
}

////////////////////////////////////////////////////////////////////////////////
/// Return the TList corresponding to object's name based hash value.
/// One can iterate this list "manually" to find, e.g. objects with
/// the same name.

const TList *THashTable::GetListForObject(const char *name) const
{
   const ULong_t hash = ::Hash(name);

   R__COLLECTION_READ_LOCKGUARD();

   const Int_t slot = hash % fSize;

   return fCont[slot];
}

////////////////////////////////////////////////////////////////////////////////
/// Return the TList corresponding to object's hash value.
/// One can iterate this list "manually" to find, e.g. identical
/// objects.

const TList *THashTable::GetListForObject(const TObject *obj) const
{
   if (IsArgNull("GetListForObject", obj)) return 0;

   const ULong_t hash = obj->Hash();

   R__COLLECTION_READ_LOCKGUARD();

   const Int_t slot = hash % fSize;

   return fCont[slot];
}

////////////////////////////////////////////////////////////////////////////////
/// Return address of pointer to obj

TObject **THashTable::GetObjectRef(const TObject *obj) const
{
   if (IsArgNull("GetObjectRef", obj)) return 0;

   const ULong_t hash = obj->Hash();

   R__COLLECTION_READ_LOCKGUARD();

   const Int_t slot = hash % fSize;

   if (fCont[slot]) return fCont[slot]->GetObjectRef(obj);
   return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Returns a hash table iterator.

TIterator *THashTable::MakeIterator(Bool_t dir) const
{
   return new THashTableIter(this, dir);
}

////////////////////////////////////////////////////////////////////////////
/// Print the collection header and its elements.
///
/// If recurse is non-zero, descend into printing of
/// collection-entries with recurse - 1.
/// This means, if recurse is negative, the recursion is infinite.
///
/// If option contains "details", Print will show the content of
/// each of the hash-slots.
///
/// Option is passed recursively.

void THashTable::Print(Option_t *option, Int_t recurse) const
{
   if (strstr(option,"details")==nullptr) {
      TCollection::Print(option,recurse);
      return;
   }

   PrintCollectionHeader(option);

   if (recurse != 0)
   {
      TROOT::IncreaseDirLevel();
      for (Int_t cursor = 0; cursor < Capacity();
           cursor++) {
         printf("Slot #%d:\n",cursor);
         if (fCont[cursor])
            fCont[cursor]->Print();
         else {
            TROOT::IndentLevel();
            printf("empty\n");
         }

      }
      TROOT::DecreaseDirLevel();
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Rehash the hashtable. If the collision rate becomes too high (i.e.
/// the average size of the linked lists become too long) then lookup
/// efficiency decreases since relatively long lists have to be searched
/// every time. To improve performance rehash the hashtable. This resizes
/// the table to newCapacity slots and refills the table. Use
/// AverageCollisions() to check if you need to rehash. Set checkObjValidity
/// to kFALSE if you know that all objects in the table are still valid
/// (i.e. have not been deleted from the system in the meanwhile).

void THashTable::RehashImpl(Int_t newCapacity, Bool_t checkObjValidity)
{
   THashTable *ht = new THashTable(newCapacity);

   TIter next(this);
   TObject *obj;

   auto initialSize = GetEntries();

   if (checkObjValidity && TObject::GetObjectStat() && gObjectTable) {
      while ((obj = next()))
         if (gObjectTable->PtrIsValid(obj))
            ht->AddImpl(ht->GetHashValue(obj),obj);
   } else {
      while ((obj = next()))
         ht->AddImpl(ht->GetHashValue(obj),obj);
   }

   if (initialSize != GetEntries()) {
      // Somehow in the process of copy the pointer from one hash to
      // other we ended up inducing the addition of more element to
      // the table.  Most likely those elements have not been copied ....
      // i.e. Adding *during* the Rehashing is illegal and fatal.

      Fatal("Rehash",
            "During the rehash of %p one or more element was added or removed. The initalize size was %d and now it is %d",
            this, initialSize, GetEntries());

   }

   Clear("nodelete");
   delete [] fCont;
   fCont = ht->fCont;
   ht->fCont = 0;

   fSize      = ht->fSize;     // idem
   fEntries   = ht->fEntries;
   fUsedSlots = ht->fUsedSlots;

   // this should not happen, but it will prevent an endless loop
   // in case of a very bad hash function
   if (fRehashLevel && AverageCollisions() > fRehashLevel)
      fRehashLevel = (int)AverageCollisions() + 1;

   delete ht;
}

void THashTable::Rehash(Int_t newCapacity, Bool_t checkObjValidity)
{
   // objects may implement arbitrary code for the hash functions, so take the global
   // lock here to prevent deadlocks

   R__COLLECTION_READ_LOCKGUARD_GLOBAL(ROOT::gCoreMutex);
   R__COLLECTION_WRITE_LOCKGUARD();
   RehashImpl(newCapacity, checkObjValidity);
}

void THashTable::RehashIfNeeded(Int_t newCapacity, Bool_t checkObjValidity)
{
   // objects may implement arbitrary code for the hash functions, so take the global
   // lock here to prevent deadlocks
   // This version of the function is called on demand, so check again
   // if rehashing is still needed after taking the locks

   R__COLLECTION_READ_LOCKGUARD_GLOBAL(ROOT::gCoreMutex);
   R__COLLECTION_WRITE_LOCKGUARD();

   if (fRehashLevel && AverageCollisions() > fRehashLevel) {
      RehashImpl(newCapacity, checkObjValidity);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Remove object from the hashtable.

TObject *THashTable::RemoveImpl(ULong_t hash, TObject *obj)
{
   const Int_t slot = hash % fSize;

   if (fCont[slot]) {
      TObject *ob = fCont[slot]->Remove(obj);
      if (ob) {
         fEntries--;
         if (fCont[slot]->GetSize() == 0) {
            // calling the destructor while having the lock is in general unsafe,
            // should be ok here because the list is empty and therefore
            // no arbitrary object destructors can be called
            SafeDelete(fCont[slot]);
            fUsedSlots--;
         }
         return ob;
      }
   }
   return 0;
}

TObject *THashTable::Remove(TObject *obj)
{
   const ULong_t hash = obj->Hash();

   R__COLLECTION_WRITE_LOCKGUARD();

   return RemoveImpl(hash, obj);
}

////////////////////////////////////////////////////////////////////////////////
/// Remove object from the hashtable without using the hash value.

TObject *THashTable::RemoveSlow(TObject *obj)
{
   R__COLLECTION_WRITE_LOCKGUARD();

   for (int i = 0; i < fSize; i++) {
      if (fCont[i]) {
         TObject *ob = fCont[i]->Remove(obj);
         if (ob) {
            fEntries--;
            if (fCont[i]->GetSize() == 0) {
              // calling the destructor while having the lock is in general unsafe,
              // should be ok here because the list is empty and therefore
              // no arbitrary object destructors can be called
               SafeDelete(fCont[i]);
               fUsedSlots--;
            }
            return ob;
         }
      }
   }
   return 0;
}

/** \class THashTableIter
Iterator of hash table.
*/

ClassImp(THashTableIter);

////////////////////////////////////////////////////////////////////////////////
/// Create a hashtable iterator. By default the iteration direction
/// is kIterForward. To go backward use kIterBackward.

THashTableIter::THashTableIter(const THashTable *ht, Bool_t dir)
{
   fTable      = ht;
   fDirection  = dir;
   fListCursor = 0;
   Reset();
}

////////////////////////////////////////////////////////////////////////////////
/// Copy ctor.

THashTableIter::THashTableIter(const THashTableIter &iter) : TIterator(iter)
{
   fTable      = iter.fTable;
   fDirection  = iter.fDirection;
   fCursor     = iter.fCursor;
   fListCursor = 0;
   if (iter.fListCursor) {
      fListCursor = (TListIter *)iter.fListCursor->GetCollection()->MakeIterator();
      if (fListCursor)
         fListCursor->operator=(*iter.fListCursor);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Overridden assignment operator.

TIterator &THashTableIter::operator=(const TIterator &rhs)
{
   if (this != &rhs && rhs.IsA() == THashTableIter::Class()) {
      const THashTableIter &rhs1 = (const THashTableIter &)rhs;
      fTable     = rhs1.fTable;
      fDirection = rhs1.fDirection;
      fCursor    = rhs1.fCursor;
      if (rhs1.fListCursor) {
         // R__COLLECTION_READ_LOCKGUARD();

         fListCursor = (TListIter *)rhs1.fListCursor->GetCollection()->MakeIterator();
         if (fListCursor)
            fListCursor->operator=(*rhs1.fListCursor);
      }
   }
   return *this;
}

////////////////////////////////////////////////////////////////////////////////
/// Overloaded assignment operator.

THashTableIter &THashTableIter::operator=(const THashTableIter &rhs)
{
   if (this != &rhs) {
      fTable     = rhs.fTable;
      fDirection = rhs.fDirection;
      fCursor    = rhs.fCursor;
      if (rhs.fListCursor) {
         // R__COLLECTION_READ_LOCKGUARD();

         fListCursor = (TListIter *)rhs.fListCursor->GetCollection()->MakeIterator();
         if (fListCursor)
            fListCursor->operator=(*rhs.fListCursor);
      }
   }
   return *this;
}

////////////////////////////////////////////////////////////////////////////////
/// Delete hashtable iterator.

THashTableIter::~THashTableIter()
{
   delete fListCursor;
}

////////////////////////////////////////////////////////////////////////////////
/// Return next object in hashtable. Returns 0 when no more objects in table.

TObject *THashTableIter::Next()
{
   // R__COLLECTION_READ_LOCKGUARD();

   while (kTRUE) {
      if (!fListCursor) {
         int slot = NextSlot();
         if (slot == -1) return 0;
         fListCursor = new TListIter(fTable->fCont[slot], fDirection);
      }

      TObject *obj = fListCursor->Next();
      if (obj) return obj;

      SafeDelete(fListCursor);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Returns index of next slot in table containing list to be iterated.

Int_t THashTableIter::NextSlot()
{
   // R__COLLECTION_READ_LOCKGUARD();

   if (fDirection == kIterForward) {
      for ( ; fCursor < fTable->Capacity() && fTable->fCont[fCursor] == 0;
              fCursor++) { }

      if (fCursor < fTable->Capacity())
         return fCursor++;

   } else {
      for ( ; fCursor >= 0 && fTable->fCont[fCursor] == 0;
              fCursor--) { }

      if (fCursor >= 0)
         return fCursor--;
   }
   return -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Reset the hashtable iterator. Either to beginning or end, depending on
/// the initial iteration direction.

void THashTableIter::Reset()
{
   if (fDirection == kIterForward)
      fCursor = 0;
   else
      fCursor = fTable->Capacity() - 1;
   SafeDelete(fListCursor);
}

////////////////////////////////////////////////////////////////////////////////
/// This operator compares two TIterator objects.

Bool_t THashTableIter::operator!=(const TIterator &aIter) const
{
   if (aIter.IsA() == THashTableIter::Class()) {
      const THashTableIter &iter(dynamic_cast<const THashTableIter &>(aIter));
      return (fListCursor != iter.fListCursor);
   }
   return false; // for base class we don't implement a comparison
}

////////////////////////////////////////////////////////////////////////////////
/// This operator compares two THashTableIter objects.

Bool_t THashTableIter::operator!=(const THashTableIter &aIter) const
{
   return (fListCursor != aIter.fListCursor);
}

////////////////////////////////////////////////////////////////////////////////
/// Return pointer to current object or nullptr.

TObject *THashTableIter::operator*() const
{
   return (fListCursor ? fListCursor->operator*() : nullptr);
}
