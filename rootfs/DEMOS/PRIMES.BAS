REM PRIMES.BAS - the sieve of Eratosthenes
limit% = 200
DIM sieve%(limit%)
count% = 0
FOR i% = 2 TO limit%
  IF sieve%(i%) = 0 THEN
    PRINT i%;" ";
    count% += 1
    FOR j% = i% * i% TO limit% STEP i%
      sieve%(j%) = 1
    NEXT
  ENDIF
NEXT
PRINT
PRINT count%; " primes below "; limit%
