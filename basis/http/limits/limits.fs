\ limits.fs - so many requests a client in so long (a fixed window each): a sign-in that can't be guessed at speed,
\ a site one client can't flood.
\   s" /login" 10 60 site rate-limit-under          \ ten tries a minute from an address; more: 429
\   5 3600 <rate-limit> constant resets  ...  email resets rate-ok? if ... then
\ Each node counts its own (a client's connections spread over them: up to as many times the count, at most).
IN: http.limits
USING: http accessors system ;

\ so many in so many seconds; each key's window's start (ms) and its count so far; the keys held before those whose
\ windows are over are looked for next (twice those held after the last look: a flood of keys costs each request
\ little, not a walk of them all)
TUPLE: rate-limiter  count seconds starts counts next-prune ;

<PRIVATE
1000 constant ms-per-s
10000 constant prune-at   \ keys held, at least, before those whose window is over are let go
: window ( limiter -- ms )  seconds>> ms-per-s * ;
: prune ( limiter -- )   \ the keys whose windows are over let go
    {: l | ks :}
    l starts>> keys to ks
    ks length 0 ?do
        i ks nth string {: ka kn :}
        ka kn l starts>> s-at drop  l window +  ms-now <= if
            ka kn l starts>> s-delete-at  ka kn l counts>> s-delete-at
        then
    loop ;
PRIVATE>

\ a limiter: so many in so many seconds, each key
: <rate-limit> ( count seconds -- limiter )  <map> <map> prune-at <rate-limiter> rescue-all ;

\ one more for the key (an address, a user, an email): within the limit?
: rate-ok? ( key n limiter -- flag )
    {: ka kn l | now c :}
    ms-now to now
    ka kn l starts>> s-at if  l window + now <=  else  true  then  if   \ (none, or its window over: a new one)
        l starts>> map-size l next-prune>> > if
            l prune  l l starts>> map-size 2 * prune-at max >>next-prune drop
        then
        now ka kn l starts>> s-at!  1 ka kn l counts>> s-at!  true exit
    then
    ka kn l counts>> s-at drop 1+ to c
    c l count>> 1+ min ka kn l counts>> s-at!
    c l count>> <= ;
: retry-after ( key n limiter -- seconds )   \ how long till its window is over
    {: ka kn l :}  ka kn l starts>> s-at 0= if  0 exit  then
    l window + ms-now - ms-per-s / 1+ 0 max ;

<PRIVATE
: limit ( req limiter -- continue? )
    {: r l :}
    r client-address {: a n :}
    a n l rate-ok? if  a dispose  true exit  then
    a n l retry-after s" {}" format {: s sn :}  s sn s" Retry-After" r header!  s dispose  a dispose
    s" Too Many Requests" 429 r respond  false ;
PRIVATE>

\ requests to the paths under the prefix ("/" all) so many in so many seconds from each client address (client-
\ address: http-trust-proxy behind a proxy); more answered 429 with Retry-After
: rate-limit-under ( prefix n count seconds router -- )
    {: pa pn count seconds rt :}  pa pn ['] limit  count seconds <rate-limit>  rt before-under-with ;
