/*
//@HEADER
// ************************************************************************
// 
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
// 
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact  H. Carter Edwards (hcedwar@sandia.gov)
// 
// ************************************************************************
//@HEADER
*/

#include <Kokkos_Core.hpp>

namespace Kokkos {
namespace Experimental {
namespace Impl {

bool
SharedAllocationRecord< void , void >::
is_sane( SharedAllocationRecord< void , void > * arg_record )
{
  constexpr static SharedAllocationRecord * zero = 0 ;

  SharedAllocationRecord * const root = arg_record ? arg_record->m_root : 0 ;

  bool ok = root != 0 && root->m_count == 0 ;

  if ( ok ) {
    SharedAllocationRecord * root_next = 0 ;

    // Lock the list:
    while ( ( root_next = Kokkos::atomic_exchange( & root->m_next , zero ) ) == 0 );

    for ( SharedAllocationRecord * rec = root_next ; ok && rec != root ; rec = rec->m_next ) {
      const bool ok_non_null  = rec && rec->m_prev && ( rec == root || rec->m_next );
      const bool ok_root      = ok_non_null && rec->m_root == root ;
      const bool ok_prev_next = ok_non_null && ( rec->m_prev != root ? rec->m_prev->m_next == rec : root_next == rec );
      const bool ok_next_prev = ok_non_null && rec->m_next->m_prev == rec ;
      const bool ok_count     = ok_non_null && 0 <= rec->m_count ;

      ok = ok_root && ok_prev_next && ok_next_prev && ok_count ;

if ( ! ok ) {
  fprintf(stderr,"Kokkos::Experimental::Impl::SharedAllocationRecord failed is_sane: rec(0x%.12lx){ m_count(%d) m_root(0x%.12lx) m_next(0x%.12lx) m_prev(0x%.12lx) m_next->m_prev(0x%.12lx) m_prev->m_next(0x%.12lx) }\n"
        , reinterpret_cast< unsigned long >( rec )
        , rec->m_count
        , reinterpret_cast< unsigned long >( rec->m_root )
        , reinterpret_cast< unsigned long >( rec->m_next )
        , reinterpret_cast< unsigned long >( rec->m_prev )
        , reinterpret_cast< unsigned long >( rec->m_next->m_prev )
        , reinterpret_cast< unsigned long >( rec->m_prev != rec->m_root ? rec->m_prev->m_next : root_next )
        );
}

    }

    if ( zero != Kokkos::atomic_exchange( & root->m_next , root_next ) ) {
      Kokkos::Impl::throw_runtime_exception("Kokkos::Experimental::Impl::SharedAllocationRecord failed is_sane unlocking");
    }
  }

  return ok ; 
}


/**\brief  Construct and insert into 'arg_root' tracking set.
 *         use_count is zero.
 */
SharedAllocationRecord< void , void >::
SharedAllocationRecord( SharedAllocationRecord<void,void> * arg_root
                      , SharedAllocationHeader            * arg_alloc_ptr
                      , size_t                              arg_alloc_size
                      , SharedAllocationRecord< void , void >::function_type  arg_dealloc
                      )
  : m_alloc_ptr(  arg_alloc_ptr )
  , m_alloc_size( arg_alloc_size )
  , m_dealloc(    arg_dealloc )
  , m_root( arg_root )
  , m_prev( 0 )
  , m_next( 0 )
  , m_count( 0 )
{
  constexpr static SharedAllocationRecord * zero = 0 ;

  // Insert into the root double-linked list for tracking
  //
  // before:  arg_root->m_next == next ; next->m_prev == arg_root
  // after:   arg_root->m_next == this ; this->m_prev == arg_root ;
  //              this->m_next == next ; next->m_prev == this

  m_prev = m_root ;

  // Read root->m_next and lock by setting to zero
  while ( ( m_next = Kokkos::atomic_exchange( & m_root->m_next , zero ) ) == 0 );

  m_next->m_prev = this ;

  if ( zero != Kokkos::atomic_exchange( & m_root->m_next , this ) ) {
    Kokkos::Impl::throw_runtime_exception("Kokkos::Experimental::Impl::SharedAllocationRecord failed locking/unlocking");
  }
}

void
SharedAllocationRecord< void , void >::
increment( SharedAllocationRecord< void , void > * arg_record )
{
  const int old_count = Kokkos::atomic_fetch_add( & arg_record->m_count , 1 );

  if ( old_count < 0 ) { // Error
    Kokkos::Impl::throw_runtime_exception("Kokkos::Experimental::Impl::SharedAllocationRecord failed increment");
  }
}

SharedAllocationRecord< void , void > *
SharedAllocationRecord< void , void >::
decrement( SharedAllocationRecord< void , void > * arg_record )
{
  constexpr static SharedAllocationRecord * zero = 0 ;

  const int old_count = Kokkos::atomic_fetch_add( & arg_record->m_count , -1 );

  if ( old_count == 1 ) {

    // before:  arg_record->m_prev->m_next == arg_record  &&
    //          arg_record->m_next->m_prev == arg_record
    //
    // after:   arg_record->m_prev->m_next == arg_record->m_next  &&
    //          arg_record->m_next->m_prev == arg_record->m_prev

    SharedAllocationRecord * root_next = 0 ;

    // Lock the list:
    while ( ( root_next = Kokkos::atomic_exchange( & arg_record->m_root->m_next , 0 ) ) == 0 );

    arg_record->m_next->m_prev = arg_record->m_prev ;

    if ( root_next != arg_record ) {
      arg_record->m_prev->m_next = arg_record->m_next ;
    }
    else {
      // before:  arg_record->m_root == arg_record->m_prev
      // after:   arg_record->m_root == arg_record->m_next
      root_next = arg_record->m_next ; 
    }

    // Unlock the list:
    if ( zero != Kokkos::atomic_exchange( & arg_record->m_root->m_next , root_next ) ) {
      Kokkos::Impl::throw_runtime_exception("Kokkos::Experimental::Impl::SharedAllocationRecord failed decrement unlocking");
    }

    arg_record->m_next = 0 ;
    arg_record->m_prev = 0 ;

    function_type d = arg_record->m_dealloc ;
    (*d)( arg_record );
    arg_record = 0 ;
  }
  else if ( old_count < 1 ) { // Error
    Kokkos::Impl::throw_runtime_exception("Kokkos::Experimental::Impl::SharedAllocationRecord failed decrement count");
  }

  return arg_record ;
}

} /* namespace Impl */
} /* namespace Experimental */
} /* namespace Kokkos */

