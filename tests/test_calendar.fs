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

testing ISO 8601 / RFC 3339: written in UTC; read back - a date alone, a time, a fraction, Z or an offset
T{ 0 >iso-date s" 1970-01-01T00:00:00Z" s= -> true }T
T{ 1700000000 >iso-date s" 2023-11-14T22:13:20Z" s= -> true }T
T{ s" 2023-11-14T22:13:20Z" iso-date> -> 1700000000 true }T
T{ s" 2023-11-14" iso-date> -> 1699920000 true }T
T{ s" 2023-11-14T23:13:20+01:00" iso-date> -> 1700000000 true }T
T{ s" 2023-11-14T17:13:20-0500" iso-date> -> 1700000000 true }T
T{ s" 2023-11-14t22:13:20.123456z" iso-date> -> 1700000000 true }T
T{ s" 2023-11-14 22:13:20" iso-date> -> 1700000000 true }T
T{ s" 2023-13-01" iso-date> -> false }T
T{ s" 2023-11-14T22:13" iso-date> -> false }T
T{ s" 2023-11-14T22:13:20Q" iso-date> -> false }T
T{ s" not a date" iso-date> -> false }T
T{ s" 2023-11-14T22:13:20+01" iso-date> -> false }T   \ (an offset cut short: nothing read past the text's end)
T{ s" 2023-11-14T22:13:20+0" iso-date> -> false }T
T{ s" 2023-11-14T22:13:20+01:" iso-date> -> false }T
T{ 1700000000 >iso-date iso-date> -> 1700000000 true }T

test-summary
