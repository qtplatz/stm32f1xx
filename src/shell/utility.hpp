// Copyright (C) 2018 MS-Cheminformatics LLC
// Licence: CC BY-NC
// Author: Toshinobu Hondo, Ph.D.
// Contact: toshi.hondo@qtplatz.com
//

struct condition_waiter {
    size_t count;
    condition_waiter() : count( 0xffff ) {}
    template< typename functor >  bool operator()( functor condition ) {
        while ( --count && !condition() )
            ;
        return count != 0;
    }
};
