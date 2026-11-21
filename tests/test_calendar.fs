\ calendar (basis/calendar): seconds since 1970 as dates and back, and HTTP's form of them
IN: test-calendar
USING: tester calendar ;

: is ( s len a n -- flag )  {: s sn a n :}  s sn a n s=  s dispose ;
testing days and dates: the epoch, leap days, before it
T{ 0 days>date -> 1970 1 1 }T
T{ 2000 2 29 date>days days>date -> 2000 2 29 }T
T{ 1969 12 31 date>days -> -1 }T
T{ 2024 3 1 date>days  2024 2 28 date>days - -> 2 }T
T{ 1900 3 1 date>days  1900 2 28 date>days - -> 1 }T
testing seconds as a date and time, with its weekday
T{ 0 seconds>date -> 1970 1 1 0 0 0 4 }T
T{ 784111777 seconds>date -> 1994 11 6 8 49 37 0 }T
T{ 1994 11 6 8 49 37 date>seconds -> 784111777 }T
T{ -1 seconds>date -> 1969 12 31 23 59 59 3 }T
testing HTTP dates: RFC 9110's example, both ways; what isn't one refused
T{ 784111777 >http-date s" Sun, 06 Nov 1994 08:49:37 GMT" is -> true }T
T{ s" Sun, 06 Nov 1994 08:49:37 GMT" http-date> -> 784111777 true }T
T{ s" Sunday, 06-Nov-94 08:49:37 GMT" http-date> -> false }T
T{ s" Sun, 06 Nox 1994 08:49:37 GMT" http-date> -> false }T

test-summary
