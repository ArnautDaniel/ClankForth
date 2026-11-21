\ calendar.fs - dates from the clock's seconds (UTC, the proleptic Gregorian calendar), and the web's form of them.
\   time-now >http-date ( -- s len )  "Sun, 06 Nov 1994 08:49:37 GMT"
\   s" Sun, 06 Nov 1994 08:49:37 GMT" http-date> ( -- seconds true )
IN: calendar

86400 constant seconds-per-day
3600 constant seconds-per-hour
60 constant seconds-per-minute

<PRIVATE
719468 constant epoch-shift       \ days from 0000-03-01 to 1970-01-01 (H. Hinnant's civil-date algorithms)
146097 constant days-per-era      \ 400 years
4 constant epoch-weekday          \ 1970-01-01 was a Thursday (0 Sunday)
: weekday-name ( n -- a n )  3 * s" SunMonTueWedThuFriSat" drop + 3 ;
: month-name ( n -- a n )  1- 3 * s" JanFebMarAprMayJunJulAugSepOctNovDec" drop + 3 ;
PRIVATE>

\ the date of a day counted from 1970-01-01
: days>date ( days -- year month day )
    {: days | z era doe yoe doy mp d m y :}
    days epoch-shift + to z
    z 0>= if  z  else  z days-per-era 1- -  then  days-per-era / to era
    z era days-per-era * - to doe
    doe  doe 1460 / -  doe 36524 / +  doe 146096 / -  365 / to yoe
    doe  365 yoe *  yoe 4 / +  yoe 100 / - -  to doy
    5 doy * 2 + 153 / to mp
    doy  153 mp * 2 + 5 / -  1+ to d
    mp 10 < if  mp 3 +  else  mp 9 -  then to m
    yoe era 400 * +  m 2 <= if  1+  then to y
    y m d ;

\ the day counted from 1970-01-01 of a date
: date>days ( year month day -- days )
    {: y m d | era yoe doy doe :}
    m 2 <= if  y 1-  else  y  then to y
    y 0>= if  y  else  y 399 -  then  400 / to era
    y era 400 * - to yoe
    m 2 > if  m 3 -  else  m 9 +  then  153 *  2 +  5 /  d +  1- to doy
    yoe 365 *  yoe 4 / +  yoe 100 / -  doy + to doe
    era days-per-era *  doe +  epoch-shift - ;

\ seconds since 1970 as the date and time (UTC); the weekday 0 for Sunday
: seconds>date ( s -- year month day hour minute second weekday )
    {: s | days rest :}
    s seconds-per-day /  s seconds-per-day mod 0< if  1-  then to days
    s days seconds-per-day * - to rest
    days days>date
    rest seconds-per-hour /  rest seconds-per-hour mod seconds-per-minute /  rest seconds-per-minute mod
    days epoch-weekday + 7 mod  dup 0< if  7 +  then ;

\ the date and time as seconds since 1970
: date>seconds ( year month day hour minute second -- s )
    {: y mo d h mi s :}  y mo d date>days seconds-per-day *  h seconds-per-hour * +  mi seconds-per-minute * +  s + ;

\ seconds since 1970 as HTTP has them (RFC 9110's IMF-fixdate): a new string
: >http-date ( s -- s len )
    seconds>date {: y mo d h mi s wd :}
    wd weekday-name  d  mo month-name  y h mi s  s" {s}, {02d} {s} {} {02d}:{02d}:{02d} GMT" format ;

\ an IMF-fixdate read back; false for text that isn't one
: http-date> ( a n -- s true | false )
    {: a n | mo :}
    n 29 <> if  false exit  then
    a 3 + c@ [char] , <>  a 26 + 3 s" GMT" s= 0= or  if  false exit  then
    13 1 do  i month-name  a 8 + 3  s= if  i to mo  then  loop
    mo 0= if  false exit  then
    a 12 + 4 string>number 0= if  drop false exit  then  {: y :}
    a 5 + 2 string>number 0= if  drop false exit  then  {: d :}
    a 17 + 2 string>number 0= if  drop false exit  then  {: h :}
    a 20 + 2 string>number 0= if  drop false exit  then  {: mi :}
    a 23 + 2 string>number 0= if  drop false exit  then  {: s :}
    y mo d h mi s date>seconds true ;
